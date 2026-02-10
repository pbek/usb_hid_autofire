#include <stdio.h>
#include <string.h>
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <usb_hid_autofire_icons.h>
#include "version.h"

#define TAG "usb_hid_autofire"

// Uncomment to be able to make a screenshot
//#define USB_HID_AUTOFIRE_SCREENSHOT

#define AUTOFIRE_DELAY_MIN_MS 5U
#define AUTOFIRE_DELAY_MAX_MS 10000U
#define AUTOFIRE_DELAY_STEP_MS 10U
#define AUTOFIRE_DELAY_ACCEL_MEDIUM_MS 50U
#define AUTOFIRE_DELAY_ACCEL_FAST_MS 100U
#define AUTOFIRE_REPEAT_MEDIUM_THRESHOLD 3U
#define AUTOFIRE_REPEAT_FAST_THRESHOLD 8U
#define AUTOFIRE_DELAY_DEFAULT_MS 10U
#define AUTOFIRE_PRESET_SLOW_MS 250U
#define AUTOFIRE_PRESET_MEDIUM_MS 120U
#define AUTOFIRE_PRESET_FAST_MS 70U
#define HIGH_CPS_CONFIRM_THRESHOLD_X10 120U
#define UI_REFRESH_PERIOD_MS 250U
#define EVENT_DRAIN_MAX_COUNT 32U
#define SETTINGS_SAVE_DEBOUNCE_MS 500U
#define USB_HID_AUTOFIRE_SETTINGS_PATH APP_DATA_PATH(".settings")
#define USB_HID_AUTOFIRE_SETTINGS_FILE_TYPE "USB HID Autofire Settings"
#define USB_HID_AUTOFIRE_SETTINGS_VERSION 1U

typedef enum {
    EventTypeInput,
    EventTypeTick,
    EventTypeUiRefresh,
    EventTypeSettingsSave,
} EventType;

typedef enum {
    ClickPhasePress,
    ClickPhaseRelease,
} ClickPhase;

typedef enum {
    AutofireModeMouseLeftClick,
    AutofireModeMouseRightClick,
    AutofireModeKeyboardEnter,
    AutofireModeKeyboardSpace,
} AutofireMode;

typedef enum {
    AutofirePresetCustom,
    AutofirePresetSlow,
    AutofirePresetMedium,
    AutofirePresetFast,
    AutofirePresetCount,
} AutofirePreset;

typedef enum {
    AutofireStartupPolicyPausedOnLaunch,
    AutofireStartupPolicyRestoreLastState,
} AutofireStartupPolicy;

typedef struct {
    union {
        InputEvent input;
    };
    EventType type;
} UsbMouseEvent;

typedef struct {
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    Gui* gui;
    DialogsApp* dialogs;
    FuriTimer* click_timer;
    FuriTimer* ui_refresh_timer;
    FuriTimer* settings_save_timer;
    FuriHalUsbInterface* usb_mode_prev;
    bool active;
    bool mouse_pressed;
    bool ui_dirty;
    bool settings_dirty;
    bool show_help;
    bool last_active_state;
    uint32_t autofire_delay_ms;
    uint32_t realtime_cps_x10;
    uint32_t last_click_release_tick_ms;
    uint32_t last_click_interval_ms;
    bool adjust_hold_active;
    InputKey adjust_hold_key;
    uint16_t adjust_repeat_count;
    bool mode_hold_active;
    InputKey mode_hold_key;
    bool ok_long_handled;
    bool back_long_handled;
    AutofireMode mode;
    AutofirePreset preset;
    AutofireStartupPolicy startup_policy;
    ClickPhase click_phase;
} UsbHidAutofireApp;

static uint32_t usb_hid_autofire_delay_clamp(uint32_t delay_ms) {
    if(delay_ms < AUTOFIRE_DELAY_MIN_MS) {
        return AUTOFIRE_DELAY_MIN_MS;
    }

    if(delay_ms > AUTOFIRE_DELAY_MAX_MS) {
        return AUTOFIRE_DELAY_MAX_MS;
    }

    return delay_ms;
}

static uint32_t usb_hid_autofire_delay_decrease(uint32_t delay_ms, uint32_t step_ms) {
    if(step_ms == 0U) {
        return usb_hid_autofire_delay_clamp(delay_ms);
    }

    delay_ms = usb_hid_autofire_delay_clamp(delay_ms);
    if(delay_ms <= AUTOFIRE_DELAY_MIN_MS) {
        return AUTOFIRE_DELAY_MIN_MS;
    }

    if((delay_ms - AUTOFIRE_DELAY_MIN_MS) < step_ms) {
        return AUTOFIRE_DELAY_MIN_MS;
    }

    return delay_ms - step_ms;
}

static uint32_t usb_hid_autofire_delay_increase(uint32_t delay_ms, uint32_t step_ms) {
    if(step_ms == 0U) {
        return usb_hid_autofire_delay_clamp(delay_ms);
    }

    delay_ms = usb_hid_autofire_delay_clamp(delay_ms);
    if(delay_ms >= AUTOFIRE_DELAY_MAX_MS) {
        return AUTOFIRE_DELAY_MAX_MS;
    }

    if((AUTOFIRE_DELAY_MAX_MS - delay_ms) < step_ms) {
        return AUTOFIRE_DELAY_MAX_MS;
    }

    return delay_ms + step_ms;
}

static uint32_t usb_hid_autofire_accel_step_ms(uint16_t repeat_count) {
    if(repeat_count >= AUTOFIRE_REPEAT_FAST_THRESHOLD) {
        return AUTOFIRE_DELAY_ACCEL_FAST_MS;
    }
    if(repeat_count >= AUTOFIRE_REPEAT_MEDIUM_THRESHOLD) {
        return AUTOFIRE_DELAY_ACCEL_MEDIUM_MS;
    }
    return AUTOFIRE_DELAY_STEP_MS;
}

static uint32_t usb_hid_autofire_config_cps_x10_for_delay(uint32_t delay_ms) {
    uint32_t half_delay_ms = delay_ms / 2U;
    if(half_delay_ms == 0U) {
        half_delay_ms = 1U;
    }

    uint32_t cycle_ms = half_delay_ms * 2U;
    return (10000U + (cycle_ms / 2U)) / cycle_ms;
}

static const char* usb_hid_autofire_mode_label(AutofireMode mode) {
    switch(mode) {
        case AutofireModeMouseLeftClick:
            return "Mouse Left";
        case AutofireModeMouseRightClick:
            return "Mouse Right";
        case AutofireModeKeyboardEnter:
            return "Key Enter";
        case AutofireModeKeyboardSpace:
            return "Key Space";
        default:
            return "Unknown";
    }
}

static AutofireMode usb_hid_autofire_next_mode(AutofireMode mode) {
    switch(mode) {
        case AutofireModeMouseLeftClick:
            return AutofireModeMouseRightClick;
        case AutofireModeMouseRightClick:
            return AutofireModeKeyboardEnter;
        case AutofireModeKeyboardEnter:
            return AutofireModeKeyboardSpace;
        case AutofireModeKeyboardSpace:
        default:
            return AutofireModeMouseLeftClick;
    }
}

static AutofireMode usb_hid_autofire_prev_mode(AutofireMode mode) {
    switch(mode) {
        case AutofireModeMouseLeftClick:
            return AutofireModeKeyboardSpace;
        case AutofireModeMouseRightClick:
            return AutofireModeMouseLeftClick;
        case AutofireModeKeyboardEnter:
            return AutofireModeMouseRightClick;
        case AutofireModeKeyboardSpace:
            return AutofireModeKeyboardEnter;
        default:
            return AutofireModeMouseLeftClick;
    }
}

static const char* usb_hid_autofire_preset_label(AutofirePreset preset) {
    switch(preset) {
        case AutofirePresetCustom:
            return "Custom";
        case AutofirePresetSlow:
            return "Slow";
        case AutofirePresetMedium:
            return "Medium";
        case AutofirePresetFast:
            return "Fast";
        default:
            return "Unknown";
    }
}

static uint32_t usb_hid_autofire_preset_delay_ms(AutofirePreset preset) {
    switch(preset) {
        case AutofirePresetSlow:
            return AUTOFIRE_PRESET_SLOW_MS;
        case AutofirePresetMedium:
            return AUTOFIRE_PRESET_MEDIUM_MS;
        case AutofirePresetFast:
            return AUTOFIRE_PRESET_FAST_MS;
        case AutofirePresetCustom:
        default:
            return AUTOFIRE_DELAY_DEFAULT_MS;
    }
}

static AutofirePreset usb_hid_autofire_next_preset(AutofirePreset preset) {
    switch(preset) {
        case AutofirePresetCustom:
            return AutofirePresetSlow;
        case AutofirePresetSlow:
            return AutofirePresetMedium;
        case AutofirePresetMedium:
            return AutofirePresetFast;
        case AutofirePresetFast:
        default:
            return AutofirePresetSlow;
    }
}

static bool usb_hid_autofire_mode_is_valid(uint32_t mode_value) {
    return mode_value <= (uint32_t)AutofireModeKeyboardSpace;
}

static bool usb_hid_autofire_preset_is_valid(uint32_t preset_value) {
    return preset_value < (uint32_t)AutofirePresetCount;
}

static bool usb_hid_autofire_startup_policy_is_valid(uint32_t startup_policy_value) {
    return startup_policy_value <= (uint32_t)AutofireStartupPolicyRestoreLastState;
}

static void usb_hid_autofire_format_cps(char* out, size_t out_size, uint32_t cps_x10) {
    snprintf(out, out_size, "%lu.%lu", (unsigned long)(cps_x10 / 10U), (unsigned long)(cps_x10 % 10U));
}

static uint32_t usb_hid_autofire_effective_cycle_ms(const UsbHidAutofireApp* app) {
    uint32_t half_delay_ms = app->autofire_delay_ms / 2U;
    if(half_delay_ms == 0U) {
        half_delay_ms = 1U;
    }
    return half_delay_ms * 2U;
}

static void usb_hid_autofire_reset_cps_tracking(UsbHidAutofireApp* app) {
    app->realtime_cps_x10 = 0U;
    app->last_click_release_tick_ms = 0U;
    app->last_click_interval_ms = usb_hid_autofire_effective_cycle_ms(app);
}

static void usb_hid_autofire_record_click_release(UsbHidAutofireApp* app) {
    uint32_t now_ms = furi_get_tick();
    if(app->last_click_release_tick_ms != 0U) {
        uint32_t interval_ms = now_ms - app->last_click_release_tick_ms;
        if(interval_ms == 0U) {
            interval_ms = 1U;
        }
        app->last_click_interval_ms = interval_ms;
    }
    app->last_click_release_tick_ms = now_ms;
}

static uint32_t usb_hid_autofire_realtime_cps_x10(const UsbHidAutofireApp* app) {
    if(!app->active || (app->last_click_release_tick_ms == 0U) || (app->last_click_interval_ms == 0U)) {
        return 0U;
    }

    uint32_t elapsed_ms = furi_get_tick() - app->last_click_release_tick_ms;
    uint32_t effective_interval_ms = app->last_click_interval_ms;
    if(elapsed_ms > effective_interval_ms) {
        effective_interval_ms = elapsed_ms;
    }
    if(effective_interval_ms == 0U) {
        effective_interval_ms = 1U;
    }

    return (10000U + (effective_interval_ms / 2U)) / effective_interval_ms;
}

static void usb_hid_autofire_mark_settings_dirty(UsbHidAutofireApp* app) {
    app->settings_dirty = true;
    if(app->settings_save_timer) {
        furi_timer_start(app->settings_save_timer, furi_ms_to_ticks(SETTINGS_SAVE_DEBOUNCE_MS));
    }
}

static bool usb_hid_autofire_settings_save(const UsbHidAutofireApp* app) {
    bool success = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* settings_file = flipper_format_file_alloc(storage);
    if(settings_file && flipper_format_file_open_always(settings_file, USB_HID_AUTOFIRE_SETTINGS_PATH)) {
        do {
            if(!flipper_format_write_header_cstr(
                   settings_file,
                   USB_HID_AUTOFIRE_SETTINGS_FILE_TYPE,
                   USB_HID_AUTOFIRE_SETTINGS_VERSION)) {
                break;
            }

            uint32_t delay_ms = usb_hid_autofire_delay_clamp(app->autofire_delay_ms);
            uint32_t mode = app->mode;
            uint32_t preset = app->preset;
            uint32_t startup_policy = app->startup_policy;
            bool last_active = app->last_active_state;

            if(!flipper_format_write_uint32(settings_file, "delay_ms", &delay_ms, 1)) break;
            if(!flipper_format_write_uint32(settings_file, "mode", &mode, 1)) break;
            if(!flipper_format_write_uint32(settings_file, "preset", &preset, 1)) break;
            if(!flipper_format_write_uint32(settings_file, "startup_policy", &startup_policy, 1)) break;
            if(!flipper_format_write_bool(settings_file, "last_active", &last_active, 1)) break;

            success = true;
        } while(false);
    }

    if(settings_file) {
        flipper_format_free(settings_file);
    }
    furi_record_close(RECORD_STORAGE);

    if(!success) {
        FURI_LOG_W(TAG, "Failed to save settings");
    }

    return success;
}

static bool usb_hid_autofire_settings_load(UsbHidAutofireApp* app) {
    bool loaded = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* settings_file = flipper_format_file_alloc(storage);
    FuriString* file_type = furi_string_alloc();
    uint32_t version = 0;
    uint32_t delay_ms = AUTOFIRE_DELAY_DEFAULT_MS;
    uint32_t mode = AutofireModeMouseLeftClick;
    uint32_t preset = AutofirePresetCustom;
    uint32_t startup_policy = AutofireStartupPolicyPausedOnLaunch;
    bool last_active = false;

    if(settings_file && flipper_format_file_open_existing(settings_file, USB_HID_AUTOFIRE_SETTINGS_PATH)) {
        do {
            if(!flipper_format_read_header(settings_file, file_type, &version)) break;
            if((strcmp(furi_string_get_cstr(file_type), USB_HID_AUTOFIRE_SETTINGS_FILE_TYPE) != 0) ||
               (version != USB_HID_AUTOFIRE_SETTINGS_VERSION))
                break;
            if(!flipper_format_read_uint32(settings_file, "delay_ms", &delay_ms, 1)) break;
            if(!flipper_format_read_uint32(settings_file, "mode", &mode, 1)) break;
            if(!flipper_format_read_uint32(settings_file, "preset", &preset, 1)) break;
            if(!flipper_format_read_uint32(settings_file, "startup_policy", &startup_policy, 1)) break;
            if(!flipper_format_read_bool(settings_file, "last_active", &last_active, 1)) break;
            if(!usb_hid_autofire_mode_is_valid(mode)) break;
            if(!usb_hid_autofire_preset_is_valid(preset)) break;
            if(!usb_hid_autofire_startup_policy_is_valid(startup_policy)) break;

            loaded = true;
        } while(false);
    }

    furi_string_free(file_type);
    if(settings_file) {
        flipper_format_free(settings_file);
    }
    furi_record_close(RECORD_STORAGE);

    if(!loaded) {
        return false;
    }

    app->autofire_delay_ms = usb_hid_autofire_delay_clamp(delay_ms);
    app->mode = (AutofireMode)mode;
    app->preset = (AutofirePreset)preset;
    app->startup_policy = AutofireStartupPolicyPausedOnLaunch;
    app->last_active_state = last_active;

    if((app->preset != AutofirePresetCustom) &&
       (app->autofire_delay_ms != usb_hid_autofire_preset_delay_ms(app->preset))) {
        app->preset = AutofirePresetCustom;
    }

    return true;
}

static void usb_hid_autofire_settings_flush_if_dirty(UsbHidAutofireApp* app) {
    if(!app->settings_dirty) {
        return;
    }

    if(usb_hid_autofire_settings_save(app)) {
        app->settings_dirty = false;
    }
}

static void usb_hid_autofire_render_callback(Canvas* canvas, void* ctx) {
    UsbHidAutofireApp* app = ctx;
    char status_str[24];
    char mode_str[24];
    char preset_str[24];
    char delay_rate_str[40];
    char cps_str[24];

    canvas_clear(canvas);

    if(app->show_help) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 0, 10, "Autofire Help");

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_icon(canvas, 0, 14, &I_Ok_btn_9x9);
        canvas_draw_str(canvas, 12, 22, "start/pause");

        canvas_draw_icon(canvas, 0, 24, &I_Ok_btn_9x9);
        canvas_draw_str(canvas, 12, 32, "long: cycle preset");

        canvas_draw_icon(canvas, 0, 36, &I_ButtonUp_7x4);
        canvas_draw_icon(canvas, 9, 36, &I_ButtonDown_7x4);
        canvas_draw_str(canvas, 20, 42, "tap+hold: mode");

        canvas_draw_icon(canvas, 0, 45, &I_ButtonLeft_4x7);
        canvas_draw_icon(canvas, 7, 45, &I_ButtonRight_4x7);
        canvas_draw_str(canvas, 20, 52, "tap+hold: delay");

        canvas_draw_icon(canvas, 0, 55, &I_Pin_back_arrow_10x8);
        canvas_draw_str(canvas, 13, 63, "close");
        return;
    }

    snprintf(status_str, sizeof(status_str), "Status: %s", app->active ? "ACTIVE" : "PAUSED");
    snprintf(mode_str, sizeof(mode_str), "Mode: %s", usb_hid_autofire_mode_label(app->mode));
    snprintf(preset_str, sizeof(preset_str), "Preset: %s", usb_hid_autofire_preset_label(app->preset));
    usb_hid_autofire_format_cps(cps_str, sizeof(cps_str), app->realtime_cps_x10);
    snprintf(
        delay_rate_str,
        sizeof(delay_rate_str),
        "Delay:%lums  Rate:%s",
        (unsigned long)app->autofire_delay_ms,
        cps_str);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "USB HID Autofire");
    canvas_draw_str(canvas, 90, 10, "v");
    canvas_draw_str(canvas, 96, 10, VERSION);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 22, status_str);
    canvas_draw_str(canvas, 0, 32, mode_str);
    canvas_draw_str(canvas, 0, 42, preset_str);
    canvas_draw_str(canvas, 0, 52, delay_rate_str);
    canvas_draw_icon(canvas, 0, 55, &I_Pin_back_arrow_10x8);
    canvas_draw_str(canvas, 13, 63, "hold:help");
    canvas_draw_icon(canvas, 72, 56, &I_ButtonUp_7x4);
    canvas_draw_icon(canvas, 81, 56, &I_ButtonDown_7x4);
    canvas_draw_str(canvas, 92, 63, "mode");
}

static void usb_hid_autofire_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;

    UsbMouseEvent event;
    event.type = EventTypeInput;
    event.input = *input_event;
    furi_message_queue_put(event_queue, &event, 0);
}

static void usb_hid_autofire_timer_callback(void* ctx) {
    UsbHidAutofireApp* app = ctx;
    UsbMouseEvent event = {.type = EventTypeTick};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void usb_hid_autofire_ui_timer_callback(void* ctx) {
    UsbHidAutofireApp* app = ctx;
    UsbMouseEvent event = {.type = EventTypeUiRefresh};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void usb_hid_autofire_settings_save_timer_callback(void* ctx) {
    UsbHidAutofireApp* app = ctx;
    UsbMouseEvent event = {.type = EventTypeSettingsSave};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static uint32_t usb_hid_autofire_half_delay_ticks(const UsbHidAutofireApp* app) {
    uint32_t half_delay_ms = app->autofire_delay_ms / 2;
    if(half_delay_ms == 0) {
        half_delay_ms = 1;
    }
    return furi_ms_to_ticks(half_delay_ms);
}

static void usb_hid_autofire_schedule_next_tick(UsbHidAutofireApp* app) {
    furi_timer_start(app->click_timer, usb_hid_autofire_half_delay_ticks(app));
}

static void usb_hid_autofire_drain_event_queue(UsbHidAutofireApp* app, uint8_t max_count) {
    UsbMouseEvent event;
    for(uint8_t i = 0; i < max_count; i++) {
        if(furi_message_queue_get(app->event_queue, &event, 0) != FuriStatusOk) {
            break;
        }
    }
}

static void usb_hid_autofire_tick(UsbHidAutofireApp* app);
static void usb_hid_autofire_stop(UsbHidAutofireApp* app);

static void usb_hid_autofire_press_mode_control(UsbHidAutofireApp* app) {
    switch(app->mode) {
        case AutofireModeMouseLeftClick:
            furi_hal_hid_mouse_press(HID_MOUSE_BTN_LEFT);
            break;
        case AutofireModeMouseRightClick:
            furi_hal_hid_mouse_press(HID_MOUSE_BTN_RIGHT);
            break;
        case AutofireModeKeyboardEnter:
            furi_hal_hid_kb_press(HID_KEYBOARD_RETURN);
            break;
        case AutofireModeKeyboardSpace:
            furi_hal_hid_kb_press(HID_KEYBOARD_SPACEBAR);
            break;
        default:
            break;
    }
}

static void usb_hid_autofire_release_mode_control(UsbHidAutofireApp* app) {
    switch(app->mode) {
        case AutofireModeMouseLeftClick:
            furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
            break;
        case AutofireModeMouseRightClick:
            furi_hal_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
            break;
        case AutofireModeKeyboardEnter:
            furi_hal_hid_kb_release(HID_KEYBOARD_RETURN);
            break;
        case AutofireModeKeyboardSpace:
            furi_hal_hid_kb_release(HID_KEYBOARD_SPACEBAR);
            break;
        default:
            break;
    }
}

static void usb_hid_autofire_start(UsbHidAutofireApp* app) {
    app->active = true;
    app->click_phase = ClickPhasePress;
    usb_hid_autofire_reset_cps_tracking(app);
    furi_timer_start(app->ui_refresh_timer, furi_ms_to_ticks(UI_REFRESH_PERIOD_MS));
    usb_hid_autofire_tick(app);
}

static bool usb_hid_autofire_set_mode(UsbHidAutofireApp* app, AutofireMode new_mode) {
    if(new_mode == app->mode) {
        return false;
    }

    if(app->mouse_pressed) {
        usb_hid_autofire_release_mode_control(app);
        app->mouse_pressed = false;
    }

    app->mode = new_mode;
    app->click_phase = ClickPhasePress;
    if(app->active) {
        furi_timer_stop(app->click_timer);
        usb_hid_autofire_schedule_next_tick(app);
    }
    usb_hid_autofire_mark_settings_dirty(app);
    app->ui_dirty = true;

    return true;
}

static bool usb_hid_autofire_set_delay(
    UsbHidAutofireApp* app,
    uint32_t new_delay_ms,
    AutofirePreset new_preset) {
    new_delay_ms = usb_hid_autofire_delay_clamp(new_delay_ms);
    if((new_delay_ms == app->autofire_delay_ms) && (new_preset == app->preset)) {
        return false;
    }

    bool delay_changed = (new_delay_ms != app->autofire_delay_ms);
    app->autofire_delay_ms = new_delay_ms;
    app->preset = new_preset;
    if(app->active && delay_changed) {
        furi_timer_stop(app->click_timer);
        usb_hid_autofire_schedule_next_tick(app);
    }
    usb_hid_autofire_mark_settings_dirty(app);
    app->ui_dirty = true;

    return true;
}

static void usb_hid_autofire_adjust_delay(UsbHidAutofireApp* app, InputKey key, uint32_t step_ms) {
    if(key == InputKeyLeft) {
        usb_hid_autofire_set_delay(
            app,
            usb_hid_autofire_delay_decrease(app->autofire_delay_ms, step_ms),
            AutofirePresetCustom);
    } else if(key == InputKeyRight) {
        usb_hid_autofire_set_delay(
            app,
            usb_hid_autofire_delay_increase(app->autofire_delay_ms, step_ms),
            AutofirePresetCustom);
    }
}

static bool usb_hid_autofire_confirm_high_cps_preset(
    UsbHidAutofireApp* app,
    AutofirePreset preset,
    uint32_t preset_cps_x10) {
    if(!app->dialogs) {
        return true;
    }

    bool confirmed = false;
    DialogMessage* message = dialog_message_alloc();
    if(!message) {
        return false;
    }

    char cps_str[24];
    char text[72];
    usb_hid_autofire_format_cps(cps_str, sizeof(cps_str), preset_cps_x10);
    snprintf(
        text,
        sizeof(text),
        "%s preset: %s CPS\nApply this rate?",
        usb_hid_autofire_preset_label(preset),
        cps_str);

    dialog_message_set_header(message, "High Click Rate", 64, 12, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 34, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "No", NULL, "Yes");

    // Prevent stale buffered inputs from leaking into modal/result handling.
    usb_hid_autofire_drain_event_queue(app, EVENT_DRAIN_MAX_COUNT);
    DialogMessageButton result = dialog_message_show(app->dialogs, message);
    usb_hid_autofire_drain_event_queue(app, EVENT_DRAIN_MAX_COUNT);
    confirmed = (result == DialogMessageButtonRight);
    dialog_message_free(message);

    return confirmed;
}

static void usb_hid_autofire_apply_preset_request(UsbHidAutofireApp* app, AutofirePreset preset) {
    uint32_t preset_delay_ms = usb_hid_autofire_preset_delay_ms(preset);
    uint32_t preset_cps_x10 = usb_hid_autofire_config_cps_x10_for_delay(preset_delay_ms);
    bool was_active = app->active;
    bool should_apply = true;

    if(was_active) {
        usb_hid_autofire_stop(app);
    }

    if(preset_cps_x10 >= HIGH_CPS_CONFIRM_THRESHOLD_X10) {
        should_apply = usb_hid_autofire_confirm_high_cps_preset(app, preset, preset_cps_x10);
    }

    if(should_apply) {
        usb_hid_autofire_set_delay(app, preset_delay_ms, preset);
    }

    if(was_active) {
        usb_hid_autofire_start(app);
        app->ui_dirty = true;
    }
}

static void usb_hid_autofire_handle_delay_input(UsbHidAutofireApp* app, const InputEvent* input) {
    if((input->key != InputKeyLeft) && (input->key != InputKeyRight)) {
        return;
    }

    switch(input->type) {
        case InputTypeShort:
            usb_hid_autofire_adjust_delay(app, input->key, AUTOFIRE_DELAY_STEP_MS);
            break;

        case InputTypeLong:
            app->adjust_hold_active = true;
            app->adjust_hold_key = input->key;
            app->adjust_repeat_count = 0U;
            usb_hid_autofire_adjust_delay(app, input->key, AUTOFIRE_DELAY_ACCEL_MEDIUM_MS);
            break;

        case InputTypeRepeat:
            if((!app->adjust_hold_active) || (app->adjust_hold_key != input->key)) {
                app->adjust_hold_active = true;
                app->adjust_hold_key = input->key;
                app->adjust_repeat_count = 0U;
            }
            if(app->adjust_repeat_count < 0xFFFFU) {
                app->adjust_repeat_count++;
            }
            usb_hid_autofire_adjust_delay(
                app, input->key, usb_hid_autofire_accel_step_ms(app->adjust_repeat_count));
            break;

        case InputTypeRelease:
            if(app->adjust_hold_active && (app->adjust_hold_key == input->key)) {
                app->adjust_hold_active = false;
                app->adjust_hold_key = InputKeyMAX;
                app->adjust_repeat_count = 0U;
            }
            break;

        default:
            break;
    }
}

static void usb_hid_autofire_handle_mode_input(UsbHidAutofireApp* app, const InputEvent* input) {
    if((input->key != InputKeyUp) && (input->key != InputKeyDown)) {
        return;
    }

    switch(input->type) {
        case InputTypeShort:
        case InputTypeLong:
        case InputTypeRepeat: {
            if((input->type == InputTypeLong) || (input->type == InputTypeRepeat)) {
                if((!app->mode_hold_active) || (app->mode_hold_key != input->key)) {
                    app->mode_hold_active = true;
                    app->mode_hold_key = input->key;
                }
            }

            AutofireMode next_mode =
                (input->key == InputKeyUp) ? usb_hid_autofire_prev_mode(app->mode) :
                                             usb_hid_autofire_next_mode(app->mode);
            usb_hid_autofire_set_mode(app, next_mode);
            break;
        }

        case InputTypeRelease:
            if(app->mode_hold_active && (app->mode_hold_key == input->key)) {
                app->mode_hold_active = false;
                app->mode_hold_key = InputKeyMAX;
            }
            break;

        default:
            break;
    }
}

static void usb_hid_autofire_stop(UsbHidAutofireApp* app) {
    app->active = false;
    app->click_phase = ClickPhasePress;
    if(app->click_timer) {
        furi_timer_stop(app->click_timer);
    }
    if(app->ui_refresh_timer) {
        furi_timer_stop(app->ui_refresh_timer);
    }

    if(app->mouse_pressed) {
        usb_hid_autofire_release_mode_control(app);
        app->mouse_pressed = false;
    }
    furi_hal_hid_kb_release_all();

    usb_hid_autofire_reset_cps_tracking(app);
    app->adjust_hold_active = false;
    app->adjust_hold_key = InputKeyMAX;
    app->adjust_repeat_count = 0U;
    app->mode_hold_active = false;
    app->mode_hold_key = InputKeyMAX;
    app->ok_long_handled = false;
    app->back_long_handled = false;
}

static void usb_hid_autofire_tick(UsbHidAutofireApp* app) {
    if(!app->active) {
        return;
    }

    if(app->click_phase == ClickPhasePress) {
        usb_hid_autofire_press_mode_control(app);
        app->mouse_pressed = true;
        app->click_phase = ClickPhaseRelease;
    } else {
        usb_hid_autofire_release_mode_control(app);
        app->mouse_pressed = false;
        usb_hid_autofire_record_click_release(app);
        app->click_phase = ClickPhasePress;
    }

    usb_hid_autofire_schedule_next_tick(app);
}

int32_t usb_hid_autofire_app(void* p) {
    UNUSED(p);
    int32_t ret = -1;
    bool gui_opened = false;
    bool view_port_added = false;
    bool usb_switched = false;

    UsbHidAutofireApp app = {
        .event_queue = NULL,
        .view_port = NULL,
        .gui = NULL,
        .dialogs = NULL,
        .click_timer = NULL,
        .ui_refresh_timer = NULL,
        .settings_save_timer = NULL,
        .usb_mode_prev = NULL,
        .active = false,
        .mouse_pressed = false,
        .ui_dirty = true,
        .settings_dirty = false,
        .show_help = false,
        .last_active_state = false,
        .autofire_delay_ms = AUTOFIRE_DELAY_DEFAULT_MS,
        .realtime_cps_x10 = 0U,
        .last_click_release_tick_ms = 0U,
        .last_click_interval_ms = 0U,
        .adjust_hold_active = false,
        .adjust_hold_key = InputKeyMAX,
        .adjust_repeat_count = 0U,
        .mode_hold_active = false,
        .mode_hold_key = InputKeyMAX,
        .ok_long_handled = false,
        .back_long_handled = false,
        .mode = AutofireModeMouseLeftClick,
        .preset = AutofirePresetCustom,
        .startup_policy = AutofireStartupPolicyPausedOnLaunch,
        .click_phase = ClickPhasePress,
    };

    app.autofire_delay_ms = usb_hid_autofire_delay_clamp(app.autofire_delay_ms);
    usb_hid_autofire_settings_load(&app);
    usb_hid_autofire_reset_cps_tracking(&app);

    app.event_queue = furi_message_queue_alloc(16, sizeof(UsbMouseEvent));
    if(!app.event_queue) {
        FURI_LOG_E(TAG, "Failed to allocate event queue");
        goto cleanup;
    }

    app.view_port = view_port_alloc();
    if(!app.view_port) {
        FURI_LOG_E(TAG, "Failed to allocate viewport");
        goto cleanup;
    }

    app.click_timer = furi_timer_alloc(usb_hid_autofire_timer_callback, FuriTimerTypeOnce, &app);
    if(!app.click_timer) {
        FURI_LOG_E(TAG, "Failed to allocate timer");
        goto cleanup;
    }

    app.ui_refresh_timer = furi_timer_alloc(usb_hid_autofire_ui_timer_callback, FuriTimerTypePeriodic, &app);
    if(!app.ui_refresh_timer) {
        FURI_LOG_E(TAG, "Failed to allocate UI refresh timer");
        goto cleanup;
    }

    app.settings_save_timer =
        furi_timer_alloc(usb_hid_autofire_settings_save_timer_callback, FuriTimerTypeOnce, &app);
    if(!app.settings_save_timer) {
        FURI_LOG_E(TAG, "Failed to allocate settings save timer");
        goto cleanup;
    }

    app.usb_mode_prev = furi_hal_usb_get_config();
#ifndef USB_HID_AUTOFIRE_SCREENSHOT
    furi_hal_usb_unlock();
    if(!furi_hal_usb_set_config(&usb_hid, NULL)) {
        FURI_LOG_E(TAG, "Failed to switch USB to HID mode");
        goto cleanup;
    }
    usb_switched = true;
#endif

    view_port_draw_callback_set(app.view_port, usb_hid_autofire_render_callback, &app);
    view_port_input_callback_set(app.view_port, usb_hid_autofire_input_callback, app.event_queue);

    // Open GUI and register view_port
    app.gui = furi_record_open(RECORD_GUI);
    if(!app.gui) {
        FURI_LOG_E(TAG, "Failed to open GUI record");
        goto cleanup;
    }
    gui_opened = true;
    gui_add_view_port(app.gui, app.view_port, GuiLayerFullscreen);
    view_port_added = true;

    app.dialogs = furi_record_open(RECORD_DIALOGS);

    if((app.startup_policy == AutofireStartupPolicyRestoreLastState) && app.last_active_state) {
        usb_hid_autofire_start(&app);
    }

    view_port_update(app.view_port);
    app.ui_dirty = false;

    UsbMouseEvent event;
    while(1) {
        FuriStatus event_status = furi_message_queue_get(app.event_queue, &event, FuriWaitForever);

        if(event_status != FuriStatusOk) {
            continue;
        }

        if(event.type == EventTypeTick) {
            usb_hid_autofire_tick(&app);
        } else if(event.type == EventTypeUiRefresh) {
            uint32_t new_cps_x10 = usb_hid_autofire_realtime_cps_x10(&app);
            if(new_cps_x10 != app.realtime_cps_x10) {
                app.realtime_cps_x10 = new_cps_x10;
                app.ui_dirty = true;
            }
        } else if(event.type == EventTypeSettingsSave) {
            usb_hid_autofire_settings_flush_if_dirty(&app);
        } else if(event.type == EventTypeInput) {
            if(event.input.key == InputKeyBack) {
                if(event.input.type == InputTypeLong) {
                    app.show_help = true;
                    app.back_long_handled = true;
                    app.ui_dirty = true;
                    continue;
                }

                if(event.input.type == InputTypeRelease) {
                    if(app.back_long_handled) {
                        app.back_long_handled = false;
                        continue;
                    }
                    if(app.show_help) {
                        app.show_help = false;
                        app.ui_dirty = true;
                        continue;
                    }
                    break;
                }
            }

            if(app.show_help) {
                continue;
            }

            if((event.input.key == InputKeyUp) || (event.input.key == InputKeyDown)) {
                usb_hid_autofire_handle_mode_input(&app, &event.input);
                continue;
            }

            if((event.input.key == InputKeyLeft) || (event.input.key == InputKeyRight)) {
                usb_hid_autofire_handle_delay_input(&app, &event.input);
            } else if(event.input.key == InputKeyOk) {
                if(event.input.type == InputTypeLong) {
                    AutofirePreset next_preset = usb_hid_autofire_next_preset(app.preset);
                    usb_hid_autofire_apply_preset_request(&app, next_preset);
                    app.ok_long_handled = true;
                } else if(event.input.type == InputTypeRelease) {
                    if(app.ok_long_handled) {
                        app.ok_long_handled = false;
                    } else if(app.active) {
                        usb_hid_autofire_stop(&app);
                        app.last_active_state = false;
                        usb_hid_autofire_mark_settings_dirty(&app);
                    } else {
                        usb_hid_autofire_start(&app);
                        app.last_active_state = true;
                        usb_hid_autofire_mark_settings_dirty(&app);
                    }
                    app.ui_dirty = true;
                }
            }
        }

        if(app.ui_dirty) {
            view_port_update(app.view_port);
            app.ui_dirty = false;
        }
    }

    ret = 0;

cleanup:
    usb_hid_autofire_settings_flush_if_dirty(&app);
    usb_hid_autofire_stop(&app);

#ifndef USB_HID_AUTOFIRE_SCREENSHOT
    if(usb_switched) {
        furi_hal_usb_set_config(app.usb_mode_prev, NULL);
    }
#endif

    // remove & free all stuff created by app
    if(view_port_added && app.gui && app.view_port) {
        gui_remove_view_port(app.gui, app.view_port);
    }

    if(app.click_timer) {
        furi_timer_stop(app.click_timer);
        furi_timer_free(app.click_timer);
    }

    if(app.ui_refresh_timer) {
        furi_timer_stop(app.ui_refresh_timer);
        furi_timer_free(app.ui_refresh_timer);
    }

    if(app.settings_save_timer) {
        furi_timer_stop(app.settings_save_timer);
        furi_timer_free(app.settings_save_timer);
    }

    if(app.view_port) {
        view_port_free(app.view_port);
    }

    if(app.event_queue) {
        furi_message_queue_free(app.event_queue);
    }

    if(gui_opened) {
        furi_record_close(RECORD_GUI);
    }
    if(app.dialogs) {
        furi_record_close(RECORD_DIALOGS);
    }

    return ret;
}
