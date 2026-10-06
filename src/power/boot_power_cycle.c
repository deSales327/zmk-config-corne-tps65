/*
 * Ciclo de energia dos periféricos no arranque (corne-tps65)
 *
 * O trackpad, o trackball, o OLED e os LEDs são alimentados pelo VCC comutado do
 * nice!nano (P0.13). Um reset do nRF52840 (botão, flash, watchdog) NÃO corta esse
 * VCC de forma limpa: os chips podem ficar meio alimentados pelos pinos de sinal e
 * presos num estado estranho até se tirar toda a energia (por isso ligar a
 * bateria "a frio" os desbloqueava).
 *
 * Aqui, logo no arranque e antes de o I2C, o trackpad, o trackball e o OLED
 * serem inicializados, desligamos o VCC e voltamos a ligar — um "power-on
 * reset" limpo em cada arranque.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ctps_power, CONFIG_ZMK_LOG_LEVEL);

#define EXT_POWER_NODE DT_INST(0, zmk_ext_power_generic)

#if DT_NODE_EXISTS(EXT_POWER_NODE) && DT_NODE_HAS_PROP(EXT_POWER_NODE, control_gpios)

static int ctps_boot_power_cycle(void) {
    const struct gpio_dt_spec ctrl = GPIO_DT_SPEC_GET_BY_IDX(EXT_POWER_NODE, control_gpios, 0);

    if (!gpio_is_ready_dt(&ctrl)) {
        return 0;
    }

    /*
     * Corre ANTES de o driver I2C configurar os pinos (prioridade 50): neste
     * momento SDA/SCL ainda estão desligados (estado de reset), por isso não
     * alimentam os chips "por trás" enquanto o VCC está em baixo.
     */
    gpio_pin_configure_dt(&ctrl, GPIO_OUTPUT_INACTIVE); /* VCC desligado */
    k_busy_wait(CONFIG_CORNE_TPS65_BOOT_POWER_OFF_MS * 1000);
    gpio_pin_set_dt(&ctrl, 1); /* VCC ligado */
    k_busy_wait(CONFIG_CORNE_TPS65_BOOT_POWER_SETTLE_MS * 1000);
    return 0;
}

/* depois do GPIO (40) e antes do I2C (50), do ext-power do ZMK (81) e do OLED (85) */
SYS_INIT(ctps_boot_power_cycle, POST_KERNEL, 45);

#endif
