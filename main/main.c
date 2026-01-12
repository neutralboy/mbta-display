#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>
#include <string.h>

#include "lvgl.h"

#include "ST7789.h"
#include "Wireless.h"
#include "config.h"

#include "RGB.h"

#include "mbta.h"
#include "weather.h"

typedef enum {
    UI_MODE_MBTA = 0,
    UI_MODE_WEATHER = 1,
} ui_mode_t;

static lv_obj_t *s_screen_mbta;
static lv_obj_t *s_screen_weather;
static ui_mode_t s_ui_mode = UI_MODE_MBTA;

static void ui_set_screen_bg(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

static void ui_switch_mode(ui_mode_t mode)
{
    if (mode == s_ui_mode) {
        return;
    }

    s_ui_mode = mode;
    if (mode == UI_MODE_WEATHER && s_screen_weather != NULL) {
        lv_scr_load(s_screen_weather);
    } else if (mode == UI_MODE_MBTA && s_screen_mbta != NULL) {
        lv_scr_load(s_screen_mbta);
    }
}

static lv_obj_t *s_mbta_no_bus_banner;
static lv_obj_t *s_mbta_no_bus_label;
static lv_obj_t *s_mbta_title;
static lv_obj_t *s_mbta_big_box;
static lv_obj_t *s_mbta_big_minutes;
static lv_obj_t *s_mbta_big_suffix;
static lv_obj_t *s_mbta_row1;
static lv_obj_t *s_mbta_row2;
static lv_obj_t *s_mbta_weather_label;
static lv_obj_t *s_mbta_time_label;
static lv_obj_t *s_mbta_loader;
static uint32_t s_mbta_last_version;
static bool s_mbta_is_fetching = false;

static lv_obj_t *s_weather_title;
static lv_obj_t *s_weather_temp;
static lv_obj_t *s_weather_hilo;
static lv_obj_t *s_weather_cond;
static lv_obj_t *s_weather_loader;
static uint32_t s_weather_last_version;
static bool s_weather_is_fetching = false;

static void set_loader_opa_cb(void * var, int32_t v);
static void set_loader_value_cb(void * var, int32_t v);

static void ui_weather_init(lv_obj_t *parent)
{
    s_weather_title = lv_label_create(parent);
    lv_obj_set_width(s_weather_title, lv_pct(100));
    lv_obj_align(s_weather_title, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_color(s_weather_title, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_weather_title, &lv_font_montserrat_16, 0);
#elif LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_weather_title, &lv_font_montserrat_14, 0);
#endif
    lv_obj_set_style_text_align(s_weather_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weather_title, "Weather");

    s_weather_temp = lv_label_create(parent);
    lv_obj_set_width(s_weather_temp, lv_pct(100));
    lv_obj_align(s_weather_temp, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_style_text_color(s_weather_temp, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_48
    lv_obj_set_style_text_font(s_weather_temp, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_36
    lv_obj_set_style_text_font(s_weather_temp, &lv_font_montserrat_36, 0);
#elif LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(s_weather_temp, &lv_font_montserrat_24, 0);
#endif
    lv_obj_set_style_text_align(s_weather_temp, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weather_temp, "--°C");

    s_weather_hilo = lv_label_create(parent);
    lv_obj_set_width(s_weather_hilo, lv_pct(100));
    lv_obj_align(s_weather_hilo, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_text_color(s_weather_hilo, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_weather_hilo, &lv_font_montserrat_16, 0);
#elif LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_weather_hilo, &lv_font_montserrat_14, 0);
#elif LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_weather_hilo, &lv_font_montserrat_12, 0);
#endif
    lv_obj_set_style_text_align(s_weather_hilo, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weather_hilo, "H: --°C   L: --°C");

    s_weather_cond = lv_label_create(parent);
    lv_obj_set_width(s_weather_cond, lv_pct(100));
    lv_obj_align(s_weather_cond, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_set_style_text_color(s_weather_cond, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_weather_cond, &lv_font_montserrat_16, 0);
#elif LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_weather_cond, &lv_font_montserrat_14, 0);
#elif LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_weather_cond, &lv_font_montserrat_12, 0);
#endif
    lv_obj_set_style_text_align(s_weather_cond, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weather_cond, "Loading...");

    // Bottom loader (fetching indicator + countdown to next refresh)
    s_weather_loader = lv_bar_create(parent);
    lv_obj_set_size(s_weather_loader, lv_pct(100), 5);
    lv_obj_align(s_weather_loader, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_weather_loader, 0, 1000);
    lv_bar_set_value(s_weather_loader, 1000, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(s_weather_loader, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(s_weather_loader, 0, 0);
    // Darker baseline, bright indicator
    lv_obj_set_style_bg_color(s_weather_loader, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_weather_loader, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_add_flag(s_weather_loader, LV_OBJ_FLAG_HIDDEN);

    s_weather_last_version = 0;
    s_weather_is_fetching = false;
}

static void ui_weather_update(void)
{
    weather_state_t st;
    if (!Weather_GetState(&st)) {
        return;
    }

    // Handle Loader/Fetching Animation
    if (st.is_fetching != s_weather_is_fetching) {
        s_weather_is_fetching = st.is_fetching;
        if (s_weather_is_fetching) {
            lv_obj_clear_flag(s_weather_loader, LV_OBJ_FLAG_HIDDEN);
            // Signal Fetching: Yellow + Flash
            lv_obj_set_style_bg_color(s_weather_loader, lv_color_hex(0xFBC02D), LV_PART_INDICATOR);
            lv_bar_set_value(s_weather_loader, 1000, LV_ANIM_OFF);
            lv_anim_del(s_weather_loader, set_loader_value_cb);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_weather_loader);
            lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
            lv_anim_set_time(&a, 600);
            lv_anim_set_playback_time(&a, 600);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, set_loader_opa_cb);
            lv_anim_start(&a);
        } else {
            // Fetch Done: White + Restart countdown
            lv_anim_del(s_weather_loader, set_loader_opa_cb);
            lv_obj_set_style_opa(s_weather_loader, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_weather_loader, lv_color_white(), LV_PART_INDICATOR);

            if (st.has_data) {
                lv_obj_clear_flag(s_weather_loader, LV_OBJ_FLAG_HIDDEN);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_weather_loader);
                lv_anim_set_values(&a, 1000, 0);
                lv_anim_set_time(&a, WEATHER_FETCH_PERIOD_MS);
                lv_anim_set_exec_cb(&a, set_loader_value_cb);
                lv_anim_start(&a);
            } else {
                lv_obj_add_flag(s_weather_loader, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (st.version == s_weather_last_version) {
        return;
    }
    s_weather_last_version = st.version;

    if (!st.has_data) {
        lv_label_set_text(s_weather_temp, "--°C");
        lv_label_set_text(s_weather_hilo, "H: --°C   L: --°C");
        lv_label_set_text(s_weather_cond, st.is_fetching ? "Updating..." : st.condition);
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%d" "\xC2\xB0" "C", st.temp_c);
    lv_label_set_text(s_weather_temp, buf);

    snprintf(buf, sizeof(buf), "H: %d" "\xC2\xB0" "C   L: %d" "\xC2\xB0" "C", st.high_c, st.low_c);
    lv_label_set_text(s_weather_hilo, buf);

    lv_label_set_text(s_weather_cond, st.condition);
}

static void set_loader_opa_cb(void * var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, v, 0);
}

static void set_loader_value_cb(void * var, int32_t v)
{
    lv_bar_set_value((lv_obj_t *)var, v, LV_ANIM_OFF);
}

static void ui_mbta_init(lv_obj_t *parent)
{
    // Banner (hidden unless bus missing) - docked at the bottom like WiFi at the top
    s_mbta_no_bus_banner = lv_obj_create(parent);
    lv_obj_set_size(s_mbta_no_bus_banner, lv_pct(100), 18);
    lv_obj_align(s_mbta_no_bus_banner, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_mbta_no_bus_banner, 0, 0);
    lv_obj_set_style_border_width(s_mbta_no_bus_banner, 0, 0);
    lv_obj_set_style_pad_all(s_mbta_no_bus_banner, 4, 0);
    lv_obj_set_style_bg_color(s_mbta_no_bus_banner, lv_color_hex(0xE57373), 0);
    lv_obj_set_flex_flow(s_mbta_no_bus_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_mbta_no_bus_banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_mbta_no_bus_label = lv_label_create(s_mbta_no_bus_banner);
    lv_label_set_text(s_mbta_no_bus_label, "No bus service");
    lv_obj_set_style_text_color(s_mbta_no_bus_label, lv_color_black(), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_mbta_no_bus_label, &lv_font_montserrat_12, 0);
#endif
    lv_obj_add_flag(s_mbta_no_bus_banner, LV_OBJ_FLAG_HIDDEN);

    // Title (centered)
    s_mbta_title = lv_label_create(parent);
    lv_obj_set_width(s_mbta_title, lv_pct(100));
    lv_obj_align(s_mbta_title, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_color(s_mbta_title, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_mbta_title, &lv_font_montserrat_14, 0);
#elif LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_mbta_title, &lv_font_montserrat_12, 0);
#elif LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_mbta_title, &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_align(s_mbta_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_mbta_title, "Loading...");

    // Big minutes box
    s_mbta_big_box = lv_obj_create(parent);
    lv_obj_set_size(s_mbta_big_box, 140, 120);
    lv_obj_align(s_mbta_big_box, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_radius(s_mbta_big_box, 10, 0);
    lv_obj_set_style_border_color(s_mbta_big_box, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_mbta_big_box, 2, 0);
    lv_obj_set_style_bg_opa(s_mbta_big_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_mbta_big_box, 0, 0);
    lv_obj_set_style_clip_corner(s_mbta_big_box, true, 0);

    s_mbta_big_minutes = lv_label_create(s_mbta_big_box);
    lv_obj_set_style_text_color(s_mbta_big_minutes, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_48
    lv_obj_set_style_text_font(s_mbta_big_minutes, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_36
    lv_obj_set_style_text_font(s_mbta_big_minutes, &lv_font_montserrat_36, 0);
#elif LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(s_mbta_big_minutes, &lv_font_montserrat_24, 0);
#endif
    lv_label_set_text(s_mbta_big_minutes, "--");
    lv_obj_align(s_mbta_big_minutes, LV_ALIGN_CENTER, 0, -14);

    s_mbta_big_suffix = lv_label_create(s_mbta_big_box);
    lv_obj_set_style_text_color(s_mbta_big_suffix, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_mbta_big_suffix, &lv_font_montserrat_16, 0);
#elif LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_mbta_big_suffix, &lv_font_montserrat_14, 0);
#endif
    lv_label_set_text(s_mbta_big_suffix, "min");
    lv_obj_align(s_mbta_big_suffix, LV_ALIGN_CENTER, 0, 38);

    // Extra rows
    s_mbta_row1 = lv_label_create(parent);
    lv_obj_set_width(s_mbta_row1, lv_pct(100));
    lv_obj_align(s_mbta_row1, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_text_color(s_mbta_row1, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(s_mbta_row1, &lv_font_montserrat_24, 0);
#elif LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_mbta_row1, &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_align(s_mbta_row1, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_mbta_row1, "");

    s_mbta_row2 = lv_label_create(parent);
    lv_obj_set_width(s_mbta_row2, lv_pct(100));
    lv_obj_align(s_mbta_row2, LV_ALIGN_TOP_MID, 0, 224);
    lv_obj_set_style_text_color(s_mbta_row2, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_mbta_row2, &lv_font_montserrat_12, 0);
#endif
    lv_obj_set_style_text_align(s_mbta_row2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_mbta_row2, "");

    s_mbta_time_label = lv_label_create(parent);
    lv_obj_set_width(s_mbta_time_label, lv_pct(100));
    lv_obj_align(s_mbta_time_label, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_text_color(s_mbta_time_label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_mbta_time_label, &lv_font_montserrat_16, 0);
#elif LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_mbta_time_label, &lv_font_montserrat_14, 0);
#elif LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_mbta_time_label, &lv_font_montserrat_12, 0);
#endif
    lv_obj_set_style_text_align(s_mbta_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_mbta_time_label, "");

    s_mbta_weather_label = lv_label_create(parent);
    lv_obj_set_width(s_mbta_weather_label, lv_pct(100));
    lv_obj_align(s_mbta_weather_label, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_obj_set_style_text_color(s_mbta_weather_label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(s_mbta_weather_label, &lv_font_montserrat_16, 0);
#elif LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_mbta_weather_label, &lv_font_montserrat_14, 0);
#elif LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_mbta_weather_label, &lv_font_montserrat_12, 0);
#endif
    lv_obj_set_style_text_align(s_mbta_weather_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_mbta_weather_label, "");

    s_mbta_loader = lv_bar_create(s_mbta_big_box);
    lv_obj_set_size(s_mbta_loader, lv_pct(100), 5);
    lv_obj_align(s_mbta_loader, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_mbta_loader, 0, 1000);
    lv_bar_set_value(s_mbta_loader, 1000, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(s_mbta_loader, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(s_mbta_loader, 0, 0);
    // Darker baseline, bright indicator
    lv_obj_set_style_bg_color(s_mbta_loader, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mbta_loader, lv_color_white(), LV_PART_INDICATOR);

    s_mbta_last_version = 0;
}

static void ui_mbta_update(void)
{
    // Update Time
    time_t now;
    time(&now);
    static time_t last_time = 0;

    if (now != last_time) {
        last_time = now;
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        if (now > 1577836800) { // Sane time check (> 2020)
            char time_buf[32];
            strftime(time_buf, sizeof(time_buf), "%b %d %H:%M", &timeinfo);
            lv_label_set_text(s_mbta_time_label, time_buf);
        } else {
            lv_label_set_text(s_mbta_time_label, "");
        }

        // Update Weather on MBTA screen
        weather_state_t wst;
        if (Weather_GetState(&wst) && wst.has_data) {
            char w_buf[64];
            snprintf(w_buf, sizeof(w_buf), "%d  L: %d H: %d  %s", 
                     wst.temp_c, wst.low_c, wst.high_c, wst.condition);
            lv_label_set_text(s_mbta_weather_label, w_buf);
        } else {
            lv_label_set_text(s_mbta_weather_label, "");
        }
    }

    mbta_state_t st;
    if (!MBTA_GetState(&st)) {
        return;
    }

    // Note: display_off is used for schedule-based mode switching.
    // We no longer turn the backlight off here; main loop will swap to Weather.

    // Handle Loader/Fetching Animation
    if (st.is_fetching != s_mbta_is_fetching) {
        s_mbta_is_fetching = st.is_fetching;
        if (s_mbta_is_fetching) {
            lv_obj_clear_flag(s_mbta_loader, LV_OBJ_FLAG_HIDDEN);
            // Signal Fetching: Yellow + Flash
            lv_obj_set_style_bg_color(s_mbta_loader, lv_color_hex(0xFBC02D), LV_PART_INDICATOR);
            lv_bar_set_value(s_mbta_loader, 1000, LV_ANIM_OFF);
            lv_anim_del(s_mbta_loader, set_loader_value_cb);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_mbta_loader);
            lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
            lv_anim_set_time(&a, 600);
            lv_anim_set_playback_time(&a, 600);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, set_loader_opa_cb);
            lv_anim_start(&a);
        } else {
            // Fetch Done: White + Restart 30s Countdown
            lv_anim_del(s_mbta_loader, set_loader_opa_cb);
            lv_obj_set_style_opa(s_mbta_loader, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_mbta_loader, lv_color_white(), LV_PART_INDICATOR);
            
            if (st.has_data) {
                lv_obj_clear_flag(s_mbta_loader, LV_OBJ_FLAG_HIDDEN);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_mbta_loader);
                lv_anim_set_values(&a, 1000, 0);
                lv_anim_set_time(&a, MBTA_FETCH_PERIOD_MS);
                lv_anim_set_exec_cb(&a, set_loader_value_cb);
                lv_anim_start(&a);
            } else {
                lv_obj_add_flag(s_mbta_loader, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (st.version == s_mbta_last_version) {
        return;
    }
    s_mbta_last_version = st.version;

    if (st.no_bus_service_banner) {
        lv_obj_clear_flag(s_mbta_no_bus_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mbta_no_bus_banner, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.title[0] != '\0') {
        lv_label_set_text(s_mbta_title, st.title);
    }

    if (!st.has_data) {
        lv_label_set_text(s_mbta_big_minutes, "--");
        lv_obj_add_flag(s_mbta_big_suffix, LV_OBJ_FLAG_HIDDEN);

        if (strcmp(st.title, "Sleep Mode") == 0) {
            lv_obj_add_flag(s_mbta_big_box, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_mbta_row1, "Service resumes at 6am");
        } else {
            lv_obj_clear_flag(s_mbta_big_box, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_mbta_row1, "No data");
        }
        lv_label_set_text(s_mbta_row2, "");
        return;
    }

    lv_obj_clear_flag(s_mbta_big_box, LV_OBJ_FLAG_HIDDEN);

    if (st.arrival_count <= 0) {
        lv_label_set_text(s_mbta_big_minutes, "--");
        lv_obj_add_flag(s_mbta_big_suffix, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mbta_row1, "No upcoming arrivals");
        lv_label_set_text(s_mbta_row2, "");
        return;
    }

    char buf[32];
    if (st.arrivals_min[0] <= 0) {
        lv_label_set_text(s_mbta_big_minutes, "ARR");
        lv_obj_add_flag(s_mbta_big_suffix, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(buf, sizeof(buf), "%d", st.arrivals_min[0]);
        lv_label_set_text(s_mbta_big_minutes, buf);
        lv_obj_clear_flag(s_mbta_big_suffix, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.arrival_count >= 2) {
        snprintf(buf, sizeof(buf), "Next: %d min", st.arrivals_min[1]);
        lv_label_set_text(s_mbta_row1, buf);
    } else {
        lv_label_set_text(s_mbta_row1, "");
    }

    if (st.arrival_count >= 3) {
        snprintf(buf, sizeof(buf), "Then: %d min", st.arrivals_min[2]);
        lv_label_set_text(s_mbta_row2, buf);
    } else {
        lv_label_set_text(s_mbta_row2, "");
    }
}

static lv_obj_t *s_wifi_bar;
static lv_obj_t *s_wifi_label;
static wireless_status_t s_last_wifi_status = (wireless_status_t)255;

static void ui_wifi_status_init(void)
{
    // Put WiFi status on the top layer so it persists across screen changes.
    s_wifi_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_wifi_bar, lv_pct(100), 18);
    lv_obj_align(s_wifi_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(s_wifi_bar, 0, 0);
    lv_obj_set_style_border_width(s_wifi_bar, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_bar, 4, 0);
    lv_obj_set_style_bg_color(s_wifi_bar, lv_color_hex(0xFBC02D), 0);
    lv_obj_set_flex_flow(s_wifi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wifi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_wifi_label = lv_label_create(s_wifi_bar);
    lv_label_set_text(s_wifi_label, "Connecting");

#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_12, 0);
#endif
}

static void ui_wifi_status_update(void)
{
    wireless_status_t status = Wireless_GetStatus();
    if (status == s_last_wifi_status) {
        return;
    }

    s_last_wifi_status = status;

    switch (status) {
    case WIRELESS_STATUS_CONNECTED:
        lv_obj_set_style_bg_color(s_wifi_bar, lv_color_hex(0x2E7D32), 0);
        lv_obj_set_style_text_color(s_wifi_label, lv_color_white(), 0);
        lv_label_set_text(s_wifi_label, "Connected");
        break;
    case WIRELESS_STATUS_FAILED:
        lv_obj_set_style_bg_color(s_wifi_bar, lv_color_hex(0xE57373), 0);
        lv_obj_set_style_text_color(s_wifi_label, lv_color_black(), 0);
        lv_label_set_text(s_wifi_label, "Failed");
        break;
    case WIRELESS_STATUS_CONNECTING:
    default:
        lv_obj_set_style_bg_color(s_wifi_bar, lv_color_hex(0xFBC02D), 0);
        lv_obj_set_style_text_color(s_wifi_label, lv_color_black(), 0);
        lv_label_set_text(s_wifi_label, "Connecting");
        break;
    }
}

void app_main(void)
{
    // US Eastern with DST rules (set early for UI)
    setenv("TZ", DEFAULT_TIMEZONE, 1);
    tzset();

    // Ensure the onboard RGB/status LED cannot light from a floating data pin.
    RGB_ForceOff();

    LCD_Init();
    LVGL_Init();

    // Set brightness after all hardware init is complete
    ESP_LOGI("MAIN", "Applying brightness: %d%%", MBTA_BRIGHTNESS_PCT);
    BK_Light(MBTA_BRIGHTNESS_PCT);

    // Create two screens and switch between them as needed.
    s_screen_mbta = lv_obj_create(NULL);
    ui_set_screen_bg(s_screen_mbta);
    s_screen_weather = lv_obj_create(NULL);
    ui_set_screen_bg(s_screen_weather);

    ui_wifi_status_init();

    // Init UI objects on their respective screens.
    ui_mbta_init(s_screen_mbta);
    ui_weather_init(s_screen_weather);

    Wireless_Init();
    Weather_TaskStart();
    if (!UI_FORCE_WEATHER) {
        MBTA_TaskStart();
    }

    // Initial screen
    if (UI_FORCE_WEATHER) {
        s_ui_mode = UI_MODE_WEATHER;
        lv_scr_load(s_screen_weather);
    } else {
        s_ui_mode = UI_MODE_MBTA;
        lv_scr_load(s_screen_mbta);
    }

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
        ui_wifi_status_update();

        // Update both screens (objects can be updated even when not active).
        ui_weather_update();
        if (!UI_FORCE_WEATHER) {
            ui_mbta_update();

            // Switch to Weather when MBTA is in its scheduled "display_off" period.
            mbta_state_t st;
            if (MBTA_GetState(&st)) {
                ui_switch_mode(st.display_off ? UI_MODE_WEATHER : UI_MODE_MBTA);
            }
        }
    }
}
