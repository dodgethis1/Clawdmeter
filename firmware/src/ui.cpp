#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <string.h>
#include "logo.h"
#include "icons.h"
#include "usage_history.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_bar_h;
    int16_t usage_arc_size;
    int16_t usage_arc_width;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_pill_font;
    const lv_font_t* usage_reset_font;
    const lv_font_t* usage_bar_label_font;   // reset time printed on the bar

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        // Two oversized panels (Current / Weekly) fill the screen: giant
        // percentage, thick bar, and a donut gauge — readable from across
        // the room.
        L.content_y = 90;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 10;
        L.usage_bar_y = 90;
        L.usage_bar_h = 34;
        L.usage_arc_size = 118;
        L.usage_arc_width = 16;
        L.usage_pct_font = &font_styrene_48;
        L.usage_pill_font = &font_styrene_24;
        L.usage_reset_font = &font_styrene_20;
        L.usage_bar_label_font = &font_styrene_28;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8), two big panels.
        L.content_y = 70;
        L.usage_panel_h = 118;
        L.usage_panel_gap = 8;
        L.usage_bar_y = 58;
        L.usage_bar_h = 24;
        L.usage_arc_size = 84;
        L.usage_arc_width = 12;
        L.usage_pct_font = &font_styrene_28;
        L.usage_pill_font = &font_styrene_16;
        L.usage_reset_font = &font_styrene_16;
        L.usage_bar_label_font = &font_styrene_16;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* arc_session;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* arc_weekly;
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- History screen widgets ----
static lv_obj_t* history_container;
static lv_obj_t* hist_chart;
static lv_chart_series_t* hist_series;
static lv_obj_t* lbl_hist_peak;
static lv_obj_t* lbl_hist_avg;
static lv_obj_t* lbl_hist_resets;
static lv_obj_t* lbl_anim_hist;

// ---- Limited (takeover) overlay widgets ----
static lv_obj_t* limited_overlay;
static lv_obj_t* limited_flash;
static lv_obj_t* lbl_lim_pill;
static lv_obj_t* lbl_lim_countdown;
static lv_obj_t* lbl_lim_sub;
static lv_obj_t* bar_lim_drain;
static lv_obj_t* lbl_lim_weekly;
static lv_obj_t* lbl_anim_lim;

// ---- Limited / milestone state ----
static bool     s_limited = false;
static uint32_t data_rx_ms = 0;             // lv_tick when last data arrived
static int      s_session_reset_mins = -1;  // from last data
static float    prev_session_pct = -1.0f;
static float    prev_weekly_pct  = -1.0f;
static uint32_t moment_until_ms = 0;        // splash "moment" override active
static screen_t moment_prev = SCREEN_USAGE;
static const char* moment_status = NULL;

#define SESSION_WINDOW_MINS (5 * 60)   // drain bar scale: 5h session window
#define MILESTONE_HOT_PCT   80.0f
#define WEEKLY_RESET_DROP   30.0f

static const char* const nap_messages[] = {
    "Napping", "Dreaming", "Recharging", "Hibernating",
    "Snoozing", "Counting tokens", "Resting", "Regenerating",
};
#define NAP_MSG_COUNT (sizeof(nap_messages) / sizeof(nap_messages[0]))      // status line: connection state + whimsical idle

static lv_obj_t* logo_img;

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static screen_t prev_non_splash_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.usage_pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 14, 0);
    lv_obj_set_style_pad_right(lbl, 14, 0);
    lv_obj_set_style_pad_top(lbl, 4, 0);
    lv_obj_set_style_pad_bottom(lbl, 4, 0);
    return lbl;
}

// ======== Usage Screen ========

// Donut gauge: a full-circle arc that fills clockwise from 12 o'clock, with
// a short window label ("5h" / "7d") in the hole.
static lv_obj_t* make_donut(lv_obj_t* parent, const char* center_text) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, L.usage_arc_size, L.usage_arc_size);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_arc_width(arc, L.usage_arc_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, L.usage_arc_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_align(arc, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* center = lv_label_create(arc);
    lv_label_set_text(center, center_text);
    lv_obj_set_style_text_font(center, L.usage_pill_font, 0);
    lv_obj_set_style_text_color(center, COL_DIM, 0);
    lv_obj_center(center);
    return arc;
}

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             const char* window_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset,
                             lv_obj_t** out_arc) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_LEFT, 0, 0);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, L.usage_pill_font->line_height + 8);

    int bar_w = L.content_w - 32 - L.usage_arc_size - 14;
    *out_bar = make_bar(panel, 0, L.usage_bar_y, bar_w, L.usage_bar_h);

    // Reset time rides on the bar itself — big print, zero extra space.
    *out_reset = lv_label_create(*out_bar);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.usage_bar_label_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_TEXT, 0);
    lv_obj_center(*out_reset);

    *out_arc = make_donut(panel, window_text);
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// ======== History Screen ========

// Recolor each chart segment by its value: green / amber / red, matching the
// usage-bar thresholds.
static void hist_chart_draw_cb(lv_event_t* e) {
    lv_draw_task_t* t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t* base = (lv_draw_dsc_base_t*)lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS) return;
    lv_draw_line_dsc_t* ld = lv_draw_task_get_line_dsc(t);
    if (!ld) return;
    int cnt = usage_history_count();
    int idx = (int)base->id2 - (HISTORY_CAP - cnt);   // chart is right-aligned
    ld->color = pct_color((float)usage_history_at(idx));
}

static lv_obj_t* make_stat_cell(lv_obj_t* parent, int x, int y, int w, int h,
                                const char* caption, lv_obj_t** out_value) {
    lv_obj_t* cell = make_panel(parent, x, y, w, h);
    lv_obj_set_style_pad_top(cell, 6, 0);
    lv_obj_set_style_pad_bottom(cell, 6, 0);

    lv_obj_t* cap = lv_label_create(cell);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(cap, COL_DIM, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 0);

    *out_value = lv_label_create(cell);
    lv_label_set_text(*out_value, "--");
    lv_obj_set_style_text_font(*out_value, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(*out_value, COL_TEXT, 0);
    lv_obj_align(*out_value, LV_ALIGN_BOTTOM_MID, 0, 0);
    return cell;
}

static void init_history_screen(lv_obj_t* scr) {
    history_container = lv_obj_create(scr);
    lv_obj_set_size(history_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(history_container, 0, 0);
    lv_obj_set_style_bg_opa(history_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(history_container, 0, 0);
    lv_obj_set_style_pad_all(history_container, 0, 0);
    lv_obj_clear_flag(history_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(history_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(history_container);
    lv_label_set_text(title, "History");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Geometry derived from the shared layout so both boards work.
    int axis_w   = 38;                                   // y-label gutter
    int chart_x  = L.margin + axis_w;
    int chart_y  = L.content_y + 14;
    int chart_w  = L.content_w - axis_w;
    int chart_h  = (L.scr_h - L.content_y) * 44 / 100;

    hist_chart = lv_chart_create(history_container);
    lv_obj_set_pos(hist_chart, chart_x, chart_y);
    lv_obj_set_size(hist_chart, chart_w, chart_h);
    lv_chart_set_type(hist_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(hist_chart, HISTORY_CAP);
    lv_chart_set_range(hist_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(hist_chart, 3, 0);
    lv_obj_set_style_bg_color(hist_chart, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(hist_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hist_chart, 0, 0);
    lv_obj_set_style_radius(hist_chart, 8, 0);
    lv_obj_set_style_line_color(hist_chart, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_line_width(hist_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(hist_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(hist_chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(hist_chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(hist_chart, hist_chart_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    hist_series = lv_chart_add_series(hist_chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(hist_chart, hist_series, LV_CHART_POINT_NONE);

    // Dashed 80% warning line over the chart.
    static lv_point_precise_t limit_pts[2];
    int y80 = chart_y + chart_h * 20 / 100;
    limit_pts[0].x = chart_x;           limit_pts[0].y = y80;
    limit_pts[1].x = chart_x + chart_w; limit_pts[1].y = y80;
    lv_obj_t* limit_line = lv_line_create(history_container);
    lv_line_set_points(limit_line, limit_pts, 2);
    lv_obj_set_style_line_color(limit_line, COL_RED, 0);
    lv_obj_set_style_line_width(limit_line, 2, 0);
    lv_obj_set_style_line_dash_width(limit_line, 5, 0);
    lv_obj_set_style_line_dash_gap(limit_line, 6, 0);
    lv_obj_set_style_line_opa(limit_line, LV_OPA_60, 0);

    // Y-axis labels.
    const struct { const char* txt; int pct; } ylabels[] =
        { {"80", 80}, {"50", 50}, {"0", 0} };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* yl = lv_label_create(history_container);
        lv_label_set_text(yl, ylabels[i].txt);
        lv_obj_set_style_text_font(yl, L.usage_reset_font, 0);
        lv_obj_set_style_text_color(yl, COL_DIM, 0);
        lv_obj_set_pos(yl, L.margin, chart_y + chart_h * (100 - ylabels[i].pct) / 100 - 10);
    }

    // X-axis labels.
    const char* const xlabels[] = { "-8h", "-6h", "-4h", "-2h", "now" };
    for (int i = 0; i < 5; i++) {
        lv_obj_t* xl = lv_label_create(history_container);
        lv_label_set_text(xl, xlabels[i]);
        lv_obj_set_style_text_font(xl, L.usage_reset_font, 0);
        lv_obj_set_style_text_color(xl, i == 4 ? COL_TEXT : COL_DIM, 0);
        lv_obj_set_pos(xl, chart_x + (chart_w - 44) * i / 4, chart_y + chart_h + 6);
    }

    // Stats row: peak / avg / resets.
    int stats_y = chart_y + chart_h + 38;
    int stats_h = L.usage_panel_h * 68 / 100;
    int cell_w  = (L.content_w - 20) / 3;
    make_stat_cell(history_container, L.margin, stats_y, cell_w, stats_h,
                   "peak", &lbl_hist_peak);
    make_stat_cell(history_container, L.margin + cell_w + 10, stats_y, cell_w, stats_h,
                   "avg", &lbl_hist_avg);
    make_stat_cell(history_container, L.margin + 2 * (cell_w + 10), stats_y, cell_w, stats_h,
                   "resets", &lbl_hist_resets);

    lbl_anim_hist = lv_label_create(history_container);
    lv_label_set_text(lbl_anim_hist, "");
    lv_obj_set_style_text_font(lbl_anim_hist, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim_hist, COL_ACCENT, 0);
    lv_obj_align(lbl_anim_hist, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_obj_add_flag(history_container, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_history_widgets(void) {
    if (!hist_chart) return;
    int cnt = usage_history_count();
    lv_chart_set_all_value(hist_chart, hist_series, LV_CHART_POINT_NONE);
    for (int i = 0; i < cnt; i++) {
        lv_chart_set_value_by_id(hist_chart, hist_series,
                                 HISTORY_CAP - cnt + i, usage_history_at(i));
    }
    lv_label_set_text_fmt(lbl_hist_peak, "%d%%", usage_history_peak());
    lv_label_set_text_fmt(lbl_hist_avg, "%d%%", usage_history_avg());
    lv_label_set_text_fmt(lbl_hist_resets, "%d", usage_history_resets());
    lv_obj_set_style_text_color(lbl_hist_peak,
                                pct_color((float)usage_history_peak()), 0);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    make_usage_panel(usage_group, L.content_y, "Current", "5h",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset, &arc_session);
    make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly", "7d",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset, &arc_weekly);

    build_pair_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Limited (rate-limit takeover) ========

static void pill_pulse_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

static void flash_fade_cb(void* obj, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

static void flash_done_cb(lv_anim_t* a) {
    lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void init_limited_overlay(lv_obj_t* scr) {
    limited_overlay = lv_obj_create(scr);
    lv_obj_set_size(limited_overlay, L.scr_w, L.scr_h);
    lv_obj_set_pos(limited_overlay, 0, 0);
    lv_obj_set_style_bg_opa(limited_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(limited_overlay, 0, 0);
    lv_obj_set_style_pad_all(limited_overlay, 0, 0);
    lv_obj_clear_flag(limited_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(limited_overlay, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_lim_pill = lv_label_create(limited_overlay);
    lv_label_set_text(lbl_lim_pill, "Rate limited");
    lv_obj_set_style_text_font(lbl_lim_pill, L.usage_pill_font, 0);
    lv_obj_set_style_text_color(lbl_lim_pill, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl_lim_pill, COL_RED, 0);
    lv_obj_set_style_bg_opa(lbl_lim_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_lim_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl_lim_pill, 18, 0);
    lv_obj_set_style_pad_right(lbl_lim_pill, 18, 0);
    lv_obj_set_style_pad_top(lbl_lim_pill, 6, 0);
    lv_obj_set_style_pad_bottom(lbl_lim_pill, 6, 0);
    lv_obj_align(lbl_lim_pill, LV_ALIGN_TOP_MID, 0, 18);

    lbl_lim_countdown = lv_label_create(limited_overlay);
    lv_label_set_text(lbl_lim_countdown, "-:--:--");
    lv_obj_set_style_text_font(lbl_lim_countdown, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(lbl_lim_countdown, COL_TEXT, 0);
    lv_obj_align(lbl_lim_countdown, LV_ALIGN_BOTTOM_MID, 0, -140);

    lbl_lim_sub = lv_label_create(limited_overlay);
    lv_label_set_text(lbl_lim_sub, "until session resets");
    lv_obj_set_style_text_font(lbl_lim_sub, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(lbl_lim_sub, COL_DIM, 0);
    lv_obj_align(lbl_lim_sub, LV_ALIGN_BOTTOM_MID, 0, -110);

    bar_lim_drain = make_bar(limited_overlay, L.margin + 20, 0,
                             L.content_w - 40, 8);
    lv_obj_align(bar_lim_drain, LV_ALIGN_BOTTOM_MID, 0, -88);
    lv_obj_set_style_bg_color(bar_lim_drain, COL_ACCENT, LV_PART_INDICATOR);

    lbl_lim_weekly = lv_label_create(limited_overlay);
    lv_label_set_text(lbl_lim_weekly, "");
    lv_obj_set_style_text_font(lbl_lim_weekly, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(lbl_lim_weekly, COL_DIM, 0);
    lv_obj_align(lbl_lim_weekly, LV_ALIGN_BOTTOM_MID, 0, -58);

    lbl_anim_lim = lv_label_create(limited_overlay);
    lv_label_set_text(lbl_anim_lim, "");
    lv_obj_set_style_text_font(lbl_anim_lim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim_lim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim_lim, LV_ALIGN_BOTTOM_MID, 0, -15);

    // Full-screen red flash used on takeover entry (topmost, hidden).
    limited_flash = lv_obj_create(scr);
    lv_obj_set_size(limited_flash, L.scr_w, L.scr_h);
    lv_obj_set_pos(limited_flash, 0, 0);
    lv_obj_set_style_bg_color(limited_flash, COL_RED, 0);
    lv_obj_set_style_bg_opa(limited_flash, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(limited_flash, 0, 0);
    lv_obj_clear_flag(limited_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(limited_flash, LV_OBJ_FLAG_HIDDEN);

    // Slow breathing pulse on the pill, forever.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl_lim_pill);
    lv_anim_set_exec_cb(&a, pill_pulse_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_50);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_playback_duration(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_add_flag(limited_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void play_entry_flash(void) {
    if (!limited_flash) return;
    lv_obj_clear_flag(limited_flash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(limited_flash);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, limited_flash);
    lv_anim_set_exec_cb(&a, flash_fade_cb);
    lv_anim_set_values(&a, LV_OPA_70, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_repeat_count(&a, 2);
    lv_anim_set_completed_cb(&a, flash_done_cb);
    lv_anim_start(&a);
}

// Splash "moment": briefly force a named animation (milestones, limit-exit
// celebration), then return to wherever we were.
static void play_moment(const char* anim, uint32_t ms, const char* status) {
    if (s_limited) return;
    moment_prev = (current_screen == SCREEN_SPLASH || current_screen == SCREEN_LIMITED)
                      ? prev_non_splash_screen : current_screen;
    moment_status = status;
    moment_until_ms = lv_tick_get() + ms;
    splash_force_anim(anim);
    ui_show_screen(SCREEN_SPLASH);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    init_usage_screen(scr);
    init_history_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    // Overlay last so it stacks above the splash canvas.
    init_limited_overlay(scr);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    data_rx_ms = lv_tick_get();
    s_session_reset_mins = data->session_reset_mins;

    // ---- Rate-limit takeover state machine ----
    bool lim = (strcmp(data->status, "limited") == 0);
    if (lim && !s_limited) {
        s_limited = true;
        moment_until_ms = 0;
        ui_show_screen(SCREEN_LIMITED);
        play_entry_flash();
    } else if (!lim && s_limited) {
        s_limited = false;
        splash_clear_force();
        play_moment("dance djmix", 8000, "Back in business");
    }

    // ---- Milestone moments (skip while limited or mid-moment) ----
    if (!s_limited && moment_until_ms == 0) {
        if (prev_session_pct >= 0.0f &&
            prev_session_pct < MILESTONE_HOT_PCT &&
            data->session_pct >= MILESTONE_HOT_PCT) {
            play_moment("expression surprise", 6000, "Redlining");
        } else if (prev_weekly_pct >= 0.0f &&
                   prev_weekly_pct - data->weekly_pct >= WEEKLY_RESET_DROP) {
            play_moment("dance djmix", 8000, "Fresh week");
        }
    }
    prev_session_pct = data->session_pct;
    prev_weekly_pct  = data->weekly_pct;

    // ---- History buffer + widgets ----
    usage_history_sample(data->session_pct);
    refresh_history_widgets();

    // ---- Limited overlay extras ----
    if (lbl_lim_weekly) {
        char wbuf[64];
        if (data->weekly_reset_mins >= 0) {
            snprintf(wbuf, sizeof(wbuf), "weekly %d%%  \xC2\xB7  %dd %dh",
                     (int)(data->weekly_pct + 0.5f),
                     data->weekly_reset_mins / 1440,
                     (data->weekly_reset_mins % 1440) / 60);
        } else {
            snprintf(wbuf, sizeof(wbuf), "weekly %d%%",
                     (int)(data->weekly_pct + 0.5f));
        }
        lv_label_set_text(lbl_lim_weekly, wbuf);
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);
    lv_arc_set_value(arc_session, s_pct);
    lv_obj_set_style_arc_color(arc_session, pct_color(data->session_pct), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(lbl_session_pct, pct_color(data->session_pct), 0);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
    lv_arc_set_value(arc_weekly, w_pct);
    lv_obj_set_style_arc_color(arc_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(lbl_weekly_pct, pct_color(data->weekly_pct), 0);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_tick_anim(void) {
    uint32_t now = lv_tick_get();

    // ---- Moment expiry: return to wherever we were ----
    if (moment_until_ms != 0 && (int32_t)(now - moment_until_ms) >= 0) {
        moment_until_ms = 0;
        moment_status = NULL;
        splash_clear_force();
        ui_show_screen(moment_prev);
    }

    // ---- Live countdown while limited (once a second is plenty) ----
    if (s_limited && lbl_lim_countdown && s_session_reset_mins >= 0) {
        static uint32_t last_cd_ms = 0;
        if (now - last_cd_ms >= 1000) {
            last_cd_ms = now;
            int64_t remaining_ms =
                (int64_t)s_session_reset_mins * 60000 - (int64_t)(now - data_rx_ms);
            if (remaining_ms < 0) remaining_ms = 0;
            uint32_t secs = remaining_ms / 1000;
            lv_label_set_text_fmt(lbl_lim_countdown, "%u:%02u:%02u",
                                  (unsigned)(secs / 3600),
                                  (unsigned)((secs % 3600) / 60),
                                  (unsigned)(secs % 60));
            int64_t window_ms = (int64_t)SESSION_WINDOW_MINS * 60000;
            int drained = (int)(100 - remaining_ms * 100 / window_ms);
            if (drained < 0) drained = 0;
            if (drained > 100) drained = 100;
            lv_bar_set_value(bar_lim_drain, drained, LV_ANIM_ON);
        }
    }

    // ---- Status line: pick the label on the visible screen ----
    lv_obj_t* target;
    switch (current_screen) {
    case SCREEN_USAGE:   target = lbl_anim;      break;
    case SCREEN_HISTORY: target = lbl_anim_hist; break;
    case SCREEN_LIMITED: target = lbl_anim_lim;  break;
    default:             return;
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (moment_status) {
        text = moment_status;
    } else if (s_limited) {
        text = nap_messages[anim_msg_idx % NAP_MSG_COUNT];
    } else if (!s_ble_connected) {
        text = ble_has_bonds() ? "Disconnected" : "Pairing";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(target, buf);
}

// ---- Auto-cycle: alternate Usage <-> Activity (splash). A tap switches
// immediately and pauses the cycle so it doesn't fight the user.
#define SCREEN_CYCLE_INTERVAL_MS 12000
#define SCREEN_CYCLE_TAP_PAUSE_MS 30000
static uint32_t next_auto_cycle_ms = SCREEN_CYCLE_INTERVAL_MS;

// Usage -> History -> Activity (splash) -> Usage.
static screen_t next_cycle_screen(screen_t s) {
    switch (s) {
    case SCREEN_USAGE:   return SCREEN_HISTORY;
    case SCREEN_HISTORY: return SCREEN_SPLASH;
    default:             return SCREEN_USAGE;
    }
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (s_limited) return;                  // takeover owns the screen
    if (moment_until_ms != 0) {             // tap skips a moment early
        moment_until_ms = 0;
        moment_status = NULL;
        splash_clear_force();
        ui_show_screen(moment_prev);
    } else {
        ui_show_screen(next_cycle_screen(current_screen));
    }
    next_auto_cycle_ms = lv_tick_get() + SCREEN_CYCLE_TAP_PAUSE_MS;
}

void ui_tick_screen_cycle(void) {
    if (s_limited || moment_until_ms != 0) return;
    if ((int32_t)(lv_tick_get() - next_auto_cycle_ms) < 0) return;
    ui_show_screen(next_cycle_screen(current_screen));
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (history_container) lv_obj_add_flag(history_container, LV_OBJ_FLAG_HIDDEN);
    if (limited_overlay)   lv_obj_add_flag(limited_overlay, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_HISTORY: lv_obj_clear_flag(history_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_LIMITED:
        // Sleeping creature on the splash canvas + info overlay on top.
        splash_force_anim("expression sleep");
        splash_show();
        lv_obj_clear_flag(limited_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(limited_overlay);
        break;
    default: break;
    }

    bool logo_hidden = (screen == SCREEN_SPLASH || screen == SCREEN_LIMITED);
    if (logo_img) {
        if (logo_hidden) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen == SCREEN_USAGE || screen == SCREEN_HISTORY)
        prev_non_splash_screen = screen;
    current_screen = screen;
    next_auto_cycle_ms = lv_tick_get() + SCREEN_CYCLE_INTERVAL_MS;
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    // Connected → usage panels; otherwise → pairing hint. The bottom status
    // line carries the live state word (Connected / Disconnected / Pairing).
    if (usage_group && pair_group) {
        if (s_ble_connected) {
            lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
}


