/* Pimoroni PIM447 trackball — API usada pelo ecrã OLED e pelo sleeper */
#pragma once
#include <zephyr/device.h>
#include <stdbool.h>

int pim447_set_sleep(const struct device *dev, bool sleep);
bool pim447_is_scroll_mode(void);
void pim447_toggle_scroll_mode(void);
