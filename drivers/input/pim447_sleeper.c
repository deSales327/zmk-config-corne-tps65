/* Põe o PIM447 a dormir quando o ZMK entra em deep sleep e acorda-o ao voltar */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include "pim447.h"

#define GET_DEV(node_id) DEVICE_DT_GET(node_id),
static const struct device *devs[] = {DT_FOREACH_STATUS_OKAY(pimoroni_pim447, GET_DEV)};

static int on_activity(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }
    /* só em SLEEP: em IDLE a bola tem de continuar a acordar o teclado */
    bool sleep = ev->state == ZMK_ACTIVITY_SLEEP;
    for (size_t i = 0; i < ARRAY_SIZE(devs); i++) {
        if (device_is_ready(devs[i])) {
            pim447_set_sleep(devs[i], sleep);
        }
    }
    return 0;
}

ZMK_LISTENER(pim447_sleeper, on_activity);
ZMK_SUBSCRIPTION(pim447_sleeper, zmk_activity_state_changed);
