/*
 * "Fita de LEDs" virtual: os LEDs WS2812 do Corne + o LED RGB do trackball PIM447
 * como último pixel. Apontando o `zmk,underglow` para este nó, a bola passa a
 * fazer parte do RGB do teclado (efeitos, cor, brilho, on/off, auto-off).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT pimoroni_pim447_led_strip

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

#include "pim447.h"

LOG_MODULE_REGISTER(pim447_led_strip, CONFIG_INPUT_LOG_LEVEL);

struct tee_config {
    const struct device *strip;
    const struct device *ball;
    size_t length;
};

static int tee_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t num_pixels) {
    const struct tee_config *cfg = dev->config;
    int ret = 0;

    if (num_pixels == 0) {
        return 0;
    }

    /* guardar o último pixel ANTES: o driver WS2812 pode alterar o buffer */
    struct led_rgb last = pixels[num_pixels - 1];

    if (num_pixels > 1) {
        ret = led_strip_update_rgb(cfg->strip, pixels, num_pixels - 1);
    }
    if (device_is_ready(cfg->ball)) {
        pim447_set_rgb(cfg->ball, last.r, last.g, last.b);
    }
    return ret;
}

static size_t tee_length(const struct device *dev) {
    const struct tee_config *cfg = dev->config;
    return cfg->length;
}

static const struct led_strip_driver_api tee_api = {
    .update_rgb = tee_update_rgb,
    .length = tee_length,
};

static int tee_init(const struct device *dev) {
    const struct tee_config *cfg = dev->config;
    if (!device_is_ready(cfg->strip)) {
        LOG_ERR("LED strip not ready");
        return -ENODEV;
    }
    return 0;
}

#define TEE_DEFINE(n)                                                                              \
    static const struct tee_config tee_config_##n = {                                              \
        .strip = DEVICE_DT_GET(DT_INST_PHANDLE(n, led_strip)),                                     \
        .ball = DEVICE_DT_GET(DT_INST_PHANDLE(n, trackball)),                                      \
        .length = DT_INST_PROP(n, chain_length),                                                   \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, tee_init, NULL, NULL, &tee_config_##n, POST_KERNEL,                   \
                          CONFIG_PIM447_LED_STRIP_INIT_PRIORITY, &tee_api);

DT_INST_FOREACH_STATUS_OKAY(TEE_DEFINE)
