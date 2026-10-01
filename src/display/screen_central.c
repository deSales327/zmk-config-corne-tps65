/*
 * Ecrã da metade ESQUERDA (central)
 *
 *  +--------+---------------------------------+
 *  | ícone  | NOME DA LAYER                    |
 *  | layer  | [BT]1 [ok]                  wpm  |
 *  | 32x32  | L[bat] R[bat]                42  |
 *  +--------+---------------------------------+
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include <zmk/display.h>
#include <zmk/keymap.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/usb.h>
#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/wpm.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

#include "art.h"
#include "common.h"

#define TICK_MS 100
#define ICON_FRAME_TICKS 6 /* troca de frame do ícone a cada 600 ms */

static lv_obj_t *icon, *layer_lbl, *conn_img, *prof_lbl, *link_img, *wpm_txt, *wpm_lbl;
static lv_obj_t *bat_l, *bat_r, *bat_r_lbl;

static struct {
    int layer;
    int icon_set;
    int frame;
    int transport;
    int profile;
    int link; /* 0 livre, 1 desligado, 2 ligado */
    int wpm;
    int bat_l, chg_l, bat_r;
} cur = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -2};

static uint32_t ticks;

static const lv_image_dsc_t *const ICONS[][2] = {
    {&icon_base_0, &icon_base_1},   {&icon_lower_0, &icon_lower_1}, {&icon_raise_0, &icon_raise_1},
    {&icon_mouse_0, &icon_mouse_1}, {&icon_other_0, &icon_other_1},
};

static const lv_image_dsc_t *const BATS[] = {&bat_0, &bat_1, &bat_2, &bat_3, &bat_4, &bat_5};

static int icon_for_layer(int index, const char *name) {
    if (name != NULL) {
        if (strncasecmp(name, "base", 4) == 0 || strncasecmp(name, "qwerty", 6) == 0) {
            return 0;
        }
        if (strncasecmp(name, "lower", 5) == 0 || strncasecmp(name, "num", 3) == 0) {
            return 1;
        }
        if (strncasecmp(name, "raise", 5) == 0 || strncasecmp(name, "sym", 3) == 0) {
            return 2;
        }
        if (strncasecmp(name, "mouse", 5) == 0) {
            return 3;
        }
    }
    return index <= 3 ? index : 4;
}

static void update(lv_timer_t *t) {
    ARG_UNUSED(t);
    ticks++;

    /* ---- layer ---- */
    int layer = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(layer));
    if (layer != cur.layer) {
        cur.layer = layer;
        static char buf[16];
        if (name != NULL && name[0] != '\0') {
            strncpy(buf, name, sizeof(buf) - 1);
        } else {
            snprintf(buf, sizeof(buf), "Layer %d", layer);
        }
        for (char *p = buf; *p; p++) {
            if (*p >= 'a' && *p <= 'z') {
                *p -= 32;
            }
        }
        lv_label_set_text_static(layer_lbl, buf);
        cur.icon_set = icon_for_layer(layer, name);
        cur.frame = -1;
    }
    int frame = (ticks / ICON_FRAME_TICKS) & 1;
    if (frame != cur.frame) {
        cur.frame = frame;
        lv_image_set_src(icon, ICONS[cur.icon_set][frame]);
    }

    /* ---- ligação ao computador ---- */
    struct zmk_endpoint_instance ep = zmk_endpoint_get_selected();
    int transport = ep.transport;
    int profile = zmk_ble_active_profile_index();
    int link = zmk_ble_active_profile_is_open() ? 0
               : (zmk_ble_active_profile_is_connected() ? 2 : 1);
    if (transport == ZMK_TRANSPORT_USB) {
        link = 2;
    }
    if (transport != cur.transport || profile != cur.profile || link != cur.link) {
        cur.transport = transport;
        cur.profile = profile;
        cur.link = link;
        static char pbuf[12];
        if (transport == ZMK_TRANSPORT_USB) {
            lv_image_set_src(conn_img, &sym_usb);
            pbuf[0] = '\0';
        } else {
            lv_image_set_src(conn_img, &sym_bt);
            snprintf(pbuf, sizeof(pbuf), "%d", profile + 1);
        }
        lv_label_set_text_static(prof_lbl, pbuf);
        lv_image_set_src(link_img, link == 2 ? &sym_ok : (link == 1 ? &sym_off : &sym_open));
    }

    /* ---- WPM ---- */
#if IS_ENABLED(CONFIG_ZMK_WPM)
    int wpm = zmk_wpm_get_state();
    if (wpm != cur.wpm) {
        cur.wpm = wpm;
        static char wbuf[6];
        snprintf(wbuf, sizeof(wbuf), "%3d", wpm);
        lv_label_set_text_static(wpm_txt, wbuf);
    }
#endif

    /* ---- baterias (a cada ~2 s) ---- */
    if (ticks % 20 == 1) {
        int lvl = zmk_battery_state_of_charge();
        int chg = zmk_usb_is_powered();
        if (lvl != cur.bat_l || chg != cur.chg_l) {
            cur.bat_l = lvl;
            cur.chg_l = chg;
            lv_image_set_src(bat_l, chg ? &bat_chg : BATS[ctps_bat_bars(lvl)]);
        }
        int r = -1;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
        uint8_t rl;
        if (zmk_split_central_get_peripheral_battery_level(0, &rl) == 0 && rl > 0) {
            r = rl;
        }
#endif
        if (r != cur.bat_r) {
            cur.bat_r = r;
            lv_image_set_src(bat_r, BATS[ctps_bat_bars(r)]);
            lv_label_set_text_static(bat_r_lbl, r < 0 ? "R?" : "R");
        }
    }
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *scr = ctps_screen();

    icon = ctps_image(scr, &icon_base_0, 0, 0);
    layer_lbl = ctps_label(scr, &lv_font_montserrat_16, 36, -2);

    conn_img = ctps_image(scr, &sym_bt, 36, 16);
    prof_lbl = ctps_label(scr, &lv_font_unscii_8, 45, 16);
    link_img = ctps_image(scr, &sym_off, 54, 16);

    lv_obj_t *l = ctps_label(scr, &lv_font_unscii_8, 36, 24);
    lv_label_set_text_static(l, "L");
    bat_l = ctps_image(scr, &bat_0, 44, 25);
    bat_r_lbl = ctps_label(scr, &lv_font_unscii_8, 62, 24);
    lv_label_set_text_static(bat_r_lbl, "R");
    bat_r = ctps_image(scr, &bat_0, 71, 25);

    wpm_lbl = ctps_label(scr, &lv_font_unscii_8, 104, 16);
    wpm_txt = ctps_label(scr, &lv_font_unscii_8, 104, 24);
#if IS_ENABLED(CONFIG_ZMK_WPM)
    lv_label_set_text_static(wpm_lbl, "wpm");
    lv_label_set_text_static(wpm_txt, "  0");
#endif

    lv_timer_create(update, TICK_MS, NULL);
    update(NULL);
    return scr;
}
