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

#include <stdio.h>
#include <uiohook.h>
#include <windows.h>

#include "input_helper.h"
#include "logger.h"
#include "monitor_helper.h"

// Some buggy versions of MinGW and MSys do not include these constants in winuser.h.
#ifndef MAPVK_VK_TO_VSC
#define MAPVK_VK_TO_VSC         0
#define MAPVK_VSC_TO_VK         1
#define MAPVK_VK_TO_CHAR        2
#define MAPVK_VSC_TO_VK_EX      3
#endif
// Some buggy versions of MinGW and MSys only define this value for Windows
// versions >= 0x0600 (Windows Vista) when it should be 0x0500 (Windows 2000).
#ifndef MAPVK_VK_TO_VSC_EX
#define MAPVK_VK_TO_VSC_EX      4
#endif

#ifndef KEYEVENTF_SCANCODE
#define KEYEVENTF_EXTENDEDKEY   0x0001
#define KEYEVENTF_KEYUP         0x0002
#define KEYEVENTF_UNICODE       0x0004
#define KEYEVENTF_SCANCODE      0x0008
#endif

#ifndef KEYEVENTF_KEYDOWN
#define KEYEVENTF_KEYDOWN       0x0000
#endif

#define MAX_WINDOWS_COORD_VALUE (1 << 16)

// TODO I doubt this table is complete.
// http://letcoderock.blogspot.fr/2011/10/sendinput-with-shift-key-not-work.html
static const uint16_t extend_key_table[10] = {
    VK_UP,
    VK_DOWN,
    VK_LEFT,
    VK_RIGHT,
    VK_HOME,
    VK_END,
    VK_PRIOR, // PgUp
    VK_NEXT,  //  PgDn
    VK_INSERT,
    VK_DELETE
};

typedef struct {
    LONG x;
    LONG y;
} normalized_coordinate;

static LONG get_absolute_coordinate(LONG coordinate, int screen_size) {
    return MulDiv((int) coordinate, MAX_WINDOWS_COORD_VALUE, screen_size);
}

static normalized_coordinate normalize_coordinates(LONG x, LONG y, int screen_width, int screen_height, LARGESTNEGATIVECOORDINATES lnc) {
    x += abs(lnc.left);
    y += abs(lnc.top);

    // Prevent clicking 0 coordinates to prevent monitor flicker
    if (x == 0)
    {
        x++;
    }
    if (y == 0)
    {
        y++;
    }

    normalized_coordinate nc = {
            .x = get_absolute_coordinate(x, screen_width),
            .y = get_absolute_coordinate(y, screen_height)
    };

    return nc;
}

static int map_keyboard_event(uiohook_event * const event, INPUT * const input) {
    input->type = INPUT_KEYBOARD; // | KEYEVENTF_SCANCODE
    //input->ki.time = GetSystemTime();

    switch (event->type) {
        case EVENT_KEY_PRESSED:
            input->ki.dwFlags = KEYEVENTF_KEYDOWN;
            break;

        case EVENT_KEY_RELEASED:
            input->ki.dwFlags = KEYEVENTF_KEYUP;
            break;

        default:
            logger(LOG_LEVEL_DEBUG, "%s [%u]: Invalid event for keyboard event mapping: %#X.\n",
                    __FUNCTION__, __LINE__, event->type);
            return UIOHOOK_FAILURE;
    }

    input->ki.wVk = (WORD) scancode_to_keycode(event->data.keyboard.keycode);
    if (input->ki.wVk == 0x0000) {
        logger(LOG_LEVEL_WARN, "%s [%u]: Unable to lookup scancode: %li\n",
                __FUNCTION__, __LINE__, event->data.keyboard.keycode);
        return UIOHOOK_FAILURE;
    }

    input->ki.wScan = MapVirtualKeyW(input->ki.wVk, MAPVK_VK_TO_VSC);

    // FIXME Why is this checking MASK_SHIFT
    if (event->mask & MASK_SHIFT) {
        for (int i = 0; i < sizeof(extend_key_table) / sizeof(uint16_t)
                && input->ki.wVk == extend_key_table[i]; i++) {
            input->ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    }

    return UIOHOOK_SUCCESS;
}

static int map_mouse_event(uiohook_event * const event, INPUT * const input, bool move_mouse) {
    input->type = INPUT_MOUSE;
    input->mi.mouseData = 0;
    input->mi.dwExtraInfo = 0;
    input->mi.time = 0; // GetSystemTime();

    if (move_mouse) {
        uint16_t screen_width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        uint16_t screen_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        LARGESTNEGATIVECOORDINATES lnc = get_largest_negative_coordinates();

        normalized_coordinate nc = normalize_coordinates(event->data.mouse.x, event->data.mouse.y, screen_width, screen_height, lnc);

        input->mi.dx = nc.x;
        input->mi.dy = nc.y;
    }

    switch (event->type) {
        case EVENT_MOUSE_PRESSED:
            if (event->data.mouse.button == MOUSE_NOBUTTON) {
                logger(LOG_LEVEL_WARN, "%s [%u]: No button specified for mouse pressed event!\n",
                        __FUNCTION__, __LINE__);
                return UIOHOOK_FAILURE;
            } else if (event->data.mouse.button == MOUSE_BUTTON1) {
                input->mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            } else if (event->data.mouse.button == MOUSE_BUTTON2) {
                input->mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            } else if (event->data.mouse.button == MOUSE_BUTTON3) {
                input->mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
            } else {
                input->mi.dwFlags = MOUSEEVENTF_XDOWN;
                if (event->data.mouse.button == MOUSE_BUTTON4) {
                    input->mi.mouseData = XBUTTON1;
                } else if (event->data.mouse.button == MOUSE_BUTTON5) {
                    input->mi.mouseData = XBUTTON2;
                } else {
                    input->mi.mouseData = event->data.mouse.button - 3;
                }
            }

            if (move_mouse) {
                // We need to move the mouse to the correct location prior to clicking.
                event->type = EVENT_MOUSE_MOVED;
                // TODO Remember to check the status here.
                hook_post_event(event);
                event->type = EVENT_MOUSE_PRESSED;
            }

            break;

        case EVENT_MOUSE_RELEASED:
            if (event->data.mouse.button == MOUSE_NOBUTTON) {
                logger(LOG_LEVEL_WARN, "%s [%u]: No button specified for mouse released event!\n",
                        __FUNCTION__, __LINE__);
                return UIOHOOK_FAILURE;
            } else if (event->data.mouse.button == MOUSE_BUTTON1) {
                input->mi.dwFlags = MOUSEEVENTF_LEFTUP;
            } else if (event->data.mouse.button == MOUSE_BUTTON2) {
                input->mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            } else if (event->data.mouse.button == MOUSE_BUTTON3) {
                input->mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
            } else {
                input->mi.dwFlags = MOUSEEVENTF_XUP;
                if (event->data.mouse.button == MOUSE_BUTTON4) {
                    input->mi.mouseData = XBUTTON1;
                } else if (event->data.mouse.button == MOUSE_BUTTON5) {
                    input->mi.mouseData = XBUTTON2;
                } else {
                    input->mi.mouseData = event->data.mouse.button - 3;
                }
            }

            if (move_mouse) {
                // We need to move the mouse to the correct location prior to clicking.
                event->type = EVENT_MOUSE_MOVED;
                // TODO Remember to check the status here.
                hook_post_event(event);
                event->type = EVENT_MOUSE_PRESSED;
            }

            break;

        case EVENT_MOUSE_WHEEL:
            input->mi.dwFlags = MOUSEEVENTF_WHEEL;

            // type, amount and rotation?
            input->mi.mouseData = event->data.wheel.amount * event->data.wheel.rotation;
            break;

        case EVENT_MOUSE_DRAGGED:
        case EVENT_MOUSE_MOVED:
            input->mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK;
            break;

        default:
            logger(LOG_LEVEL_DEBUG, "%s [%u]: Invalid event for mouse event mapping: %#X.\n",
                    __FUNCTION__, __LINE__, event->type);
            return UIOHOOK_FAILURE;
    }

    return UIOHOOK_SUCCESS;
}

static int do_hook_post_event(uiohook_event * const event, bool move_mouse) {
    INPUT *input = (INPUT *) calloc(1, sizeof(INPUT));
    if (input == NULL) {
        logger(LOG_LEVEL_ERROR, "%s [%u]: failed to allocate memory: calloc!\n",
               __FUNCTION__, __LINE__);
        return UIOHOOK_ERROR_OUT_OF_MEMORY;
    }

    int status = UIOHOOK_FAILURE;
    switch (event->type) {
        case EVENT_KEY_PRESSED:
        case EVENT_KEY_RELEASED:
            status = map_keyboard_event(event, input);
            break;

        case EVENT_MOUSE_PRESSED:
        case EVENT_MOUSE_RELEASED:
        case EVENT_MOUSE_WHEEL:
        case EVENT_MOUSE_MOVED:
        case EVENT_MOUSE_DRAGGED:
            status = map_mouse_event(event, input, move_mouse);
            break;

        case EVENT_KEY_TYPED:
        case EVENT_MOUSE_CLICKED:

        case EVENT_HOOK_ENABLED:
        case EVENT_HOOK_DISABLED:

        default:
            logger(LOG_LEVEL_DEBUG, "%s [%u]: Ignoring post event: %#X.\n",
                   __FUNCTION__, __LINE__, event->type);
            status = UIOHOOK_FAILURE;
    }

    if (status == UIOHOOK_SUCCESS && !SendInput(1, input, sizeof(INPUT))) {
        logger(LOG_LEVEL_ERROR, "%s [%u]: SendInput() failed! (%#lX)\n",
               __FUNCTION__, __LINE__, (unsigned long) GetLastError());
        status = UIOHOOK_FAILURE;
    }

    free(input);

    return status;
}

UIOHOOK_API int hook_post_event(uiohook_event * const event) {
    return do_hook_post_event(event, true);
}

UIOHOOK_API int hook_post_event_dont_move_mouse(uiohook_event * const event) {
    return do_hook_post_event(event, false);
}
