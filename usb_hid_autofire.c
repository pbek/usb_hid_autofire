#include <stdio.h>
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include "version.h"

#define TAG "usb_hid_autofire"

// Uncomment to be able to make a screenshot
//#define USB_HID_AUTOFIRE_SCREENSHOT

#define AUTOFIRE_DELAY_MIN_MS 5U
#define AUTOFIRE_DELAY_MAX_MS 1000U
#define AUTOFIRE_DELAY_STEP_MS 10U
#define AUTOFIRE_DELAY_ACCEL_MEDIUM_MS 50U
#define AUTOFIRE_DELAY_ACCEL_FAST_MS 100U
#define AUTOFIRE_REPEAT_MEDIUM_THRESHOLD 3U
#define AUTOFIRE_REPEAT_FAST_THRESHOLD 8U
#define AUTOFIRE_DELAY_DEFAULT_MS 10U
#define UI_REFRESH_PERIOD_MS 250U

typedef enum {
    EventTypeInput,
    EventTypeTick,
    EventTypeUiRefresh,
} EventType;

typedef enum {
    ClickPhasePress,
    ClickPhaseRelease,
} ClickPhase;

typedef enum {
    AutofireModeLeftClick,
} AutofireMode;

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
    FuriTimer* click_timer;
    FuriTimer* ui_refresh_timer;
    FuriHalUsbInterface* usb_mode_prev;
    bool active;
    bool mouse_pressed;
    bool ui_dirty;
    uint32_t autofire_delay_ms;
    uint32_t realtime_cps_x10;
    uint32_t last_click_release_tick_ms;
    uint32_t last_click_interval_ms;
    bool adjust_hold_active;
    InputKey adjust_hold_key;
    uint16_t adjust_repeat_count;
    AutofireMode mode;
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

static const char* usb_hid_autofire_mode_label(AutofireMode mode) {
    switch(mode) {
        case AutofireModeLeftClick:
            return "Left Click";
        default:
            return "Unknown";
    }
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

static void usb_hid_autofire_render_callback(Canvas* canvas, void* ctx) {
    UsbHidAutofireApp* app = ctx;
    char mode_str[24];
    char delay_str[24];
    char cps_str[24];
    snprintf(mode_str, sizeof(mode_str), "Mode: %s", usb_hid_autofire_mode_label(app->mode));
    snprintf(delay_str, sizeof(delay_str), "Delay: %lu ms", (unsigned long)app->autofire_delay_ms);
    usb_hid_autofire_format_cps(cps_str, sizeof(cps_str), app->realtime_cps_x10);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "USB HID Autofire");
    canvas_draw_str(canvas, 90, 10, "v");
    canvas_draw_str(canvas, 96, 10, VERSION);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 22, app->active ? "Status: ACTIVE" : "Status: PAUSED");
    canvas_draw_str(canvas, 0, 32, mode_str);
    canvas_draw_str(canvas, 0, 42, delay_str);
    canvas_draw_str(canvas, 0, 52, "Rate:");
    canvas_draw_str(canvas, 32, 52, cps_str);
    canvas_draw_str(canvas, 62, 52, "CPS");
    canvas_draw_str(canvas, 0, 63, "OK toggle  Back exit");
}

static void usb_hid_autofire_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;

    UsbMouseEvent event;
    event.type = EventTypeInput;
    event.input = *input_event;
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
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

static bool usb_hid_autofire_set_delay(UsbHidAutofireApp* app, uint32_t new_delay_ms) {
    new_delay_ms = usb_hid_autofire_delay_clamp(new_delay_ms);
    if(new_delay_ms == app->autofire_delay_ms) {
        return false;
    }

    app->autofire_delay_ms = new_delay_ms;
    if(app->active) {
        furi_timer_stop(app->click_timer);
        usb_hid_autofire_schedule_next_tick(app);
    }
    app->ui_dirty = true;

    return true;
}

static void usb_hid_autofire_adjust_delay(UsbHidAutofireApp* app, InputKey key, uint32_t step_ms) {
    if(key == InputKeyLeft) {
        usb_hid_autofire_set_delay(
            app, usb_hid_autofire_delay_decrease(app->autofire_delay_ms, step_ms));
    } else if(key == InputKeyRight) {
        usb_hid_autofire_set_delay(
            app, usb_hid_autofire_delay_increase(app->autofire_delay_ms, step_ms));
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
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        app->mouse_pressed = false;
    }

    usb_hid_autofire_reset_cps_tracking(app);
    app->adjust_hold_active = false;
    app->adjust_hold_key = InputKeyMAX;
    app->adjust_repeat_count = 0U;
}

static void usb_hid_autofire_tick(UsbHidAutofireApp* app) {
    if(!app->active) {
        return;
    }

    if(app->click_phase == ClickPhasePress) {
        furi_hal_hid_mouse_press(HID_MOUSE_BTN_LEFT);
        app->mouse_pressed = true;
        app->click_phase = ClickPhaseRelease;
    } else {
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
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
        .click_timer = NULL,
        .ui_refresh_timer = NULL,
        .usb_mode_prev = NULL,
        .active = false,
        .mouse_pressed = false,
        .ui_dirty = true,
        .autofire_delay_ms = AUTOFIRE_DELAY_DEFAULT_MS,
        .realtime_cps_x10 = 0U,
        .last_click_release_tick_ms = 0U,
        .last_click_interval_ms = 0U,
        .adjust_hold_active = false,
        .adjust_hold_key = InputKeyMAX,
        .adjust_repeat_count = 0U,
        .mode = AutofireModeLeftClick,
        .click_phase = ClickPhasePress,
    };

    app.autofire_delay_ms = usb_hid_autofire_delay_clamp(app.autofire_delay_ms);
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
        } else if(event.type == EventTypeInput) {
            if(event.input.key == InputKeyBack) {
                break;
            }

            if((event.input.key == InputKeyLeft) || (event.input.key == InputKeyRight)) {
                usb_hid_autofire_handle_delay_input(&app, &event.input);
            } else if((event.input.key == InputKeyOk) && (event.input.type == InputTypeRelease)) {
                if(app.active) {
                    usb_hid_autofire_stop(&app);
                } else {
                    app.active = true;
                    app.click_phase = ClickPhasePress;
                    usb_hid_autofire_reset_cps_tracking(&app);
                    furi_timer_start(app.ui_refresh_timer, furi_ms_to_ticks(UI_REFRESH_PERIOD_MS));
                    usb_hid_autofire_tick(&app);
                }
                app.ui_dirty = true;
            }
        }

        if(app.ui_dirty) {
            view_port_update(app.view_port);
            app.ui_dirty = false;
        }
    }

    ret = 0;

cleanup:
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

    if(app.view_port) {
        view_port_free(app.view_port);
    }

    if(app.event_queue) {
        furi_message_queue_free(app.event_queue);
    }

    if(gui_opened) {
        furi_record_close(RECORD_GUI);
    }

    return ret;
}
