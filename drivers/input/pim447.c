/*
 * Pimoroni Trackball Breakout (PIM447) — driver de input para Zephyr/ZMK
 *
 * - Lê as contagens esquerda/direita/cima/baixo e o clique por I2C (0x0A).
 * - Pino INT (ativo em LOW) opcional; sem ele, lê periodicamente (polling
 *   adaptativo: rápido enquanto a bola mexe, lento quando está parada).
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
    uint16_t poll_ms;      /* período de leitura com a bola a mexer */
    uint16_t idle_poll_ms; /* período de leitura com a bola parada */
    uint16_t rotation;     /* rotação dos eixos (0/90/180/270, sentido horário) */
    uint8_t cursor_mult;
    uint8_t accel;
    uint8_t scroll_div;
    bool click_toggles_scroll;
    bool led_feedback;
    uint8_t noise_threshold; /* contagens mínimas para arrancar a partir de repouso */
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
    int64_t last_activity_ms; /* último movimento/clique (para o polling adaptativo) */
    int64_t last_count_ms;    /* última contagem aceite (filtro de ruído) */
    int64_t pend_ms;          /* início da janela do filtro de ruído */
    int32_t pend_x, pend_y;   /* contagens retidas pelo filtro de ruído */
    int32_t speed16_avg;      /* velocidade suavizada (para a aceleração) */
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
    /*
     * Média móvel da velocidade: uma leitura com 1 contagem e a seguinte com 3
     * já não fazem o ganho saltar de um valor para outro (cursor aos solavancos).
     */
    if (dt >= 100) {
        data->speed16_avg = speed16; /* a arrancar de repouso */
    } else {
        data->speed16_avg = (data->speed16_avg * 3 + speed16) / 4;
    }
    int32_t gain16 = cfg->cursor_mult * (16 + cfg->accel * data->speed16_avg / 4);
    gain16 = MIN(gain16, cfg->cursor_mult * 16 * 12); /* teto: 12x */

    int32_t x16 = *dx * gain16 + data->rem_x;
    int32_t y16 = *dy * gain16 + data->rem_y;
    *dx = x16 / 16;
    *dy = y16 / 16;
    data->rem_x = x16 - *dx * 16;
    data->rem_y = y16 - *dy * 16;
}

/* Roda os eixos da bola conforme a montagem (vale para cursor e scroll) */
static void rotate(uint16_t deg, int32_t *dx, int32_t *dy) {
    int32_t x = *dx, y = *dy;
    switch (deg) {
    case 90:
        *dx = -y;
        *dy = x;
        break;
    case 180:
        *dx = -x;
        *dy = -y;
        break;
    case 270:
        *dx = y;
        *dy = -x;
        break;
    default:
        break;
    }
}

/* Lê a bola e envia os eventos. Devolve true se houve movimento ou clique. */
static bool process(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;
    uint8_t b[5];
    bool activity = false;

    if (data->sleeping) {
        return false;
    }
    if (reg_read(dev, REG_LEFT, b, sizeof(b)) != 0) {
        LOG_DBG("read failed");
        return false;
    }

    int32_t dx = (int32_t)b[1] - (int32_t)b[0]; /* direita - esquerda */
    int32_t dy = (int32_t)b[3] - (int32_t)b[2]; /* baixo - cima */
    rotate(cfg->rotation, &dx, &dy);
    bool pressed = (b[4] & MSK_SWITCH_STATE) != 0;

    if (pressed != data->pressed) {
        data->pressed = pressed;
        activity = true;
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
        return activity;
    }

    /*
     * Filtro de ruído: com a bola parada, uma contagem solta (vibração,
     * tocar na bola, mexer no teclado) não mexe o cursor. A partir de
     * repouso só arranca com `noise_threshold` contagens em 80 ms.
     */
    int64_t now = k_uptime_get();
    if (cfg->noise_threshold > 1 && now - data->last_count_ms > 150) {
        if (now - data->pend_ms > 80) {
            data->pend_x = data->pend_y = 0;
            data->pend_ms = now;
        }
        data->pend_x += dx;
        data->pend_y += dy;
        if (abs(data->pend_x) + abs(data->pend_y) < cfg->noise_threshold) {
            return true; /* continua a ler depressa para apanhar a próxima contagem */
        }
        dx = data->pend_x;
        dy = data->pend_y;
        data->pend_x = data->pend_y = 0;
    }
    data->last_count_ms = now;
    activity = true;

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
    return activity;
}

static void work_handler(struct k_work *work) {
    struct pim447_data *data = CONTAINER_OF(work, struct pim447_data, work);
    const struct device *dev = data->dev;
    const struct pim447_config *cfg = dev->config;

    /*
     * Se o INT continua ativo depois de ler, volta a ler — mas no máximo
     * algumas vezes seguidas. Um INT preso em LOW (mau contacto) não pode
     * monopolizar a fila de trabalho do sistema (teclas e ecrã correm lá).
     */
    for (int i = 0; i < 8; i++) {
        process(dev);
        if (cfg->int_gpio.port == NULL || data->sleeping ||
            gpio_pin_get_dt(&cfg->int_gpio) <= 0) {
            break;
        }
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
    uint16_t next = cfg->idle_poll_ms;
    if (!data->sleeping) {
        int64_t now = k_uptime_get();
        if (process(data->dev)) {
            data->last_activity_ms = now;
        }
        /* rápido durante 1 s depois do último movimento, depois abranda */
        if (now - data->last_activity_ms < 1000) {
            next = cfg->poll_ms;
        }
    }
    k_work_reschedule(&data->poll_work, K_MSEC(next));
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
        led_restore(dev);
        /* limpa contagens acumuladas durante o sono */
        k_work_submit(&data->work);
    }
    return ret;
}

int pim447_set_rgb(const struct device *dev, uint8_t r, uint8_t g, uint8_t b) {
    struct pim447_data *data = dev->data;
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

    /* descarta o movimento acumulado desde que a bola ligou (evita um salto
     * enorme do cursor na primeira leitura) */
    uint8_t junk[5];
    reg_read(dev, REG_LEFT, junk, sizeof(junk));

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
            k_work_reschedule(&data->poll_work, K_MSEC(500)); /* vigia do INT */
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
        .idle_poll_ms = DT_INST_PROP(n, idle_poll_interval_ms),                                    \
        .rotation = DT_INST_PROP(n, rotation),                                                     \
        .cursor_mult = DT_INST_PROP(n, cursor_multiplier),                                         \
        .accel = DT_INST_PROP(n, acceleration),                                                    \
        .scroll_div = MAX(1, DT_INST_PROP(n, scroll_divisor)),                                     \
        .click_toggles_scroll = DT_INST_PROP(n, click_toggles_scroll),                             \
        .led_feedback = DT_INST_PROP(n, led_feedback),                                             \
        .led_brightness = DT_INST_PROP(n, led_brightness),                                         \
        .noise_threshold = DT_INST_PROP(n, noise_threshold),                                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, pim447_init, NULL, &pim447_data_##n, &pim447_config_##n,              \
                          POST_KERNEL, CONFIG_INPUT_PIM447_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PIM447_DEFINE)
