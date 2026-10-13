#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"

#include <stdlib.h>
#include <stdint.h>

#include <fcntl.h>
#include <keyboard.h>
#include <syscalls.h>
#include <hpet.h>
#include <display.h>
#include <string.h>
#include <stdio.h>

// the crime-scene window library, this port only runs as a window on the desktop
#include <crime/crime.h>
#include <crime/sys.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define KEY_QUEUE_SIZE 64

static cs_window_t *win;
static int hpet_fd = -2;

static keyboard_event_t key_queue[KEY_QUEUE_SIZE];
static int key_head = 0;
static int key_count = 0;

static int mouse_locked = 0;
static int mouse_dx = 0;
static int mouse_dy = 0;
static int mouse_buttons = 0;
static int mouse_pending = 0;

static uint64_t start_fs = 0;

static uint64_t dg_now_fs(void)
{
    uint64_t now = 0;

    if (hpet_fd == -2)
        hpet_fd = _open("/dev/hpet", O_RDONLY);
    if (hpet_fd < 0)
        return 0;

    if (_ioctl(hpet_fd, HPET_IOCTL_GET_FEMTOSECONDS, &now) == 0)
        return now;
    if (_read(hpet_fd, &now, sizeof(now)) == (long)sizeof(now))
        return now;
    return 0;
}

static unsigned char map_key(const keyboard_event_t* ev)
{
    switch (ev->keycode)
    {
        case Escape:         return KEY_ESCAPE;
        case CarriageReturn: return KEY_ENTER;
        case LineFeed:       return KEY_ENTER;
        case FileSeparator:  return KEY_ENTER;
        case GroupSeparator: return KEY_FIRE;
        case HorizontalTab:  return KEY_TAB;
        case Backspace:      return KEY_BACKSPACE;
        case ArrowLeft:      return KEY_LEFTARROW;
        case ArrowRight:     return KEY_RIGHTARROW;
        case ArrowUp:        return KEY_UPARROW;
        case ArrowDown:      return KEY_DOWNARROW;
        case Home:           return KEY_HOME;
        case End:            return KEY_END;
        case Insert:         return KEY_INS;
        case Delete:         return KEY_DEL;
        case PageUp:         return KEY_PGUP;
        case PageDown:       return KEY_PGDN;
        case F1:             return KEY_F1;
        case F2:             return KEY_F2;
        case F3:             return KEY_F3;
        case F4:             return KEY_F4;
        case F5:             return KEY_F5;
        case F6:             return KEY_F6;
        case F7:             return KEY_F7;
        case F8:             return KEY_F8;
        case F9:             return KEY_F9;
        case F10:            return KEY_F10;
        case F11:            return KEY_F11;
        case F12:            return KEY_F12;
        case 42:             return KEY_RSHIFT;
        case 54:             return KEY_RSHIFT;
        default:             break;
    }
    char ascii = tty_key_to_ascii(ev);
    if (ascii == '\n' || ascii == '\r') return KEY_ENTER;
    if (ascii >= 'A' && ascii <= 'Z') ascii = (char)(ascii + ('a' - 'A'));
    return (unsigned char)ascii;
}

// take everything the desktop sent since last time. keys and mouse are kept until the game asks
static void dg_pump_events(void)
{
    cs_event_t ev;

    while (cs_event_wait(&ev, 0)) {
        switch (ev.type)
        {
            case CS_EV_KEY:
                if (key_count < KEY_QUEUE_SIZE) {
                    keyboard_event_t *slot = &key_queue[(key_head + key_count++) % KEY_QUEUE_SIZE];
                    slot->keycode = ev.keycode;
                    slot->action  = ev.key_action;
                    slot->keymod  = ev.keymod;
                }
                break;

            // with the pointer locked the desktop sends how far the mouse moved, thats what turns
            case CS_EV_MOUSE_RELATIVE:
                mouse_dx += ev.x;
                mouse_dy += ev.y;
                mouse_buttons = (int)ev.buttons;
                mouse_pending = 1;
                break;

            // without it theres only where the pointer is, which is no use for looking around.
            // the buttons still count
            case CS_EV_MOUSE:
                mouse_buttons = (int)ev.buttons;
                mouse_pending = 1;
                break;

            case CS_EV_CLOSE_REQUEST:
                cs_window_close(win);
                exit(0);
                break;

            // a resize needs nothing doing here, win is already the new size and the next frame fills it
            default:
                break;
        }
    }
}

void DG_Init(void)
{
    win = cs_window_open(DOOMGENERIC_RESX, DOOMGENERIC_RESY, "DOOM");
    if (!win) {
        printf("CrimeScene Display Server is not running\n");
        exit(1);
    }

    start_fs = dg_now_fs();
}

// the game has the mouse while its being played and gives it back for the menu, a pause
// or a demo, the way a windows game does. alt tab takes it away whatever we want
static void dg_update_mouse_lock(void)
{
    int want = (gamestate == GS_LEVEL && !menuactive && !paused && !demoplayback);
    if (want == mouse_locked)
        return;

    mouse_locked = want;
    cs_pointer_lock(win, want);
}

void DG_DrawFrame(void)
{
    if (!win || !DG_ScreenBuffer) return;

    dg_pump_events();
    dg_update_mouse_lock();

    // stretched to whatever size the window is right now
    uint32_t rw = (uint32_t)win->width;
    uint32_t rh = (uint32_t)win->height;

    static uint32_t* x_map = NULL;
    static uint32_t last_rw = 0;
    
    if (rw != last_rw) {
        if (x_map) free(x_map);
        x_map = (uint32_t*)malloc(rw * sizeof(uint32_t));
        uint32_t x_ratio = (DOOMGENERIC_RESX << 16) / rw;
        for (uint32_t x = 0; x < rw; x++) {
            x_map[x] = (x * x_ratio) >> 16;
        }
        last_rw = rw;
    }

    uint32_t y_ratio = (DOOMGENERIC_RESY << 16) / rh;
    uint32_t src_y_fp = 0;

    for (uint32_t y = 0; y < rh; y++) {
        uint32_t* dst = &win->pixels[y * (uint32_t)win->pitch];
        const pixel_t* src_row = &DG_ScreenBuffer[(src_y_fp >> 16) * DOOMGENERIC_RESX];
        src_y_fp += y_ratio;

        // same size is the usual case, no point going through the map for it
        if (rw == DOOMGENERIC_RESX) {
            memcpy(dst, src_row, rw * sizeof(uint32_t));
            continue;
        }

        for (uint32_t x = 0; x < rw; x++) {
            dst[x] = src_row[x_map[x]];
        }
    }

    cs_window_commit(win, 0, 0, win->width, win->height);
}

// actually asleep, not spinning. on a desktop the cpu has other windows to draw
void DG_SleepMs(uint32_t ms) {
    poll(NULL, 0, (int)ms);
}

uint32_t DG_GetTicksMs(void) {
    static uint32_t fallback_ms = 0;
    uint64_t now_fs = dg_now_fs();

    if (!now_fs)
        return fallback_ms++;
    if (!start_fs || now_fs < start_fs)
        start_fs = now_fs;

    return (uint32_t)((now_fs - start_fs) / femtosecondsPerMillisecond);
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    dg_pump_events();

    while (key_count > 0) {
        keyboard_event_t key_event = key_queue[key_head];
        key_head = (key_head + 1) % KEY_QUEUE_SIZE;
        key_count--;

        unsigned char key = map_key(&key_event);
        if (key == 0) continue;
        *pressed = (key_event.action != KEY_RELEASE);
        *doomKey = key;
        return 1;
    }
    return 0;
}

int DG_GetMouse(int *buttons, int *dx, int *dy) {
    int doom_buttons = 0;

    if (!buttons || !dx || !dy)
        return 0;

    dg_pump_events();
    if (!mouse_pending)
        return 0;

    if (mouse_buttons & CS_BUTTON_LEFT) doom_buttons |= 1;
    if (mouse_buttons & CS_BUTTON_RIGHT) doom_buttons |= 2;
    if (mouse_buttons & CS_BUTTON_MIDDLE) doom_buttons |= 4;

    *buttons = doom_buttons;
    *dx = mouse_dx;
    *dy = mouse_dy;

    mouse_dx = 0;
    mouse_dy = 0;
    mouse_pending = 0;

    return 1;
}

void DG_SetWindowTitle(const char* title) { (void)title; }