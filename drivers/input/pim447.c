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
    bool flashing;
    uint8_t rgb[3]; /* cor pedida pelo RGB do teclado (pim447_set_rgb) */
    int scroll_acc_x, scroll_acc_y;
    int64_t last_move_ms;
    bool cb_added;
    bool ready;
    int retries;
    struct k_work_delayable retry_work;
    int32_t rem_x, rem_y; /* restos (x1/16) para movimento suave */
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

static void led_restore(const struct device *dev) {
    struct pim447_data *data = dev->data;
    if (data->sleeping) {
        set_led(dev, 0, 0, 0, 0);
    } else {
        set_led(dev, data->rgb[0], data->rgb[1], data->rgb[2], 0);
    }
}

static void led_off_handler(struct k_work *work) {
    struct k_work_delayable *dw = k_work_delayable_from_work(work);
    struct pim447_data *data = CONTAINER_OF(dw, struct pim447_data, led_off_work);
    data->flashing = false;
    led_restore(data->dev);
}

static void flash_mode(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;
    if (!cfg->led_feedback) {
        return;
    }
    uint8_t v = cfg->led_brightness;
    data->flashing = true;
    if (data->scroll_mode) {
        set_led(dev, 0, v, 0, 0);
    } else {
        set_led(dev, 0, 0, v, 0);
    }
    k_work_reschedule(&data->led_off_work, K_MSEC(400));
}

/*
 * Aceleração pela VELOCIDADE da bola (contagens por 10 ms):
 *   ganho = multiplicador * (1 + aceleração * velocidade / 4)
 * Rodar devagar = preciso; rodar depressa = atravessa o ecrã.
 * Trabalha em 1/16 de pixel e guarda o resto, para não perder movimento.
 */
static void accelerate(const struct pim447_config *cfg, struct pim447_data *data, int32_t *dx,
                       int32_t *dy) {
    int64_t now = k_uptime_get();
    int32_t dt = (int32_t)CLAMP(now - data->last_move_ms, 1, 100);
    data->last_move_ms = now;

    int32_t counts = abs(*dx) + abs(*dy);
    int32_t speed16 = counts * 160 / dt; /* contagens por 10 ms, x16 */
    int32_t gain16 = cfg->cursor_mult * (16 + cfg->accel * speed16 / 4);
    gain16 = MIN(gain16, cfg->cursor_mult * 16 * 12); /* teto: 12x */

    int32_t x16 = *dx * gain16 + data->rem_x;
    int32_t y16 = *dy * gain16 + data->rem_y;
    *dx = x16 / 16;
    *dy = y16 / 16;
    data->rem_x = x16 - *dx * 16;
    data->rem_y = y16 - *dy * 16;
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
        accelerate(cfg, data, &dx, &dy);
        if (dx != 0 || dy != 0) {
            input_report_rel(dev, INPUT_REL_X, dx, false, K_FOREVER);
            input_report_rel(dev, INPUT_REL_Y, dy, true, K_FOREVER);
        }
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

    if (cfg->int_gpio.port != NULL) {
        /* modo INT: só um "vigia" — se o INT ficou ativo sem flanco, lê agora */
        if (!data->sleeping && gpio_pin_get_dt(&cfg->int_gpio) > 0) {
            k_work_submit(&data->work);
        }
        k_work_reschedule(&data->poll_work, K_MSEC(500));
        return;
    }
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
    if (!data->ready) {
        return 0;
    }
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
        led_restore(dev);
        /* limpa contagens acumuladas durante o sono */
        k_work_submit(&data->work);
    }
    return ret;
}

int pim447_set_rgb(const struct device *dev, uint8_t r, uint8_t g, uint8_t b) {
    struct pim447_data *data = dev->data;
    if (!data->ready) {
        data->rgb[0] = r;
        data->rgb[1] = g;
        data->rgb[2] = b;
        return 0;
    }
    if (data->rgb[0] == r && data->rgb[1] == g && data->rgb[2] == b) {
        return 0;
    }
    data->rgb[0] = r;
    data->rgb[1] = g;
    data->rgb[2] = b;
    if (!data->sleeping && !data->flashing) {
        set_led(dev, r, g, b, 0);
    }
    return 0;
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

static int pim447_hw_setup(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;
    uint8_t id[2];

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
        if (ret == 0 && !data->cb_added) {
            gpio_init_callback(&data->int_cb, int_cb, BIT(cfg->int_gpio.pin));
            ret = gpio_add_callback(cfg->int_gpio.port, &data->int_cb);
            data->cb_added = (ret == 0);
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
            k_work_reschedule(&data->poll_work, K_MSEC(500)); /* vigia do INT */
        }
    } else {
        k_work_reschedule(&data->poll_work, K_MSEC(cfg->poll_ms));
    }

    data->ready = true;
    led_restore(dev); /* aplica a cor do RGB que possa ter chegado entretanto */
    LOG_INF("PIM447 ready (%s, %s)", cfg->int_gpio.port ? "INT" : "polling",
            cfg->click_toggles_scroll ? "click toggles scroll" : "click = BTN_0");
    return 0;
}

/*
 * Arranque robusto: se o trackball não responder no arranque, liberta o
 * barramento I2C e tenta outra vez a cada 2 s (até ~30 s), em vez de ficar morto.
 */
static void retry_handler(struct k_work *work) {
    struct k_work_delayable *dw = k_work_delayable_from_work(work);
    struct pim447_data *data = CONTAINER_OF(dw, struct pim447_data, retry_work);
    const struct device *dev = data->dev;
    const struct pim447_config *cfg = dev->config;

    data->retries++;
    i2c_recover_bus(cfg->i2c.bus);
    if (pim447_hw_setup(dev) == 0) {
        LOG_INF("PIM447 recovered after %d retries", data->retries);
        return;
    }
    if (data->retries < 15) {
        k_work_reschedule(&data->retry_work, K_SECONDS(2));
    } else {
        LOG_ERR("PIM447 not responding, giving up");
    }
}

static int pim447_init(const struct device *dev) {
    struct pim447_data *data = dev->data;

    data->dev = dev;
    k_work_init(&data->work, work_handler);
    k_work_init_delayable(&data->poll_work, poll_handler);
    k_work_init_delayable(&data->led_off_work, led_off_handler);
    k_work_init_delayable(&data->retry_work, retry_handler);

    int ret = pim447_hw_setup(dev);
    if (ret != 0) {
        LOG_WRN("PIM447 init failed (%d), will retry", ret);
        k_work_schedule(&data->retry_work, K_SECONDS(2));
    }
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
