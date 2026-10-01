/*
 * Ecrã da metade DIREITA (periférico, com o trackpad)
 *
 *  +--------+--------------+----------------+
 *  | Pixo   |  mini-mapa   | [modo] MOVE    |
 *  | (mas-  |  do trackpad | [link] OK      |
 *  | cote)  |   ·          | [bat]  95%     |
 *  +--------+--------------+----------------+
 *
 * O Pixo reage às teclas da direita (escreve), ao trackpad (aponta),
 * ao scroll (olhos arregalados), adormece ao fim de 1 min sem uso e fica
 * triste se perder a ligação à metade esquerda.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>

#include <zmk/display.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>

#include "art.h"
#include "common.h"

#define TICK_MS 100
#define ANIM_TICKS 3 /* frame da mascote a cada 300 ms */

#define PAD_X 38
#define PAD_Y 2
#define PAD_W 40
#define PAD_H 28
#define DOT 4

#define SLEEP_AFTER_MS 60000
#define ACTIVE_MS 700

/* ---- estado partilhado com outras threads (input / eventos) ---- */
static atomic_t acc_dx, acc_dy;
static atomic_t last_key_ms, last_move_ms, last_scroll_ms, last_click_ms;
static atomic_t touching;

static lv_obj_t *mascot, *pad, *dot, *mode_img, *mode_lbl, *link_img, *link_lbl, *bat_img,
    *bat_lbl;
static uint32_t ticks;
static int dot_x = (PAD_W - DOT) / 2, dot_y = (PAD_H - DOT) / 2;

static const lv_image_dsc_t *const BATS[] = {&bat_0, &bat_1, &bat_2, &bat_3, &bat_4, &bat_5};

enum mood { M_IDLE, M_TYPE, M_PAD, M_SCROLL, M_SLEEP, M_SAD };

/* ----------------------------------------------------------------------- */
/* Fontes de atividade                                                      */
/* ----------------------------------------------------------------------- */

static int on_position(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state) {
        atomic_set(&last_key_ms, (atomic_val_t)k_uptime_get_32());
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ctps_screen_keys, on_position);
ZMK_SUBSCRIPTION(ctps_screen_keys, zmk_position_state_changed);

#if DT_HAS_COMPAT_STATUS_OKAY(azoteq_tps43)
static void on_input(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);
    uint32_t now = k_uptime_get_32();

    switch (evt->type) {
    case INPUT_EV_REL:
        if (evt->code == INPUT_REL_X) {
            atomic_add(&acc_dx, evt->value);
            atomic_set(&last_move_ms, now);
        } else if (evt->code == INPUT_REL_Y) {
            atomic_add(&acc_dy, evt->value);
            atomic_set(&last_move_ms, now);
        } else if (evt->code == INPUT_REL_WHEEL || evt->code == INPUT_REL_HWHEEL) {
            atomic_set(&last_scroll_ms, now);
        }
        break;
    case INPUT_EV_KEY:
        if (evt->code == INPUT_BTN_TOUCH) {
            atomic_set(&touching, evt->value ? 1 : 0);
        } else if (evt->value) {
            atomic_set(&last_click_ms, now);
        }
        break;
    default:
        break;
    }
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST(0, azoteq_tps43)), on_input, NULL);
#endif

/* ----------------------------------------------------------------------- */
/* Desenho                                                                  */
/* ----------------------------------------------------------------------- */

static bool recent(atomic_t *ts, uint32_t now, uint32_t window) {
    uint32_t t = (uint32_t)atomic_get(ts);
    return t != 0 && (now - t) < window;
}

static const lv_image_dsc_t *mascot_frame(enum mood m, uint32_t f) {
    switch (m) {
    case M_TYPE:
        return (f & 1) ? &pixo_type_1 : &pixo_type_0;
    case M_PAD:
        return (f & 1) ? &pixo_pad_1 : &pixo_pad_0;
    case M_SCROLL:
        return (f & 1) ? &pixo_scroll_1 : &pixo_scroll_0;
    case M_SLEEP:
        return ((f / 3) & 1) ? &pixo_sleep_1 : &pixo_sleep_0;
    case M_SAD:
        return &pixo_sad_0;
    case M_IDLE:
    default:
        /* respira devagar e pestaneja de vez em quando */
        if (f % 14 == 13) {
            return &pixo_idle_2;
        }
        return ((f / 4) & 1) ? &pixo_idle_1 : &pixo_idle_0;
    }
}

static void update(lv_timer_t *t) {
    ARG_UNUSED(t);
    ticks++;
    uint32_t now = k_uptime_get_32();

    /* ---- ponto no mini-mapa do trackpad ---- */
    int dx = (int)atomic_set(&acc_dx, 0);
    int dy = (int)atomic_set(&acc_dy, 0);
    if (dx || dy) {
        dot_x += dx / 12;
        dot_y += dy / 12;
        dot_x = CLAMP(dot_x, 0, PAD_W - 2 - DOT);
        dot_y = CLAMP(dot_y, 0, PAD_H - 2 - DOT);
        lv_obj_set_pos(dot, dot_x, dot_y);
    }
    bool touch = atomic_get(&touching) || recent(&last_move_ms, now, ACTIVE_MS);
    if (touch) {
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    } else if (!lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN) && !recent(&last_move_ms, now, 1500)) {
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
    /* clique: o mini-mapa pisca (borda grossa) */
    lv_obj_set_style_border_width(pad, recent(&last_click_ms, now, 250) ? 3 : 1, 0);

    /* ---- modo / humor ---- */
    bool connected = zmk_split_bt_peripheral_is_connected();
    enum mood m;
    const char *mode;
    const lv_image_dsc_t *mimg;
    if (recent(&last_scroll_ms, now, ACTIVE_MS)) {
        m = M_SCROLL, mode = "SCRL", mimg = &sym_scroll;
    } else if (recent(&last_click_ms, now, ACTIVE_MS)) {
        m = M_PAD, mode = "TAP", mimg = &sym_tap;
    } else if (touch) {
        m = M_PAD, mode = "MOVE", mimg = &sym_tap;
    } else if (recent(&last_key_ms, now, ACTIVE_MS)) {
        m = M_TYPE, mode = "TYPE", mimg = &sym_ok;
    } else {
        uint32_t last = MAX(MAX((uint32_t)atomic_get(&last_key_ms), (uint32_t)atomic_get(&last_move_ms)),
                            (uint32_t)atomic_get(&last_scroll_ms));
        if (now - last > SLEEP_AFTER_MS) {
            m = M_SLEEP, mode = "ZZZ", mimg = &sym_zz;
        } else {
            m = M_IDLE, mode = "IDLE", mimg = &sym_ok;
        }
    }
    if (!connected && m != M_TYPE && m != M_PAD && m != M_SCROLL) {
        m = M_SAD;
    }

    static const char *last_mode;
    if (mode != last_mode) {
        last_mode = mode;
        lv_label_set_text_static(mode_lbl, mode);
        lv_image_set_src(mode_img, mimg);
    }

    static const lv_image_dsc_t *last_frame;
    const lv_image_dsc_t *fr = mascot_frame(m, ticks / ANIM_TICKS);
    if (fr != last_frame) {
        last_frame = fr;
        lv_image_set_src(mascot, fr);
    }

    /* ---- ligação à metade esquerda ---- */
    static int last_conn = -1;
    if (connected != last_conn) {
        last_conn = connected;
        lv_image_set_src(link_img, connected ? &sym_link : &sym_off);
        lv_label_set_text_static(link_lbl, connected ? "LINK" : "----");
    }

    /* ---- bateria (a cada ~2 s) ---- */
    if (ticks % 20 == 1) {
        static int last_lvl = -2, last_chg = -1;
        int lvl = zmk_battery_state_of_charge();
        int chg = zmk_usb_is_powered();
        if (lvl != last_lvl || chg != last_chg) {
            last_lvl = lvl;
            last_chg = chg;
            static char b[6];
            snprintf(b, sizeof(b), lvl >= 100 ? "%d" : "%d%%", lvl);
            lv_label_set_text_static(bat_lbl, b);
            lv_image_set_src(bat_img, chg ? &bat_chg : BATS[ctps_bat_bars(lvl)]);
        }
    }
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *scr = ctps_screen();

    mascot = ctps_image(scr, &pixo_idle_0, 0, 0);

    pad = lv_obj_create(scr);
    lv_obj_remove_style_all(pad);
    lv_obj_set_pos(pad, PAD_X, PAD_Y);
    lv_obj_set_size(pad, PAD_W, PAD_H);
    lv_obj_set_style_border_color(pad, CTPS_FG, 0);
    lv_obj_set_style_border_width(pad, 1, 0);
    lv_obj_set_style_border_opa(pad, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pad, 3, 0);
    lv_obj_set_style_pad_all(pad, 1, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

    dot = lv_obj_create(pad);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, DOT, DOT);
    lv_obj_set_style_bg_color(dot, CTPS_FG, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(dot, dot_x, dot_y);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

    mode_img = ctps_image(scr, &sym_ok, 82, 0);
    mode_lbl = ctps_label(scr, &lv_font_unscii_8, 92, 0);
    link_img = ctps_image(scr, &sym_off, 82, 12);
    link_lbl = ctps_label(scr, &lv_font_unscii_8, 92, 12);
    bat_img = ctps_image(scr, &bat_0, 82, 25);
    bat_lbl = ctps_label(scr, &lv_font_unscii_8, 98, 24);

    lv_timer_create(update, TICK_MS, NULL);
    update(NULL);
    return scr;
}
