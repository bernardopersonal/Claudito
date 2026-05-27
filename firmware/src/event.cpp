#include "event.h"
#include "ble.h"
#include "sound.h"
#include "idle.h"
#include "ui.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <string.h>

// Fonts
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);

// ---- Permission dialog state ----
static bool perm_active = false;
static lv_obj_t* perm_overlay = nullptr;
static lv_obj_t* perm_panel = nullptr;
static lv_obj_t* perm_lbl_title = nullptr;
static lv_obj_t* perm_lbl_tool = nullptr;
static lv_obj_t* perm_lbl_cmd = nullptr;
static lv_obj_t* perm_btn_allow = nullptr;
static lv_obj_t* perm_btn_always = nullptr;
static lv_obj_t* perm_btn_deny = nullptr;

// ---- Timeout ----
static uint32_t perm_show_ms = 0;
#define PERM_TIMEOUT_MS 30000   // auto-deny after 30s

// USB HID scan codes
#define HID_KEY_ENTER   0x28
#define HID_KEY_ESCAPE  0x29
#define HID_KEY_TAB     0x2B

static void dismiss_perm_dialog(const char* decision) {
    perm_active = false;
    if (perm_overlay) {
        lv_obj_delete(perm_overlay);
        perm_overlay = nullptr;
    }
    if (strcmp(decision, "deny") != 0) {
        ui_set_activity(ACTIVITY_WORKING);
    } else {
        ui_set_activity(ACTIVITY_IDLE);
    }
    Serial.printf("Event: permission → %s\n", decision);
}

// Send a HID keypress and release after a short delay.
static void hid_tap(uint8_t key, uint8_t modifier = 0) {
    ble_keyboard_press(key, modifier);
    delay(30);
    ble_keyboard_release();
    delay(30);
}

static void btn_allow_cb(lv_event_t* e) {
    (void)e;
    sound_play(SND_STOP);
    // Claude Code permission prompt: Enter accepts the focused option (Allow)
    hid_tap(HID_KEY_ENTER);
    dismiss_perm_dialog("allow_once");
}

static void btn_always_cb(lv_event_t* e) {
    (void)e;
    sound_play(SND_STOP);
    // Tab to "Always" option, then Enter to select
    hid_tap(HID_KEY_TAB);
    hid_tap(HID_KEY_ENTER);
    dismiss_perm_dialog("allow_always");
}

static void btn_deny_cb(lv_event_t* e) {
    (void)e;
    sound_play(SND_STOP_FAILURE);
    // Escape dismisses / denies the permission
    hid_tap(HID_KEY_ESCAPE);
    dismiss_perm_dialog("deny");
}

static void show_permission_dialog(const char* tool, const char* cmd) {
    if (perm_active && perm_overlay) {
        // Already showing one — dismiss old first
        lv_obj_delete(perm_overlay);
        perm_overlay = nullptr;
    }

    const int W = board_caps().width;
    const int H = board_caps().height;
    idle_note_activity();

    // Semi-transparent overlay
    perm_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(perm_overlay);
    lv_obj_set_size(perm_overlay, W, H);
    lv_obj_set_style_bg_color(perm_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(perm_overlay, LV_OPA_70, 0);
    lv_obj_align(perm_overlay, LV_ALIGN_CENTER, 0, 0);

    // Dialog panel
    int pw = W - 40;
    int ph = 300;
    perm_panel = lv_obj_create(perm_overlay);
    lv_obj_remove_style_all(perm_panel);
    lv_obj_set_size(perm_panel, pw, ph);
    lv_obj_align(perm_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(perm_panel, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(perm_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(perm_panel, 16, 0);
    lv_obj_set_style_pad_all(perm_panel, 20, 0);
    lv_obj_set_flex_flow(perm_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(perm_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(perm_panel, 8, 0);

    // Title
    perm_lbl_title = lv_label_create(perm_panel);
    lv_label_set_text(perm_lbl_title, "Permission Request");
    lv_obj_set_style_text_font(perm_lbl_title, &font_styrene_24, 0);
    lv_obj_set_style_text_color(perm_lbl_title, THEME_ACCENT, 0);

    // Tool name
    perm_lbl_tool = lv_label_create(perm_panel);
    char tool_txt[64];
    snprintf(tool_txt, sizeof(tool_txt), "Tool: %s", tool);
    lv_label_set_text(perm_lbl_tool, tool_txt);
    lv_obj_set_style_text_font(perm_lbl_tool, &font_styrene_20, 0);
    lv_obj_set_style_text_color(perm_lbl_tool, THEME_TEXT, 0);

    // Command preview (truncated)
    perm_lbl_cmd = lv_label_create(perm_panel);
    lv_label_set_long_mode(perm_lbl_cmd, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(perm_lbl_cmd, pw - 40);
    char cmd_txt[160];
    if (cmd && strlen(cmd) > 0) {
        // Truncate command display to ~120 chars
        if (strlen(cmd) > 120) {
            snprintf(cmd_txt, sizeof(cmd_txt), "%.117s...", cmd);
        } else {
            snprintf(cmd_txt, sizeof(cmd_txt), "%s", cmd);
        }
    } else {
        snprintf(cmd_txt, sizeof(cmd_txt), "(no command details)");
    }
    lv_label_set_text(perm_lbl_cmd, cmd_txt);
    lv_obj_set_style_text_font(perm_lbl_cmd, &font_styrene_14, 0);
    lv_obj_set_style_text_color(perm_lbl_cmd, THEME_DIM, 0);

    // Button row container
    lv_obj_t* btn_row = lv_obj_create(perm_panel);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, pw - 40, 50);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, 0);

    // Style for buttons
    auto make_btn = [&](const char* text, lv_color_t bg, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* btn = lv_btn_create(btn_row);
        lv_obj_set_size(btn, (pw - 80) / 3, 44);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &font_styrene_16, 0);
        lv_obj_set_style_text_color(lbl, THEME_TEXT, 0);
        lv_obj_center(lbl);
        return btn;
    };

    perm_btn_allow  = make_btn("Once",   THEME_GREEN, btn_allow_cb);
    perm_btn_always = make_btn("Always", THEME_ACCENT, btn_always_cb);
    perm_btn_deny   = make_btn("Deny",   THEME_RED,   btn_deny_cb);

    perm_active = true;
    perm_show_ms = millis();
    Serial.printf("Event: permission dialog shown (tool=%s)\n", tool);
}

// ---- Event dispatch ----

static void dispatch_event(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("Event: JSON parse error");
        return;
    }

    const char* ev = doc["ev"] | "";

    if (strcmp(ev, "stop") == 0) {
        sound_play(SND_STOP);
        ui_set_activity(ACTIVITY_IDLE);
    } else if (strcmp(ev, "fail") == 0) {
        sound_play(SND_STOP_FAILURE);
        ui_set_activity(ACTIVITY_IDLE);
    } else if (strcmp(ev, "perm") == 0) {
        sound_play(SND_PERMISSION_REQUEST);
        ui_set_activity(ACTIVITY_WAITING);
        const char* tool = doc["tool"] | "Unknown";
        const char* cmd  = doc["cmd"]  | "";
        show_permission_dialog(tool, cmd);
    } else if (strcmp(ev, "pprompt") == 0) {
        sound_play(SND_PERMISSION_PROMPT);
        ui_set_activity(ACTIVITY_WAITING);
    } else if (strcmp(ev, "idle") == 0) {
        sound_play(SND_IDLE_PROMPT);
        ui_set_activity(ACTIVITY_IDLE);
    } else if (strcmp(ev, "done") == 0) {
        sound_play(SND_TASK_COMPLETED);
        ui_set_activity(ACTIVITY_IDLE);
    } else if (strcmp(ev, "working") == 0) {
        ui_set_activity(ACTIVITY_WORKING);
    } else {
        Serial.printf("Event: unknown event '%s'\n", ev);
    }
}

// ---- Public API ----

void event_init(void) {
    Serial.println("Event: init OK");
}

void event_tick(void) {
    // Check for new BLE events
    if (ble_has_event()) {
        const char* json = ble_get_event();
        dispatch_event(json);
    }

    // Permission dialog timeout — auto-dismiss (no HID key, user handles on Mac)
    if (perm_active && (millis() - perm_show_ms > PERM_TIMEOUT_MS)) {
        Serial.println("Event: permission timeout → dismiss");
        sound_play(SND_STOP_FAILURE);
        dismiss_perm_dialog("timeout");
    }
}

bool event_permission_active(void) {
    return perm_active;
}
