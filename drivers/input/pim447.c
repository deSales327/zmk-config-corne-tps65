/*
 * Pimoroni Trackball Breakout (PIM447) — driver de input para Zephyr/ZMK
 *
 * - Lê as contagens esquerda/direita/cima/baixo e o clique por I2C (0x0A).
 * - Pino INT (ativo em LOW) opcional; sem ele, lê periodicamente.
 * - Modo cursor (INPUT_REL_X/Y) ou scroll (INPUT_REL_WHEEL/HWHEEL).
 * - Com `click-toggles-scroll`, o clique da bola alterna cursor <-> scroll
 *   (e o LED pisca: azul = cursor, verde = scroll). Sem essa opção, o clique
 *   é enviado como botão esquerdo (INPUT_BTN_0).
 * - Dorme quando o ZMK entra em sleep/idle (registo CTRL) e apaga o LED.
 *
 * Mapa de registos conforme a biblioteca oficial da Pimoroni (trackball-python).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT pimoroni_pim447

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

#include "pim447.h"

LOG_MODULE_REGISTER(pim447, CONFIG_INPUT_LOG_LEVEL);

#define REG_LED_RED 0x00
#define REG_LEFT 0x04
#define REG_SWITCH 0x08
#define MSK_SWITCH_STATE 0x80
#define REG_INT 0xF9
#define MSK_INT_TRIGGERED 0x01
#define MSK_INT_OUT_EN 0x02
#define REG_CHIP_ID_L 0xFA
#define REG_CTRL 0xFE
#define MSK_CTRL_SLEEP 0x01
#define CHIP_ID 0xBA11

struct pim447_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec int_gpio;
    uint16_t poll_ms;
    uint8_t cursor_mult;
    uint8_t accel;
    uint8_t scroll_div;
    bool click_toggles_scroll;
    bool led_feedback;
    uint8_t led_brightness;
};

struct pim447_data {
    const struct device *dev;
    struct gpio_callback int_cb;
    struct k_work work;
    struct k_work_delayable poll_work;
    struct k_work_delayable led_off_work;
    bool scroll_mode;
    bool pressed;
    bool sleeping;
    int scroll_acc_x, scroll_acc_y;
};

static const struct device *pim447_first_dev;

/* ------------------------------------------------------------------------- */

static int reg_write(const struct device *dev, uint8_t reg, uint8_t val) {
    const struct pim447_config *cfg = dev->config;
    uint8_t buf[2] = {reg, val};
    return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

static int reg_read(const struct device *dev, uint8_t reg, uint8_t *out, size_t len) {
    const struct pim447_config *cfg = dev->config;
    return i2c_write_read_dt(&cfg->i2c, &reg, 1, out, len);
}

static void set_led(const struct device *dev, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    const struct pim447_config *cfg = dev->config;
    uint8_t buf[5] = {REG_LED_RED, r, g, b, w};
    i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

static void led_off_handler(struct k_work *work) {
    struct k_work_delayable *dw = k_work_delayable_from_work(work);
    struct pim447_data *data = CONTAINER_OF(dw, struct pim447_data, led_off_work);
    set_led(data->dev, 0, 0, 0, 0);
}

static void flash_mode(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;
    if (!cfg->led_feedback) {
        return;
    }
    uint8_t v = cfg->led_brightness;
    if (data->scroll_mode) {
        set_led(dev, 0, v, 0, 0);
    } else {
        set_led(dev, 0, 0, v, 0);
    }
    k_work_reschedule(&data->led_off_work, K_MSEC(400));
}

/* aceleração simples: d * mult * (2 + accel*|d|) / 2  */
static int32_t accelerate(const struct pim447_config *cfg, int32_t d) {
    if (d == 0) {
        return 0;
    }
    int32_t a = abs(d);
    return d * cfg->cursor_mult * (2 + cfg->accel * (a - 1)) / 2;
}

static void process(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;
    uint8_t b[5];

    if (data->sleeping) {
        return;
    }
    if (reg_read(dev, REG_LEFT, b, sizeof(b)) != 0) {
        LOG_DBG("read failed");
        return;
    }

    int32_t dx = (int32_t)b[1] - (int32_t)b[0]; /* direita - esquerda */
    int32_t dy = (int32_t)b[3] - (int32_t)b[2]; /* baixo - cima */
    bool pressed = (b[4] & MSK_SWITCH_STATE) != 0;

    if (pressed != data->pressed) {
        data->pressed = pressed;
        if (cfg->click_toggles_scroll) {
            if (pressed) {
                data->scroll_mode = !data->scroll_mode;
                data->scroll_acc_x = data->scroll_acc_y = 0;
                LOG_INF("mode: %s", data->scroll_mode ? "scroll" : "cursor");
                flash_mode(dev);
            }
        } else {
            input_report_key(dev, INPUT_BTN_0, pressed ? 1 : 0, true, K_FOREVER);
        }
    }

    if (dx == 0 && dy == 0) {
        return;
    }

    if (data->scroll_mode) {
        data->scroll_acc_x += dx;
        data->scroll_acc_y += dy;
        int32_t sx = data->scroll_acc_x / cfg->scroll_div;
        int32_t sy = data->scroll_acc_y / cfg->scroll_div;
        data->scroll_acc_x -= sx * cfg->scroll_div;
        data->scroll_acc_y -= sy * cfg->scroll_div;
        if (sx != 0 || sy != 0) {
            /* bola para cima = conteúdo sobe (scroll para cima) */
            input_report_rel(dev, INPUT_REL_HWHEEL, sx, false, K_FOREVER);
            input_report_rel(dev, INPUT_REL_WHEEL, -sy, true, K_FOREVER);
        }
    } else {
        input_report_rel(dev, INPUT_REL_X, accelerate(cfg, dx), false, K_FOREVER);
        input_report_rel(dev, INPUT_REL_Y, accelerate(cfg, dy), true, K_FOREVER);
    }
}

static void work_handler(struct k_work *work) {
    struct pim447_data *data = CONTAINER_OF(work, struct pim447_data, work);
    const struct device *dev = data->dev;
    const struct pim447_config *cfg = dev->config;

    process(dev);

    /* se o INT continua ativo (perdemos um flanco), volta a ler */
    if (cfg->int_gpio.port != NULL && !data->sleeping && gpio_pin_get_dt(&cfg->int_gpio) > 0) {
        k_work_submit(&data->work);
    }
}

static void poll_handler(struct k_work *work) {
    struct k_work_delayable *dw = k_work_delayable_from_work(work);
    struct pim447_data *data = CONTAINER_OF(dw, struct pim447_data, poll_work);
    const struct pim447_config *cfg = data->dev->config;

    if (!data->sleeping) {
        process(data->dev);
    }
    k_work_reschedule(&data->poll_work, K_MSEC(cfg->poll_ms));
}

static void int_cb(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    struct pim447_data *data = CONTAINER_OF(cb, struct pim447_data, int_cb);
    k_work_submit(&data->work);
}

/* ------------------------------------------------------------------------- */
/* API pública (pim447.h)                                                     */

int pim447_set_sleep(const struct device *dev, bool sleep) {
    struct pim447_data *data = dev->data;
    uint8_t ctrl = 0;
    int ret = reg_read(dev, REG_CTRL, &ctrl, 1);
    if (ret == 0) {
        ctrl = sleep ? (ctrl | MSK_CTRL_SLEEP) : (ctrl & ~MSK_CTRL_SLEEP);
        ret = reg_write(dev, REG_CTRL, ctrl);
    }
    data->sleeping = sleep;
    if (sleep) {
        set_led(dev, 0, 0, 0, 0);
    } else {
        /* limpa contagens acumuladas durante o sono */
        k_work_submit(&data->work);
    }
    return ret;
}

bool pim447_is_scroll_mode(void) {
    if (pim447_first_dev == NULL) {
        return false;
    }
    struct pim447_data *data = pim447_first_dev->data;
    return data->scroll_mode;
}

void pim447_toggle_scroll_mode(void) {
    if (pim447_first_dev == NULL) {
        return;
    }
    struct pim447_data *data = pim447_first_dev->data;
    data->scroll_mode = !data->scroll_mode;
    flash_mode(pim447_first_dev);
}

/* ------------------------------------------------------------------------- */

static int pim447_init(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;
    uint8_t id[2];

    data->dev = dev;
    k_work_init(&data->work, work_handler);
    k_work_init_delayable(&data->poll_work, poll_handler);
    k_work_init_delayable(&data->led_off_work, led_off_handler);

    if (!i2c_is_ready_dt(&cfg->i2c)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    /* o PIM447 tem o seu próprio MCU; dá-lhe tempo para arrancar */
    int ret = -EIO;
    for (int i = 0; i < 20 && ret != 0; i++) {
        ret = reg_read(dev, REG_CHIP_ID_L, id, 2);
        if (ret != 0) {
            k_msleep(25);
        }
    }
    if (ret != 0) {
        LOG_ERR("no response at 0x%02x", cfg->i2c.addr);
        return -EIO;
    }
    uint16_t chip = id[0] | (id[1] << 8);
    if (chip != CHIP_ID) {
        LOG_ERR("unexpected chip id 0x%04x", chip);
        return -ENODEV;
    }

    set_led(dev, 0, 0, 0, 0);
    reg_write(dev, REG_CTRL, 0); /* acordado */

    if (pim447_first_dev == NULL) {
        pim447_first_dev = dev;
    }

    if (cfg->int_gpio.port != NULL) {
        uint8_t intreg = 0;
        reg_read(dev, REG_INT, &intreg, 1);
        reg_write(dev, REG_INT, intreg | MSK_INT_OUT_EN);

        ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
        if (ret == 0) {
            gpio_init_callback(&data->int_cb, int_cb, BIT(cfg->int_gpio.pin));
            ret = gpio_add_callback(cfg->int_gpio.port, &data->int_cb);
        }
        if (ret == 0) {
            ret = gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
        }
        if (ret != 0) {
            LOG_WRN("INT setup failed (%d), falling back to polling", ret);
            k_work_reschedule(&data->poll_work, K_MSEC(cfg->poll_ms));
        } else {
            /* lição do trackpad: se já está ativo, não haverá flanco */
            k_work_submit(&data->work);
        }
    } else {
        k_work_reschedule(&data->poll_work, K_MSEC(cfg->poll_ms));
    }

    LOG_INF("PIM447 ready (%s, %s)", cfg->int_gpio.port ? "INT" : "polling",
            cfg->click_toggles_scroll ? "click toggles scroll" : "click = BTN_0");
    return 0;
}

#define PIM447_DEFINE(n)                                                                           \
    static struct pim447_data pim447_data_##n;                                                     \
    static const struct pim447_config pim447_config_##n = {                                        \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                            \
        .int_gpio = GPIO_DT_SPEC_INST_GET_OR(n, int_gpios, {0}),                                   \
        .poll_ms = DT_INST_PROP(n, poll_interval_ms),                                              \
        .cursor_mult = DT_INST_PROP(n, cursor_multiplier),                                         \
        .accel = DT_INST_PROP(n, acceleration),                                                    \
        .scroll_div = MAX(1, DT_INST_PROP(n, scroll_divisor)),                                     \
        .click_toggles_scroll = DT_INST_PROP(n, click_toggles_scroll),                             \
        .led_feedback = DT_INST_PROP(n, led_feedback),                                             \
        .led_brightness = DT_INST_PROP(n, led_brightness),                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, pim447_init, NULL, &pim447_data_##n, &pim447_config_##n,              \
                          POST_KERNEL, CONFIG_INPUT_PIM447_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PIM447_DEFINE)
