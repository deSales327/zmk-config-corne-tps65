/*
 * Corne + TPS65 — ecrãs OLED personalizados (128x32, 1 bit)
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <lvgl.h>

/*
 * O ZMK usa o tema "mono" com fundo branco / texto preto no LVGL; o SSD1306 do
 * Corne está configurado com inversion-on, por isso no ecrã aparece fundo apagado
 * e pixels acesos. Seguimos a mesma convenção.
 */
#define CTPS_FG lv_color_black()
#define CTPS_BG lv_color_white()

static inline lv_obj_t *ctps_label(lv_obj_t *parent, const lv_font_t *font, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, CTPS_FG, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text_static(l, "");
    return l;
}

static inline lv_obj_t *ctps_image(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y) {
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, src);
    lv_obj_set_pos(i, x, y);
    return i;
}

static inline lv_obj_t *ctps_screen(void) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, CTPS_BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

/* 0..100 % -> 0..5 barras */
static inline int ctps_bat_bars(int level) {
    if (level < 0) {
        return 0;
    }
    int b = (level + 10) / 20;
    return b > 5 ? 5 : b;
}
