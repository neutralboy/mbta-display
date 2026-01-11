#include "mbta_ui.h"

#include "mbta.h"

#include <stdio.h>
#include <string.h>

// Layout constants (pixels)
#define WIFI_BAR_H 18

static lv_obj_t *s_root;
static lv_obj_t *s_no_bus_banner;
static lv_obj_t *s_no_bus_label;

static lv_obj_t *s_title;

static lv_obj_t *s_big_box;
static lv_obj_t *s_big_minutes;
static lv_obj_t *s_big_suffix;

static lv_obj_t *s_row1;
static lv_obj_t *s_row2;

static uint32_t s_last_version;

static const lv_font_t *font_title;
static const lv_font_t *font_big;
static const lv_font_t *font_small;

static void set_row_text(lv_obj_t *row, const char *txt)
{
    if (row == NULL) return;
    lv_label_set_text(row, txt ? txt : "");
}

static void set_minutes_text(lv_obj_t *label, int minutes)
{
    if (label == NULL) return;

    char buf[16];
    if (minutes <= 0) {
        snprintf(buf, sizeof(buf), "0");
    } else {
        snprintf(buf, sizeof(buf), "%d", minutes);
    }
    lv_label_set_text(label, buf);
}

void MBTA_UI_Init(void)
{
    // Root container below Wi-Fi bar
    s_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, WIFI_BAR_H);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 8, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);

    font_title = LV_FONT_DEFAULT;
    font_big = LV_FONT_DEFAULT;
    font_small = LV_FONT_DEFAULT;

#if LV_FONT_MONTSERRAT_18
    font_title = &lv_font_montserrat_18;
#endif
#if LV_FONT_MONTSERRAT_28
    font_big = &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_12
    font_small = &lv_font_montserrat_12;
#endif

    // Optional red "No bus service" banner (only shown when falling back to T)
    s_no_bus_banner = lv_obj_create(s_root);
    lv_obj_set_size(s_no_bus_banner, lv_pct(100), 22);
    lv_obj_align(s_no_bus_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(s_no_bus_banner, 4, 0);
    lv_obj_set_style_border_width(s_no_bus_banner, 0, 0);
    lv_obj_set_style_bg_color(s_no_bus_banner, lv_color_hex(0xE57373), 0);
    lv_obj_set_style_pad_all(s_no_bus_banner, 4, 0);

    s_no_bus_label = lv_label_create(s_no_bus_banner);
    lv_label_set_text(s_no_bus_label, "No bus service");
    lv_obj_set_style_text_color(s_no_bus_label, lv_color_black(), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_no_bus_label, &lv_font_montserrat_12, 0);
#endif
    lv_obj_center(s_no_bus_label);

    // Title
    s_title = lv_label_create(s_root);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_text_font(s_title, font_small, 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_label_set_text(s_title, "Loading...");

    // Big minutes box
    s_big_box = lv_obj_create(s_root);
    lv_obj_set_size(s_big_box, 90, 70);
    lv_obj_align(s_big_box, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_radius(s_big_box, 10, 0);
    lv_obj_set_style_border_color(s_big_box, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_big_box, 2, 0);
    lv_obj_set_style_bg_opa(s_big_box, LV_OPA_TRANSP, 0);

    s_big_minutes = lv_label_create(s_big_box);
    lv_obj_set_style_text_font(s_big_minutes, font_big, 0);
    lv_obj_set_style_text_color(s_big_minutes, lv_color_white(), 0);
    lv_label_set_text(s_big_minutes, "--");
    lv_obj_align(s_big_minutes, LV_ALIGN_CENTER, 0, -6);

    s_big_suffix = lv_label_create(s_big_box);
    lv_obj_set_style_text_font(s_big_suffix, font_small, 0);
    lv_obj_set_style_text_color(s_big_suffix, lv_color_white(), 0);
    lv_label_set_text(s_big_suffix, "min");
    lv_obj_align(s_big_suffix, LV_ALIGN_CENTER, 0, 22);

    // Two rows for extra arrivals
    s_row1 = lv_label_create(s_root);
    lv_obj_align(s_row1, LV_ALIGN_TOP_LEFT, 0, 135);
    lv_obj_set_style_text_font(s_row1, font_small, 0);
    lv_obj_set_style_text_color(s_row1, lv_color_white(), 0);
    lv_label_set_text(s_row1, "");

    s_row2 = lv_label_create(s_root);
    lv_obj_align(s_row2, LV_ALIGN_TOP_LEFT, 0, 155);
    lv_obj_set_style_text_font(s_row2, font_small, 0);
    lv_obj_set_style_text_color(s_row2, lv_color_white(), 0);
    lv_label_set_text(s_row2, "");

    // Start hidden
    lv_obj_add_flag(s_no_bus_banner, LV_OBJ_FLAG_HIDDEN);

    s_last_version = 0;
}

void MBTA_UI_Tick(void)
{
    mbta_state_t st;
    if (!MBTA_GetState(&st)) {
        return;
    }

    if (st.version == s_last_version) {
        return;
    }
    s_last_version = st.version;

    if (st.no_bus_service_banner) {
        lv_obj_clear_flag(s_no_bus_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_no_bus_banner, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.title[0] != '\0') {
        lv_label_set_text(s_title, st.title);
    } else {
        lv_label_set_text(s_title, "");
    }

    if (!st.has_data) {
        lv_label_set_text(s_big_minutes, "--");
        set_row_text(s_row1, "No data");
        set_row_text(s_row2, "");
        return;
    }

    if (st.arrival_count <= 0) {
        lv_label_set_text(s_big_minutes, "--");
        set_row_text(s_row1, "No upcoming arrivals");
        set_row_text(s_row2, "");
        return;
    }

    set_minutes_text(s_big_minutes, st.arrivals_min[0]);

    char buf[32];
    if (st.arrival_count >= 2) {
        snprintf(buf, sizeof(buf), "Next: %d min", st.arrivals_min[1]);
        set_row_text(s_row1, buf);
    } else {
        set_row_text(s_row1, "");
    }

    if (st.arrival_count >= 3) {
        snprintf(buf, sizeof(buf), "Then: %d min", st.arrivals_min[2]);
        set_row_text(s_row2, buf);
    } else {
        set_row_text(s_row2, "");
    }
}
