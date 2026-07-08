#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <string.h>
#include "logo.h"
#include "icons.h"
#include "usage_history.h"
#include "weekly_history.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
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

// ---- Trend pages (History = 8h session, Weekly = 7d) ----
typedef uint8_t (*trend_at_fn)(int);
typedef int (*trend_cnt_fn)(void);

struct TrendPage {
    lv_obj_t* container;
    lv_obj_t* chart;
    lv_chart_series_t* series;
    lv_obj_t* stat_val[3];
    lv_obj_t* anim_lbl;
    int cap;
    trend_at_fn at;
    trend_cnt_fn cnt;
};
static TrendPage pg_history;
static TrendPage pg_weekly;

// ---- Models screen widgets ----
// Adaptive: 1-2 buckets render as full-size usage panels (donut and all),
// 3-4 fall back to compact rows.
static lv_obj_t* models_container;
static lv_obj_t* mdl_panel[MODEL_BUCKETS_MAX];
static lv_obj_t* mdl_pill[MODEL_BUCKETS_MAX];
static lv_obj_t* mdl_pct[MODEL_BUCKETS_MAX];
static lv_obj_t* mdl_bar[MODEL_BUCKETS_MAX];
static lv_obj_t* mdl_reset[MODEL_BUCKETS_MAX];
static lv_obj_t* mdl_big_panel[2];
static lv_obj_t* mdl_big_pill[2];
static lv_obj_t* mdl_big_pct[2];
static lv_obj_t* mdl_big_bar[2];
static lv_obj_t* mdl_big_reset[2];
static lv_obj_t* mdl_big_arc[2];
static lv_obj_t* lbl_anim_models;
static int s_model_count = 0;

// ---- Clock (top right, where the battery indicator used to live) ----
static lv_obj_t* lbl_clock;
static int clock_base_mins = -1;   // daemon local time at data_rx_ms; -1 = unknown

// ---- Time-to-limit projection ----
static char  proj_bar_str[32] = "";       // "Limit ~9:40p" ("" = no projection)
static char  proj_short_str[12] = "";     // "~9:40p" for the donut hole
static char  session_reset_str[32] = "---";
static int   proj_mins_to = -1;
static bool  bar_show_proj = false;
static uint32_t bar_flip_ms = 0;
static lv_obj_t* lbl_session_window;      // session donut center label
#define BAR_FLIP_MS      5000
#define PROJ_MIN_SLOPE   0.05f            // %/min; below this, no projection

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
static lv_obj_t* ui_root;   // everything lives here; orbits for burn-in
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

// 12-hour clock: "9:41p". mins is minutes since midnight.
static void format_clock_time(int mins, char* buf, size_t len) {
    int h24 = (mins / 60) % 24;
    int m = mins % 60;
    int h12 = h24 % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, len, "%d:%02d%s", h12, m, h24 < 12 ? "a" : "p");
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
static lv_obj_t* make_donut(lv_obj_t* parent, const char* center_text,
                            lv_obj_t** out_center) {
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
    lv_obj_set_style_text_font(center, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(center, COL_DIM, 0);
    lv_obj_center(center);
    if (out_center) *out_center = center;
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

    lv_obj_t* center = NULL;
    *out_arc = make_donut(panel, window_text, &center);
    if (out_pct == &lbl_session_pct) lbl_session_window = center;
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

// ======== Trend pages (History / Weekly) ========

// Recolor each chart segment by its value: green / amber / red, matching the
// usage-bar thresholds. Generic: the page struct arrives as user data.
static void trend_draw_cb(lv_event_t* e) {
    lv_draw_task_t* t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t* base = (lv_draw_dsc_base_t*)lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS) return;
    lv_draw_line_dsc_t* ld = lv_draw_task_get_line_dsc(t);
    if (!ld) return;
    TrendPage* p = (TrendPage*)lv_event_get_user_data(e);
    int idx = (int)base->id2 - (p->cap - p->cnt());   // chart is right-aligned
    ld->color = pct_color((float)p->at(idx));
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

// Build a full chart page: title, zone-colored line chart, dashed 80% line,
// axis labels, three stat cells, status line. Used by History and Weekly.
static void build_trend_page(TrendPage* p, lv_obj_t* scr, const char* title_txt,
                             int cap, const char* const xlabels[5],
                             const char* const captions[3],
                             trend_at_fn at, trend_cnt_fn cnt) {
    p->cap = cap;
    p->at = at;
    p->cnt = cnt;

    p->container = lv_obj_create(scr);
    lv_obj_set_size(p->container, L.scr_w, L.scr_h);
    lv_obj_set_pos(p->container, 0, 0);
    lv_obj_set_style_bg_opa(p->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->container, 0, 0);
    lv_obj_set_style_pad_all(p->container, 0, 0);
    lv_obj_clear_flag(p->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(p->container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(p->container);
    lv_label_set_text(title, title_txt);
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Geometry derived from the shared layout so both boards work.
    int axis_w   = 38;                                   // y-label gutter
    int chart_x  = L.margin + axis_w;
    int chart_y  = L.content_y + 14;
    int chart_w  = L.content_w - axis_w;
    int chart_h  = (L.scr_h - L.content_y) * 44 / 100;

    p->chart = lv_chart_create(p->container);
    lv_obj_set_pos(p->chart, chart_x, chart_y);
    lv_obj_set_size(p->chart, chart_w, chart_h);
    lv_chart_set_type(p->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(p->chart, cap);
    lv_chart_set_range(p->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(p->chart, 3, 0);
    lv_obj_set_style_bg_color(p->chart, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(p->chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->chart, 0, 0);
    lv_obj_set_style_radius(p->chart, 8, 0);
    lv_obj_set_style_line_color(p->chart, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_line_width(p->chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(p->chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(p->chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(p->chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(p->chart, trend_draw_cb, LV_EVENT_DRAW_TASK_ADDED, p);
    p->series = lv_chart_add_series(p->chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(p->chart, p->series, LV_CHART_POINT_NONE);

    // Dashed 80% warning line over the chart.
    lv_point_precise_t* limit_pts =
        (lv_point_precise_t*)lv_malloc(2 * sizeof(lv_point_precise_t));
    int y80 = chart_y + chart_h * 20 / 100;
    limit_pts[0].x = chart_x;           limit_pts[0].y = y80;
    limit_pts[1].x = chart_x + chart_w; limit_pts[1].y = y80;
    lv_obj_t* limit_line = lv_line_create(p->container);
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
        lv_obj_t* yl = lv_label_create(p->container);
        lv_label_set_text(yl, ylabels[i].txt);
        lv_obj_set_style_text_font(yl, L.usage_reset_font, 0);
        lv_obj_set_style_text_color(yl, COL_DIM, 0);
        lv_obj_set_pos(yl, L.margin, chart_y + chart_h * (100 - ylabels[i].pct) / 100 - 10);
    }

    // X-axis labels.
    for (int i = 0; i < 5; i++) {
        lv_obj_t* xl = lv_label_create(p->container);
        lv_label_set_text(xl, xlabels[i]);
        lv_obj_set_style_text_font(xl, L.usage_reset_font, 0);
        lv_obj_set_style_text_color(xl, i == 4 ? COL_TEXT : COL_DIM, 0);
        lv_obj_set_pos(xl, chart_x + (chart_w - 44) * i / 4, chart_y + chart_h + 6);
    }

    // Stats row.
    int stats_y = chart_y + chart_h + 38;
    int stats_h = L.usage_panel_h * 68 / 100;
    int cell_w  = (L.content_w - 20) / 3;
    for (int i = 0; i < 3; i++) {
        make_stat_cell(p->container, L.margin + i * (cell_w + 10), stats_y,
                       cell_w, stats_h, captions[i], &p->stat_val[i]);
    }

    p->anim_lbl = lv_label_create(p->container);
    lv_label_set_text(p->anim_lbl, "");
    lv_obj_set_style_text_font(p->anim_lbl, &font_mono_32, 0);
    lv_obj_set_style_text_color(p->anim_lbl, COL_ACCENT, 0);
    lv_obj_align(p->anim_lbl, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_obj_add_flag(p->container, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_trend_chart(TrendPage* p) {
    if (!p->chart) return;
    int cnt = p->cnt();
    lv_chart_set_all_value(p->chart, p->series, LV_CHART_POINT_NONE);
    for (int i = 0; i < cnt; i++) {
        lv_chart_set_value_by_id(p->chart, p->series, p->cap - cnt + i, p->at(i));
    }
}

// ======== Models Screen ========

// Defined in the Usage section below; reused for prominent model panels.
static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             const char* window_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset,
                             lv_obj_t** out_arc);

static void init_models_screen(lv_obj_t* scr) {
    models_container = lv_obj_create(scr);
    lv_obj_set_size(models_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(models_container, 0, 0);
    lv_obj_set_style_bg_opa(models_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(models_container, 0, 0);
    lv_obj_set_style_pad_all(models_container, 0, 0);
    lv_obj_clear_flag(models_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(models_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(models_container);
    lv_label_set_text(title, "Models");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, L.title_y);

    int avail = L.scr_h - L.content_y - 70;   // leave room for the status line
    int gap = 8;
    int mh = (avail - (MODEL_BUCKETS_MAX - 1) * gap) / MODEL_BUCKETS_MAX;

    for (int i = 0; i < MODEL_BUCKETS_MAX; i++) {
        int y = L.content_y + i * (mh + gap);
        mdl_panel[i] = make_panel(models_container, L.margin, y, L.content_w, mh);

        mdl_pill[i] = make_pill(mdl_panel[i], "Model");
        lv_obj_align(mdl_pill[i], LV_ALIGN_TOP_LEFT, 0, 0);

        mdl_pct[i] = lv_label_create(mdl_panel[i]);
        lv_label_set_text(mdl_pct[i], "---%");
        lv_obj_set_style_text_font(mdl_pct[i], &font_styrene_28, 0);
        lv_obj_set_style_text_color(mdl_pct[i], COL_TEXT, 0);
        lv_obj_align(mdl_pct[i], LV_ALIGN_TOP_RIGHT, 0, 0);

        int bar_y = mh - 24 - 20;   // panel pad bottom 12 + bar height margin
        mdl_bar[i] = make_bar(mdl_panel[i], 0, bar_y, L.content_w - 32, 20);

        mdl_reset[i] = lv_label_create(mdl_bar[i]);
        lv_label_set_text(mdl_reset[i], "---");
        lv_obj_set_style_text_font(mdl_reset[i], &font_styrene_16, 0);
        lv_obj_set_style_text_color(mdl_reset[i], COL_TEXT, 0);
        lv_obj_center(mdl_reset[i]);

        lv_obj_add_flag(mdl_panel[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Full-size panels for the 1-2 bucket case — same look as the Usage page.
    for (int i = 0; i < 2; i++) {
        make_usage_panel(models_container,
                         L.content_y + i * (L.usage_panel_h + L.usage_panel_gap),
                         "Model", "7d",
                         &mdl_big_pct[i], &mdl_big_pill[i],
                         &mdl_big_bar[i], &mdl_big_reset[i], &mdl_big_arc[i]);
        mdl_big_panel[i] = lv_obj_get_parent(mdl_big_pct[i]);
        lv_obj_add_flag(mdl_big_panel[i], LV_OBJ_FLAG_HIDDEN);
    }

    lbl_anim_models = lv_label_create(models_container);
    lv_label_set_text(lbl_anim_models, "");
    lv_obj_set_style_text_font(lbl_anim_models, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim_models, COL_ACCENT, 0);
    lv_obj_align(lbl_anim_models, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_obj_add_flag(models_container, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_models_widgets(const UsageData* data) {
    s_model_count = data->model_count;
    char buf[48];
    bool big_mode = (data->model_count > 0 && data->model_count <= 2);

    for (int i = 0; i < 2; i++) {
        if (big_mode && i < data->model_count) {
            const ModelBucket* mb = &data->models[i];
            lv_label_set_text(mdl_big_pill[i], mb->name);
            int p = (int)(mb->pct + 0.5f);
            lv_label_set_text_fmt(mdl_big_pct[i], "%d%%", p);
            lv_obj_set_style_text_color(mdl_big_pct[i], pct_color(mb->pct), 0);
            lv_bar_set_value(mdl_big_bar[i], p, LV_ANIM_ON);
            lv_obj_set_style_bg_color(mdl_big_bar[i], pct_color(mb->pct), LV_PART_INDICATOR);
            lv_arc_set_value(mdl_big_arc[i], p);
            lv_obj_set_style_arc_color(mdl_big_arc[i], pct_color(mb->pct), LV_PART_INDICATOR);
            format_reset_time(mb->reset_mins, buf, sizeof(buf));
            lv_label_set_text(mdl_big_reset[i], buf);
            lv_obj_clear_flag(mdl_big_panel[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(mdl_big_panel[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < MODEL_BUCKETS_MAX; i++) {
        if (!big_mode && i < data->model_count) {
            const ModelBucket* mb = &data->models[i];
            lv_label_set_text(mdl_pill[i], mb->name);
            int p = (int)(mb->pct + 0.5f);
            lv_label_set_text_fmt(mdl_pct[i], "%d%%", p);
            lv_obj_set_style_text_color(mdl_pct[i], pct_color(mb->pct), 0);
            lv_bar_set_value(mdl_bar[i], p, LV_ANIM_ON);
            lv_obj_set_style_bg_color(mdl_bar[i], pct_color(mb->pct), LV_PART_INDICATOR);
            format_reset_time(mb->reset_mins, buf, sizeof(buf));
            lv_label_set_text(mdl_reset[i], buf);
            lv_obj_clear_flag(mdl_panel[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(mdl_panel[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
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

    // Root container everything lives in. Periodically nudged a few px
    // (burn-in prevention) — invisible against the true-black screen bg.
    ui_root = lv_obj_create(scr);
    lv_obj_set_size(ui_root, L.scr_w, L.scr_h);
    lv_obj_set_pos(ui_root, 0, 0);
    lv_obj_set_style_bg_opa(ui_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_root, 0, 0);
    lv_obj_set_style_pad_all(ui_root, 0, 0);
    lv_obj_clear_flag(ui_root, LV_OBJ_FLAG_SCROLLABLE);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    init_usage_screen(ui_root);

    static const char* const hist_x[5] = { "-8h", "-6h", "-4h", "-2h", "now" };
    static const char* const hist_caps[3] = { "peak", "avg", "resets" };
    build_trend_page(&pg_history, ui_root, "History", HISTORY_CAP, hist_x, hist_caps,
                     usage_history_at, usage_history_count);

    static const char* const week_x[5] = { "-7d", "-5d", "-4d", "-2d", "now" };
    static const char* const week_caps[3] = { "peak", "avg", "now" };
    build_trend_page(&pg_weekly, ui_root, "Weekly", WEEKLY_CAP, week_x, week_caps,
                     weekly_history_at, weekly_history_count);

    init_models_screen(ui_root);
    weekly_history_init();
    splash_init(ui_root);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(ui_root);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    // Persistent clock, top right — the battery indicator's old spot.
    lbl_clock = lv_label_create(ui_root);
    lv_label_set_text(lbl_clock, "");
    lv_obj_set_style_text_font(lbl_clock, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_clock, COL_TEXT, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -L.margin, L.title_y);

    // Overlay last so it stacks above the splash canvas.
    init_limited_overlay(ui_root);
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

    // ---- Clock base ----
    if (data->local_mins >= 0) clock_base_mins = data->local_mins;

    // ---- History + Weekly buffers, trend pages, models ----
    usage_history_sample(data->session_pct);
    weekly_history_sample(data->weekly_pct);
    refresh_trend_chart(&pg_history);
    lv_label_set_text_fmt(pg_history.stat_val[0], "%d%%", usage_history_peak());
    lv_label_set_text_fmt(pg_history.stat_val[1], "%d%%", usage_history_avg());
    lv_label_set_text_fmt(pg_history.stat_val[2], "%d", usage_history_resets());
    lv_obj_set_style_text_color(pg_history.stat_val[0],
                                pct_color((float)usage_history_peak()), 0);
    refresh_trend_chart(&pg_weekly);
    lv_label_set_text_fmt(pg_weekly.stat_val[0], "%d%%", weekly_history_peak());
    lv_label_set_text_fmt(pg_weekly.stat_val[1], "%d%%", weekly_history_avg());
    lv_label_set_text_fmt(pg_weekly.stat_val[2], "%d%%", (int)(data->weekly_pct + 0.5f));
    lv_obj_set_style_text_color(pg_weekly.stat_val[2], pct_color(data->weekly_pct), 0);
    refresh_models_widgets(data);

    // ---- Time-to-limit projection (EMA-smoothed burn rate) ----
    proj_bar_str[0] = 0;
    proj_short_str[0] = 0;
    proj_mins_to = -1;
    float slope = usage_rate_slope();
    if (!s_limited && slope > PROJ_MIN_SLOPE && data->session_pct < 99.5f) {
        int mins_to = (int)((100.0f - data->session_pct) / slope + 0.5f);
        if (mins_to < 24 * 60) {
            proj_mins_to = mins_to;
            if (clock_base_mins >= 0) {
                char t[8];
                format_clock_time((clock_base_mins + mins_to) % 1440, t, sizeof(t));
                snprintf(proj_bar_str, sizeof(proj_bar_str), "Limit ~%s", t);
                snprintf(proj_short_str, sizeof(proj_short_str), "~%s", t);
            } else {
                snprintf(proj_bar_str, sizeof(proj_bar_str), "Limit in %dh %dm",
                         mins_to / 60, mins_to % 60);
                snprintf(proj_short_str, sizeof(proj_short_str), "%dh%dm",
                         mins_to / 60, mins_to % 60);
            }
        }
    }
    // Donut hole: projection when burning, window label when idle.
    if (lbl_session_window) {
        lv_label_set_text(lbl_session_window,
                          proj_short_str[0] ? proj_short_str : "5h");
        lv_obj_set_style_text_color(lbl_session_window,
            proj_short_str[0]
                ? (proj_mins_to < 60 ? COL_RED : COL_ACCENT) : COL_DIM, 0);
    }

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
    strlcpy(session_reset_str, buf, sizeof(session_reset_str));
    if (!bar_show_proj) lv_label_set_text(lbl_session_reset, buf);

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

    // ---- Burn-in pixel shift: orbit the whole UI a few px ----
    if (ui_root) {
        static const int8_t sdx[5] = { 0,  BURNIN_SHIFT_PX, 0, -BURNIN_SHIFT_PX, 0 };
        static const int8_t sdy[5] = { 0, 0,  BURNIN_SHIFT_PX, 0, -BURNIN_SHIFT_PX };
        static uint8_t  shift_idx = 0;
        static uint32_t shift_last_ms = 0;
        if (now - shift_last_ms >= BURNIN_SHIFT_INTERVAL_MS) {
            shift_last_ms = now;
            shift_idx = (shift_idx + 1) % 5;
            lv_obj_set_pos(ui_root, sdx[shift_idx], sdy[shift_idx]);
        }
    }

    // ---- Clock tick (once a second) + night dim window ----
    if (lbl_clock && clock_base_mins >= 0) {
        static uint32_t last_clk_ms = 0;
        if (now - last_clk_ms >= 1000) {
            last_clk_ms = now;
            int cur = (clock_base_mins + (int)((now - data_rx_ms) / 60000)) % 1440;
            char cbuf[8];
            format_clock_time(cur, cbuf, sizeof(cbuf));
            lv_label_set_text(lbl_clock, cbuf);

            bool night = (NIGHT_DIM_START_MIN <= NIGHT_DIM_END_MIN)
                ? (cur >= NIGHT_DIM_START_MIN && cur < NIGHT_DIM_END_MIN)
                : (cur >= NIGHT_DIM_START_MIN || cur < NIGHT_DIM_END_MIN);
            idle_set_night_cap(night ? NIGHT_BRIGHTNESS_CAP : 255);
        }
    }

    // ---- Session bar label: alternate reset time <-> limit projection ----
    if (current_screen == SCREEN_USAGE && now - bar_flip_ms >= BAR_FLIP_MS) {
        bar_flip_ms = now;
        bar_show_proj = proj_bar_str[0] ? !bar_show_proj : false;
        if (bar_show_proj) {
            lv_label_set_text(lbl_session_reset, proj_bar_str);
            lv_obj_set_style_text_color(lbl_session_reset,
                proj_mins_to >= 0 && proj_mins_to < 60 ? COL_RED : COL_AMBER, 0);
        } else {
            lv_label_set_text(lbl_session_reset, session_reset_str);
            lv_obj_set_style_text_color(lbl_session_reset, COL_TEXT, 0);
        }
    }

    // ---- Status line: pick the label on the visible screen ----
    lv_obj_t* target;
    switch (current_screen) {
    case SCREEN_USAGE:   target = lbl_anim;             break;
    case SCREEN_HISTORY: target = pg_history.anim_lbl;  break;
    case SCREEN_WEEKLY:  target = pg_weekly.anim_lbl;   break;
    case SCREEN_MODELS:  target = lbl_anim_models;      break;
    case SCREEN_LIMITED: target = lbl_anim_lim;         break;
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

// Usage -> History -> Weekly -> Models -> Activity (splash) -> Usage.
// Models drops out of the rotation when the daemon reports no scoped buckets.
static screen_t next_cycle_screen(screen_t s) {
    switch (s) {
    case SCREEN_USAGE:   return SCREEN_HISTORY;
    case SCREEN_HISTORY: return SCREEN_WEEKLY;
    case SCREEN_WEEKLY:  return (s_model_count > 0) ? SCREEN_MODELS : SCREEN_SPLASH;
    case SCREEN_MODELS:  return SCREEN_SPLASH;
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
    if (pg_history.container) lv_obj_add_flag(pg_history.container, LV_OBJ_FLAG_HIDDEN);
    if (pg_weekly.container)  lv_obj_add_flag(pg_weekly.container, LV_OBJ_FLAG_HIDDEN);
    if (models_container)     lv_obj_add_flag(models_container, LV_OBJ_FLAG_HIDDEN);
    if (limited_overlay)      lv_obj_add_flag(limited_overlay, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_HISTORY: lv_obj_clear_flag(pg_history.container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_WEEKLY:  lv_obj_clear_flag(pg_weekly.container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_MODELS:  lv_obj_clear_flag(models_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_LIMITED:
        // Sleeping creature on the splash canvas + info overlay on top.
        splash_force_anim("expression sleep");
        splash_show();
        lv_obj_clear_flag(limited_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(limited_overlay);
        break;
    default: break;
    }

    bool art_screen = (screen == SCREEN_SPLASH);
    if (logo_img) {
        if (art_screen || screen == SCREEN_LIMITED)
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }
    // Clock stays up everywhere except over the pixel art.
    if (lbl_clock) {
        if (art_screen) lv_obj_add_flag(lbl_clock, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_clear_flag(lbl_clock, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_clock && !art_screen) lv_obj_move_foreground(lbl_clock);

    if (screen == SCREEN_USAGE || screen == SCREEN_HISTORY ||
        screen == SCREEN_WEEKLY || screen == SCREEN_MODELS)
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


