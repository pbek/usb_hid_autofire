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
#define AUTOFIRE_DELAY_DEFAULT_MS 10U

typedef enum {
    EventTypeInput,
    EventTypeTick,
} EventType;

typedef enum {
    ClickPhasePress,
    ClickPhaseRelease,
} ClickPhase;

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
    FuriHalUsbInterface* usb_mode_prev;
    bool active;
    bool mouse_pressed;
    uint32_t autofire_delay_ms;
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

static uint32_t usb_hid_autofire_delay_decrease(uint32_t delay_ms) {
    delay_ms = usb_hid_autofire_delay_clamp(delay_ms);
    if(delay_ms <= AUTOFIRE_DELAY_MIN_MS) {
        return AUTOFIRE_DELAY_MIN_MS;
    }

    if((delay_ms - AUTOFIRE_DELAY_MIN_MS) < AUTOFIRE_DELAY_STEP_MS) {
        return AUTOFIRE_DELAY_MIN_MS;
    }

    return delay_ms - AUTOFIRE_DELAY_STEP_MS;
}

static uint32_t usb_hid_autofire_delay_increase(uint32_t delay_ms) {
    delay_ms = usb_hid_autofire_delay_clamp(delay_ms);
    if(delay_ms >= AUTOFIRE_DELAY_MAX_MS) {
        return AUTOFIRE_DELAY_MAX_MS;
    }

    if((AUTOFIRE_DELAY_MAX_MS - delay_ms) < AUTOFIRE_DELAY_STEP_MS) {
        return AUTOFIRE_DELAY_MAX_MS;
    }

    return delay_ms + AUTOFIRE_DELAY_STEP_MS;
}

static void usb_hid_autofire_render_callback(Canvas* canvas, void* ctx) {
    UsbHidAutofireApp* app = ctx;
    char autofire_delay_str[12];
    snprintf(autofire_delay_str, sizeof(autofire_delay_str), "%lu", (unsigned long)app->autofire_delay_ms);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "USB HID Autofire");
    canvas_draw_str(canvas, 0, 34, app->active ? "<active>" : "<inactive>");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 90, 10, "v");
    canvas_draw_str(canvas, 96, 10, VERSION);
    canvas_draw_str(canvas, 0, 22, "Press [ok] for auto left clicking");
    canvas_draw_str(canvas, 0, 46, "delay [ms]:");
    canvas_draw_str(canvas, 50, 46, autofire_delay_str);
    canvas_draw_str(canvas, 0, 63, "Press [back] to exit");
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

static void usb_hid_autofire_stop(UsbHidAutofireApp* app) {
    app->active = false;
    app->click_phase = ClickPhasePress;
    if(app->click_timer) {
        furi_timer_stop(app->click_timer);
    }

    if(app->mouse_pressed) {
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        app->mouse_pressed = false;
    }
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
        .usb_mode_prev = NULL,
        .active = false,
        .mouse_pressed = false,
        .autofire_delay_ms = AUTOFIRE_DELAY_DEFAULT_MS,
        .click_phase = ClickPhasePress,
    };

    app.autofire_delay_ms = usb_hid_autofire_delay_clamp(app.autofire_delay_ms);

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

    UsbMouseEvent event;
    while(1) {
        FuriStatus event_status = furi_message_queue_get(app.event_queue, &event, 50);

        if(event_status == FuriStatusOk) {
            if(event.type == EventTypeTick) {
                usb_hid_autofire_tick(&app);
            } else if(event.type == EventTypeInput) {
                if(event.input.key == InputKeyBack) {
                    break;
                }

                if(event.input.type != InputTypeRelease) {
                    continue;
                }

                switch(event.input.key) {
                    case InputKeyOk:
                        if(app.active) {
                            usb_hid_autofire_stop(&app);
                        } else {
                            app.active = true;
                            app.click_phase = ClickPhasePress;
                            usb_hid_autofire_tick(&app);
                        }
                        break;
                    case InputKeyLeft:
                        {
                            uint32_t new_delay_ms =
                                usb_hid_autofire_delay_decrease(app.autofire_delay_ms);
                            if(new_delay_ms != app.autofire_delay_ms) {
                                app.autofire_delay_ms = new_delay_ms;
                                if(app.active) {
                                    furi_timer_stop(app.click_timer);
                                    usb_hid_autofire_schedule_next_tick(&app);
                                }
                            }
                        }
                        break;
                    case InputKeyRight:
                        {
                            uint32_t new_delay_ms =
                                usb_hid_autofire_delay_increase(app.autofire_delay_ms);
                            if(new_delay_ms != app.autofire_delay_ms) {
                                app.autofire_delay_ms = new_delay_ms;
                                if(app.active) {
                                    furi_timer_stop(app.click_timer);
                                    usb_hid_autofire_schedule_next_tick(&app);
                                }
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        view_port_update(app.view_port);
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
        furi_timer_free(app.click_timer);
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
