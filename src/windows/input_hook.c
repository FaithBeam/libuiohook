/* libUIOHook: Cross-platform keyboard and mouse hooking from userland.
 * Copyright (C) 2006-2022 Alexander Barker.  All Rights Reserved.
 * https://github.com/kwhat/libuiohook/
 *
 * libUIOHook is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libUIOHook is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <inttypes.h>
#include <uiohook.h>
#include <windows.h>

#include "input_helper.h"
#include "logger.h"
#include "monitor_helper.h"

// Thread and hook handles.
static DWORD hook_thread_id = 0;
static HHOOK keyboard_event_hhook = NULL, mouse_event_hhook = NULL;
static HWINEVENTHOOK win_event_hhook = NULL;
static HWND invisible_win_hwnd = NULL;

// The handle to the DLL module pulled in DllMain on DLL_PROCESS_ATTACH.
extern HINSTANCE hInst;

// Modifiers for tracking key masks.
static unsigned short int current_modifiers = 0x0000;

#ifdef USE_EPOCH_TIME
// Structure for the current Unix epoch in milliseconds.
static FILETIME system_time;
#endif

// Click count globals.
static unsigned short click_count = 0;
static uint64_t click_time = 0;
static unsigned short int click_button = MOUSE_NOBUTTON;
static POINT last_click;

// Static event memory.
static uiohook_event event;

// Event dispatch callback.
static dispatcher_t dispatch = NULL;
static void *dispatch_data = NULL;

UIOHOOK_API void hook_set_dispatch_proc(dispatcher_t dispatch_proc, void *user_data) {
    logger(LOG_LEVEL_DEBUG, "%s [%u]: Setting new dispatch callback to %#p.\n",
            __FUNCTION__, __LINE__, dispatch_proc);

    dispatch = dispatch_proc;
    dispatch_data = user_data;
}

// Send out an event if a dispatcher was set.
static void dispatch_event(uiohook_event *const event) {
    if (dispatch != NULL) {
        logger(LOG_LEVEL_DEBUG, "%s [%u]: Dispatching event type %u.\n",
                __FUNCTION__, __LINE__, event->type);

        dispatch(event, dispatch_data);
    } else {
        logger(LOG_LEVEL_WARN, "%s [%u]: No dispatch callback set!\n",
                __FUNCTION__, __LINE__);
    }
}


// Set the native modifier mask for future events.
static inline void set_modifier_mask(unsigned short int mask) {
    current_modifiers |= mask;
}

// Unset the native modifier mask for future events.
static inline void unset_modifier_mask(unsigned short int mask) {
    current_modifiers &= ~mask;
}

// Get the current native modifier mask state.
static inline unsigned short int get_modifiers() {
    return current_modifiers;
}

// Initialize the modifier mask to the current modifiers.
static void initialize_modifiers() {
    current_modifiers = 0x0000;

    // NOTE We are checking the high order bit, so it will be < 0 for a singed short.
    if (GetKeyState(VK_LSHIFT)   < 0) { set_modifier_mask(MASK_SHIFT_L);     }
    if (GetKeyState(VK_RSHIFT)   < 0) { set_modifier_mask(MASK_SHIFT_R);     }
    if (GetKeyState(VK_LCONTROL) < 0) { set_modifier_mask(MASK_CTRL_L);      }
    if (GetKeyState(VK_RCONTROL) < 0) { set_modifier_mask(MASK_CTRL_R);      }
    if (GetKeyState(VK_LMENU)    < 0) { set_modifier_mask(MASK_ALT_L);       }
    if (GetKeyState(VK_RMENU)    < 0) { set_modifier_mask(MASK_ALT_R);       }
    if (GetKeyState(VK_LWIN)     < 0) { set_modifier_mask(MASK_META_L);      }
    if (GetKeyState(VK_RWIN)     < 0) { set_modifier_mask(MASK_META_R);      }

    if (GetKeyState(VK_LBUTTON)  < 0) { set_modifier_mask(MASK_BUTTON1);     }
    if (GetKeyState(VK_RBUTTON)  < 0) { set_modifier_mask(MASK_BUTTON2);     }
    if (GetKeyState(VK_MBUTTON)  < 0) { set_modifier_mask(MASK_BUTTON3);     }
    if (GetKeyState(VK_XBUTTON1) < 0) { set_modifier_mask(MASK_BUTTON4);     }
    if (GetKeyState(VK_XBUTTON2) < 0) { set_modifier_mask(MASK_BUTTON5);     }

    if (GetKeyState(VK_NUMLOCK)  < 0) { set_modifier_mask(MASK_NUM_LOCK);    }
    if (GetKeyState(VK_CAPITAL)  < 0) { set_modifier_mask(MASK_CAPS_LOCK);   }
    if (GetKeyState(VK_SCROLL)   < 0) { set_modifier_mask(MASK_SCROLL_LOCK); }
}


/* Retrieves the mouse wheel scroll type. This function cannot be included as
 * part of the input_helper.h due to platform specific calling restrictions.
 */
static uint8_t get_scroll_wheel_type() {
    uint8_t value;
    UINT wheel_type;

    SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &wheel_type, 0);
    if (wheel_type == WHEEL_PAGESCROLL) {
        value = WHEEL_BLOCK_SCROLL;
    } else {
        value = WHEEL_UNIT_SCROLL;
    }

    return value;
}

/* Retrieves the mouse wheel scroll amount. This function cannot be included as
 * part of the input_helper.h due to platform specific calling restrictions.
 */
static uint16_t get_scroll_wheel_amount() {
    uint16_t value;
    UINT wheel_amount;

    SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &wheel_amount, 0);
    if (wheel_amount == WHEEL_PAGESCROLL) {
        value = 1;
    } else {
        value = (uint16_t) wheel_amount;
    }

    return value;
}

#ifdef USE_EPOCH_TIME
static inline uint64_t get_unix_timestamp() {
	// Get the local system time in UTC.
	GetSystemTimeAsFileTime(&system_time);

	// Convert the local system time to a Unix epoch in MS.
	// milliseconds = 100-nanoseconds / 10000
	uint64_t timestamp = (((uint64_t) system_time.dwHighDateTime << 32) | system_time.dwLowDateTime) / 10000;

	// Convert Windows epoch to Unix epoch. (1970 - 1601 in milliseconds)
    timestamp -= 11644473600000;

	return timestamp;
}
#endif

void unregister_running_hooks() {
    // Stop the event hook and any timer still running.
    if (win_event_hhook != NULL) {
        UnhookWinEvent(win_event_hhook);
        win_event_hhook = NULL;
    }

    // Destroy the native hooks.
    if (keyboard_event_hhook != NULL) {
        UnhookWindowsHookEx(keyboard_event_hhook);
        keyboard_event_hhook = NULL;
    }

    if (mouse_event_hhook != NULL) {
        UnhookWindowsHookEx(mouse_event_hhook);
        mouse_event_hhook = NULL;
    }
}

void hook_start_proc() {
    // Initialize native input helper functions.
    load_input_helper();

    // Get the local system time in UNIX epoch form.
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = GetMessageTime();
    #endif

    // Populate the hook start event.
    event.time = timestamp;
    event.reserved = 0x00;

    event.type = EVENT_HOOK_ENABLED;
    event.mask = 0x00;

    // Fire the hook start event.
    dispatch_event(&event);
}

void hook_stop_proc() {
    // Get the local system time in UNIX epoch form.
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = GetMessageTime();
    #endif

    // Populate the hook stop event.
    event.time = timestamp;
    event.reserved = 0x00;

    event.type = EVENT_HOOK_DISABLED;
    event.mask = 0x00;

    // Fire the hook stop event.
    dispatch_event(&event);

    // Deinitialize native input helper functions.
    unload_input_helper();
}

static void process_key_pressed(KBDLLHOOKSTRUCT *kbhook) {
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = kbhook->time;
    #endif

    // Check and setup modifiers.
    if      (kbhook->vkCode == VK_LSHIFT)   { set_modifier_mask(MASK_SHIFT_L);     }
    else if (kbhook->vkCode == VK_RSHIFT)   { set_modifier_mask(MASK_SHIFT_R);     }
    else if (kbhook->vkCode == VK_LCONTROL) { set_modifier_mask(MASK_CTRL_L);      }
    else if (kbhook->vkCode == VK_RCONTROL) { set_modifier_mask(MASK_CTRL_R);      }
    else if (kbhook->vkCode == VK_LMENU)    { set_modifier_mask(MASK_ALT_L);       }
    else if (kbhook->vkCode == VK_RMENU)    { set_modifier_mask(MASK_ALT_R);       }
    else if (kbhook->vkCode == VK_LWIN)     { set_modifier_mask(MASK_META_L);      }
    else if (kbhook->vkCode == VK_RWIN)     { set_modifier_mask(MASK_META_R);      }
    else if (kbhook->vkCode == VK_NUMLOCK)  { set_modifier_mask(MASK_NUM_LOCK);    }
    else if (kbhook->vkCode == VK_CAPITAL)  { set_modifier_mask(MASK_CAPS_LOCK);   }
    else if (kbhook->vkCode == VK_SCROLL)   { set_modifier_mask(MASK_SCROLL_LOCK); }

    // Populate key pressed event.
    event.time = timestamp;
    event.reserved = 0x00;

    event.type = EVENT_KEY_PRESSED;
    event.mask = get_modifiers();

    event.data.keyboard.keycode = keycode_to_scancode(kbhook->vkCode, kbhook->flags);
    event.data.keyboard.rawcode = (uint16_t) kbhook->vkCode;
    event.data.keyboard.keychar = CHAR_UNDEFINED;

    logger(LOG_LEVEL_DEBUG, "%s [%u]: Key %#X pressed. (%#X)\n",
            __FUNCTION__, __LINE__, event.data.keyboard.keycode, event.data.keyboard.rawcode);

    // Populate key pressed event.
    dispatch_event(&event);

    // If the pressed event was not consumed...
    if (event.reserved ^ 0x01) {
        // Buffer for unicode typed chars. No more than 2 needed.
        WCHAR buffer[2]; // = { WCH_NONE };

        // If the pressed event was not consumed and a unicode char exists...
        SIZE_T count = keycode_to_unicode(kbhook->vkCode, buffer, sizeof(buffer));
        for (unsigned int i = 0; i < count; i++) {
            // Populate key typed event.
            event.time = timestamp;
            event.reserved = 0x00;

            event.type = EVENT_KEY_TYPED;
            event.mask = get_modifiers();

            event.data.keyboard.keycode = VC_UNDEFINED;
            event.data.keyboard.rawcode = (uint16_t) kbhook->vkCode;
            event.data.keyboard.keychar = buffer[i];

            logger(LOG_LEVEL_DEBUG, "%s [%u]: Key %#X typed. (%lc)\n",
                    __FUNCTION__, __LINE__, event.data.keyboard.keycode, (wint_t) event.data.keyboard.keychar);

            // Fire key typed event.
            dispatch_event(&event);
        }
    }
}

static void process_key_released(KBDLLHOOKSTRUCT *kbhook) {
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = kbhook->time;
    #endif

    // Check and setup modifiers.
    if      (kbhook->vkCode == VK_LSHIFT)   { unset_modifier_mask(MASK_SHIFT_L);     }
    else if (kbhook->vkCode == VK_RSHIFT)   { unset_modifier_mask(MASK_SHIFT_R);     }
    else if (kbhook->vkCode == VK_LCONTROL) { unset_modifier_mask(MASK_CTRL_L);      }
    else if (kbhook->vkCode == VK_RCONTROL) { unset_modifier_mask(MASK_CTRL_R);      }
    else if (kbhook->vkCode == VK_LMENU)    { unset_modifier_mask(MASK_ALT_L);       }
    else if (kbhook->vkCode == VK_RMENU)    { unset_modifier_mask(MASK_ALT_R);       }
    else if (kbhook->vkCode == VK_LWIN)     { unset_modifier_mask(MASK_META_L);      }
    else if (kbhook->vkCode == VK_RWIN)     { unset_modifier_mask(MASK_META_R);      }
    else if (kbhook->vkCode == VK_NUMLOCK)  { unset_modifier_mask(MASK_NUM_LOCK);    }
    else if (kbhook->vkCode == VK_CAPITAL)  { unset_modifier_mask(MASK_CAPS_LOCK);   }
    else if (kbhook->vkCode == VK_SCROLL)   { unset_modifier_mask(MASK_SCROLL_LOCK); }

    // Populate key pressed event.
    event.time = timestamp;
    event.reserved = 0x00;

    event.type = EVENT_KEY_RELEASED;
    event.mask = get_modifiers();

    event.data.keyboard.keycode = keycode_to_scancode(kbhook->vkCode, kbhook->flags);
    event.data.keyboard.rawcode = (uint16_t) kbhook->vkCode;
    event.data.keyboard.keychar = CHAR_UNDEFINED;

    logger(LOG_LEVEL_DEBUG, "%s [%u]: Key %#X released. (%#X)\n",
            __FUNCTION__, __LINE__, event.data.keyboard.keycode, event.data.keyboard.rawcode);

    // Fire key released event.
    dispatch_event(&event);
}

LRESULT CALLBACK keyboard_hook_event_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    KBDLLHOOKSTRUCT *kbhook = (KBDLLHOOKSTRUCT *) lParam;
    switch (wParam) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            process_key_pressed(kbhook);
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            process_key_released(kbhook);
            break;

        default:
            // In theory this *should* never execute.
            logger(LOG_LEVEL_DEBUG, "%s [%u]: Unhandled Windows keyboard event: %#X.\n",
                    __FUNCTION__, __LINE__, (unsigned int) wParam);
            break;
    }

    LRESULT hook_result = -1;
    if (nCode < 0 || event.reserved ^ 0x01) {
        hook_result = CallNextHookEx(keyboard_event_hhook, nCode, wParam, lParam);
    } else {
        logger(LOG_LEVEL_DEBUG, "%s [%u]: Consuming the current event. (%li)\n",
                __FUNCTION__, __LINE__, (long) hook_result);
    }

    return hook_result;
}


static void process_button_pressed(MSLLHOOKSTRUCT *mshook, uint16_t button) {
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = mshook->time;
    #endif

    // Track the number of clicks, the button must match the previous button.
    if (button == click_button && (long int) (timestamp - click_time) <= hook_get_multi_click_time()) {
        if (click_count < USHRT_MAX) {
            click_count++;
        } else {
            logger(LOG_LEVEL_WARN, "%s [%u]: Click count overflow detected!\n",
                    __FUNCTION__, __LINE__);
        }
    } else {
        // Reset the click count.
        click_count = 1;

        // Set the previous button.
        click_button = button;
    }

    // Save this events time to calculate the click_count.
    click_time = timestamp;

    // Store the last click point.
    last_click.x = mshook->pt.x;
    last_click.y = mshook->pt.y;

    // Populate mouse pressed event.
    event.time = timestamp;
    event.reserved = 0x00;

    event.type = EVENT_MOUSE_PRESSED;
    event.mask = get_modifiers();

    event.data.mouse.button = button;
    event.data.mouse.clicks = click_count;

    event.data.mouse.x = (int16_t) mshook->pt.x;
    event.data.mouse.y = (int16_t) mshook->pt.y;

    logger(LOG_LEVEL_DEBUG, "%s [%u]: Button %u  pressed %u time(s). (%u, %u)\n",
            __FUNCTION__, __LINE__, event.data.mouse.button, event.data.mouse.clicks,
            event.data.mouse.x, event.data.mouse.y);

    // Fire mouse pressed event.
    dispatch_event(&event);
}

static void process_button_released(MSLLHOOKSTRUCT *mshook, uint16_t button) {
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = mshook->time;
    #endif

    // Populate mouse released event.
    event.time = timestamp;
    event.reserved = 0x00;

    event.type = EVENT_MOUSE_RELEASED;
    event.mask = get_modifiers();

    event.data.mouse.button = button;
    event.data.mouse.clicks = click_count;

    event.data.mouse.x = (int16_t) mshook->pt.x;
    event.data.mouse.y = (int16_t) mshook->pt.y;

    logger(LOG_LEVEL_DEBUG, "%s [%u]: Button %u released %u time(s). (%u, %u)\n",
            __FUNCTION__, __LINE__, event.data.mouse.button,
            event.data.mouse.clicks,
            event.data.mouse.x, event.data.mouse.y);

    // Fire mouse released event.
    dispatch_event(&event);

    // If the pressed event was not consumed...
    if (event.reserved ^ 0x01 && last_click.x == mshook->pt.x && last_click.y == mshook->pt.y) {
        // Populate mouse clicked event.
        event.time = timestamp;
        event.reserved = 0x00;

        event.type = EVENT_MOUSE_CLICKED;
        event.mask = get_modifiers();

        event.data.mouse.button = button;
        event.data.mouse.clicks = click_count;
        event.data.mouse.x = (int16_t) mshook->pt.x;
        event.data.mouse.y = (int16_t) mshook->pt.y;

        logger(LOG_LEVEL_DEBUG, "%s [%u]: Button %u clicked %u time(s). (%u, %u)\n",
                __FUNCTION__, __LINE__, event.data.mouse.button, event.data.mouse.clicks,
                event.data.mouse.x, event.data.mouse.y);

        // Fire mouse clicked event.
        dispatch_event(&event);
    }

    // Reset the number of clicks.
    if (button == click_button && (long int) (timestamp - click_time) > hook_get_multi_click_time()) {
        // Reset the click count.
        click_count = 0;
    }
}

static void process_mouse_moved(MSLLHOOKSTRUCT *mshook) {
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = mshook->time;
    #endif

    // We received a mouse move event with the mouse actually moving.
    // This verifies that the mouse was moved after being depressed.
    if (last_click.x != mshook->pt.x || last_click.y != mshook->pt.y) {
        // Reset the click count.
        if (click_count != 0 && (long) (timestamp - click_time) > hook_get_multi_click_time()) {
            click_count = 0;
        }

        // Populate mouse move event.
        event.time = timestamp;
        event.reserved = 0x00;

        event.mask = get_modifiers();

        // Check the modifier mask range for MASK_BUTTON1 - 5.
        bool mouse_dragged = event.mask & (MASK_BUTTON1 | MASK_BUTTON2 | MASK_BUTTON3 | MASK_BUTTON4 | MASK_BUTTON5);
        if (mouse_dragged) {
            // Create Mouse Dragged event.
            event.type = EVENT_MOUSE_DRAGGED;
        } else {
            // Create a Mouse Moved event.
            event.type = EVENT_MOUSE_MOVED;
        }

        event.data.mouse.button = MOUSE_NOBUTTON;
        event.data.mouse.clicks = click_count;
        event.data.mouse.x = (int16_t) mshook->pt.x;
        event.data.mouse.y = (int16_t) mshook->pt.y;

        logger(LOG_LEVEL_DEBUG, "%s [%u]: Mouse %s to %u, %u.\n",
                __FUNCTION__, __LINE__,  mouse_dragged ? "dragged" : "moved",
                event.data.mouse.x, event.data.mouse.y);

        // Fire mouse move event.
        dispatch_event(&event);
    }
}

static void process_mouse_wheel(MSLLHOOKSTRUCT *mshook, uint8_t direction) {
    #ifdef USE_EPOCH_TIME
    uint64_t timestamp = get_unix_timestamp();
    #else
    uint64_t timestamp = mshook->time;
    #endif

    // Track the number of clicks.
    // Reset the click count and previous button.
    click_count = 1;
    click_button = MOUSE_NOBUTTON;

    // Populate mouse wheel event.
    event.time = timestamp;
    event.reserved = 0x00;

    event.type = EVENT_MOUSE_WHEEL;
    event.mask = get_modifiers();

    event.data.wheel.clicks = click_count;
    event.data.wheel.x = (int16_t) mshook->pt.x;
    event.data.wheel.y = (int16_t) mshook->pt.y;

    event.data.wheel.type = get_scroll_wheel_type();
    event.data.wheel.amount = get_scroll_wheel_amount();

    /* Delta HIWORD(mshook->mouseData)
     * A positive value indicates that the wheel was rotated
     * forward, away from the user; a negative value indicates that
     * the wheel was rotated backward, toward the user. One wheel
     * click is defined as WHEEL_DELTA, which is 120. */
    event.data.wheel.rotation = (int16_t) HIWORD(mshook->mouseData);

    // Vertical direction needs to be inverted on Windows to conform with other platforms.
    if (direction == WHEEL_VERTICAL_DIRECTION) {
        event.data.wheel.rotation *= -1;
    }

    // Set the direction based on what event was received.
    event.data.wheel.direction = direction;

    logger(LOG_LEVEL_DEBUG, "%s [%u]: Mouse wheel type %u, rotated %i units in the %u direction at %u, %u.\n",
            __FUNCTION__, __LINE__, event.data.wheel.type,
            event.data.wheel.amount * event.data.wheel.rotation,
            event.data.wheel.direction,
            event.data.wheel.x, event.data.wheel.y);

    // Fire mouse wheel event.
    dispatch_event(&event);
}

LRESULT CALLBACK mouse_hook_event_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    MSLLHOOKSTRUCT *mshook = (MSLLHOOKSTRUCT *) lParam;
    switch (wParam) {
        case WM_LBUTTONDOWN:
            set_modifier_mask(MASK_BUTTON1);
            process_button_pressed(mshook, MOUSE_BUTTON1);
            break;

        case WM_RBUTTONDOWN:
            set_modifier_mask(MASK_BUTTON2);
            process_button_pressed(mshook, MOUSE_BUTTON2);
            break;

        case WM_MBUTTONDOWN:
            set_modifier_mask(MASK_BUTTON3);
            process_button_pressed(mshook, MOUSE_BUTTON3);
            break;

        case WM_XBUTTONDOWN:
        case WM_NCXBUTTONDOWN:
            if (HIWORD(mshook->mouseData) == XBUTTON1) {
                set_modifier_mask(MASK_BUTTON4);
                process_button_pressed(mshook, MOUSE_BUTTON4);
            } else if (HIWORD(mshook->mouseData) == XBUTTON2) {
                set_modifier_mask(MASK_BUTTON5);
                process_button_pressed(mshook, MOUSE_BUTTON5);
            } else {
                // Extra mouse buttons.
                uint16_t button = HIWORD(mshook->mouseData);

                // Add support for mouse 4 & 5.
                if (button == 4) {
                    set_modifier_mask(MOUSE_BUTTON4);
                } else if (button == 5) {
                    set_modifier_mask(MOUSE_BUTTON5);
                }

                process_button_pressed(mshook, button);
            }
            break;


        case WM_LBUTTONUP:
            unset_modifier_mask(MASK_BUTTON1);
            process_button_released(mshook, MOUSE_BUTTON1);
            break;

        case WM_RBUTTONUP:
            unset_modifier_mask(MASK_BUTTON2);
            process_button_released(mshook, MOUSE_BUTTON2);
            break;

        case WM_MBUTTONUP:
            unset_modifier_mask(MASK_BUTTON3);
            process_button_released(mshook, MOUSE_BUTTON3);
            break;

        case WM_XBUTTONUP:
        case WM_NCXBUTTONUP:
            if (HIWORD(mshook->mouseData) == XBUTTON1) {
                unset_modifier_mask(MASK_BUTTON4);
                process_button_released(mshook, MOUSE_BUTTON4);
            } else if (HIWORD(mshook->mouseData) == XBUTTON2) {
                unset_modifier_mask(MASK_BUTTON5);
                process_button_released(mshook, MOUSE_BUTTON5);
            } else {
                // Extra mouse buttons.
                uint16_t button = HIWORD(mshook->mouseData);

                // Add support for mouse 4 & 5.
                if (button == 4) {
                    unset_modifier_mask(MOUSE_BUTTON4);
                } else if (button == 5) {
                    unset_modifier_mask(MOUSE_BUTTON5);
                }

                process_button_released(mshook, MOUSE_BUTTON5);
            }
            break;

        case WM_MOUSEMOVE:
            process_mouse_moved(mshook);
            break;

        case WM_MOUSEWHEEL:
            process_mouse_wheel(mshook, WHEEL_VERTICAL_DIRECTION);
            break;

        /* For horizontal scroll wheel support.
         * NOTE Windows >= Vista
         * case 0x020E:
         */
        case WM_MOUSEHWHEEL:
            process_mouse_wheel(mshook, WHEEL_HORIZONTAL_DIRECTION);
            break;

        default:
            // In theory this *should* never execute.
            logger(LOG_LEVEL_DEBUG, "%s [%u]: Unhandled Windows mouse event: %#X.\n",
                    __FUNCTION__, __LINE__, (unsigned int) wParam);
            break;
    }

    LRESULT hook_result = -1;
    if (nCode < 0 || event.reserved ^ 0x01) {
        hook_result = CallNextHookEx(mouse_event_hhook, nCode, wParam, lParam);
    } else {
        logger(LOG_LEVEL_DEBUG, "%s [%u]: Consuming the current event. (%li)\n",
                __FUNCTION__, __LINE__, (long) hook_result);
    }

    return hook_result;
}


// Callback function that handles events.
void CALLBACK win_hook_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hWnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    switch (event) {
        case EVENT_OBJECT_NAMECHANGE:
            logger(LOG_LEVEL_DEBUG, "%s [%u]: Restarting Windows input hook on window event: %#X.\n",
                    __FUNCTION__, __LINE__, event);

            // Remove any keyboard or mouse hooks that are still running.
            if (keyboard_event_hhook != NULL) {
                UnhookWindowsHookEx(keyboard_event_hhook);
            }

            if (mouse_event_hhook != NULL) {
                UnhookWindowsHookEx(mouse_event_hhook);
            }

            // Restart the event hooks.
            keyboard_event_hhook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_hook_event_proc, hInst, 0);
            mouse_event_hhook = SetWindowsHookEx(WH_MOUSE_LL, mouse_hook_event_proc, hInst, 0);

            // Re-initialize modifier masks.
            initialize_modifiers();

            // FIXME We should compare the modifier mask before and after the restart
            // to determine if we should synthesize missing events.

            // Check for event hook error.
            if (keyboard_event_hhook == NULL || mouse_event_hhook == NULL) {
                logger(LOG_LEVEL_ERROR, "%s [%u]: SetWindowsHookEx() failed! (%#lX)\n",
                        __FUNCTION__, __LINE__, (unsigned long) GetLastError());
            }
            break;

        default:
            logger(LOG_LEVEL_DEBUG, "%s [%u]: Unhandled Windows window event: %#X.\n",
                    __FUNCTION__, __LINE__, event);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        case WM_DISPLAYCHANGE:
            enumerate_displays();
            break;
        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

static int create_invisible_window()
{
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = WS_EX_NOACTIVATE;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInst;
    wcex.hIcon = NULL;
    wcex.hCursor = NULL;
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = "libuiohook";
    wcex.hIconSm = NULL;

    if (!RegisterClassEx(&wcex)) {
        return 0;
    }

    invisible_win_hwnd = CreateWindowEx(
            WS_EX_NOACTIVATE,
            "libuiohook",
            "Hidden Window to Monitor Display Change Events",
            WS_DISABLED,
            0,
            0,
            1,
            1,
            NULL,
            NULL,
            hInst,
            NULL
    );
    if (!invisible_win_hwnd) {
        return 0;
    }

    ShowWindow(invisible_win_hwnd, SW_HIDE);

    return 1;
}

UIOHOOK_API int hook_run() {
    int status = UIOHOOK_FAILURE;

    // Set the thread id we want to signal later.
    hook_thread_id = GetCurrentThreadId();

    // Spot check the hInst in case the library was statically linked and DllMain
    // did not receive a pointer on load.
    if (hInst == NULL) {
        logger(LOG_LEVEL_WARN, "%s [%u]: hInst was not set by DllMain().\n",
                __FUNCTION__, __LINE__);

        hInst = GetModuleHandle(NULL);
        if (hInst != NULL) {
            // Initialize native input helper functions.
            load_input_helper();
        } else {
            logger(LOG_LEVEL_ERROR, "%s [%u]: Could not determine hInst for SetWindowsHookEx()! (%#lX)\n",
                    __FUNCTION__, __LINE__, (unsigned long) GetLastError());

            status = UIOHOOK_ERROR_GET_MODULE_HANDLE;
        }
    }

    // Create invisible window to receive monitor change events
    if(!create_invisible_window() || invisible_win_hwnd == NULL)
    {
        logger(LOG_LEVEL_ERROR, "%s [%u]: Create invisible window failed! (%#lX)\n",
               __FUNCTION__, __LINE__, (unsigned long) GetLastError());

        status = UIOHOOK_ERROR_CREATE_INVISIBLE_WINDOW;
    }

    // Create the native hooks.
    keyboard_event_hhook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_hook_event_proc, hInst, 0);
    mouse_event_hhook = SetWindowsHookEx(WH_MOUSE_LL, mouse_hook_event_proc, hInst, 0);

    // Create a window event hook to listen for capture change.
    win_event_hhook = SetWinEventHook(
            EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
            NULL,
            win_hook_event_proc,
            0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // If we did not encounter a problem, start processing events.
    if (keyboard_event_hhook != NULL && mouse_event_hhook != NULL) {
        if (win_event_hhook == NULL) {
            logger(LOG_LEVEL_WARN, "%s [%u]: SetWinEventHook() failed!\n",
                    __FUNCTION__, __LINE__);
        }

        logger(LOG_LEVEL_DEBUG, "%s [%u]: SetWindowsHookEx() successful.\n",
                __FUNCTION__, __LINE__);

        // Check and setup modifiers.
        initialize_modifiers();

        // Set the exit status.
        status = UIOHOOK_SUCCESS;

        // Windows does not have a hook start event or callback so we need to
        // manually fake it.
        hook_start_proc();

        // Block until the thread receives an WM_QUIT request.
        MSG message;
        while (GetMessage(&message, (HWND) NULL, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    } else {
        logger(LOG_LEVEL_ERROR, "%s [%u]: SetWindowsHookEx() failed! (%#lX)\n",
                __FUNCTION__, __LINE__, (unsigned long) GetLastError());

        status = UIOHOOK_ERROR_SET_WINDOWS_HOOK_EX;
    }


    // Unregister any hooks that may still be installed.
    unregister_running_hooks();

    // We must explicitly call the cleanup handler because Windows does not
    // provide a thread cleanup method like POSIX pthread_cleanup_push/pop.
    hook_stop_proc();

    return status;
}

UIOHOOK_API int hook_stop() {
    int status = UIOHOOK_FAILURE;

    // Destroy the invisible window
    if (PostMessage(invisible_win_hwnd, WM_CLOSE, 0, 0)) {
        // Try to exit the thread naturally.
        if (PostThreadMessage(hook_thread_id, WM_QUIT, (WPARAM) NULL, (LPARAM) NULL)) {
            status = UIOHOOK_SUCCESS;
        }
    }

    logger(LOG_LEVEL_DEBUG, "%s [%u]: Status: %#X.\n",
            __FUNCTION__, __LINE__, status);

    return status;
}
