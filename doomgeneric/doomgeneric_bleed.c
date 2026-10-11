#include "doomgeneric.h"
#include "doomkeys.h"

#include <stdlib.h>
#include <stdint.h>

#include <fcntl.h>
#include <keyboard.h>
#include <syscalls.h>
#include <hpet.h>
#include <display.h>
#include <string.h>
#include <stdio.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define TTY_ECHO         (1 << 1)
#define TTY_CANNONICAL   (1 << 2)
#define TTY_NONBLOCK     (1 << 4)

#define TTY_IOCTL_SET_FLAGS  0x5402
#define TTY_FLAGS_DOOM (TTY_NONBLOCK)

#ifndef DG_HAS_MOUSE_HEADER
typedef struct {
    int16_t dx;
    int16_t dy;
    int8_t wheel;
    uint8_t buttons;
} mouse_event_t;
#endif

#ifndef MOUSE_BTN_LEFT
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)
#endif

static uint32_t* fb0;
static uint32_t fb_pitch_pixels;
static struct fb_info fb_info_local;
static int tty_fd = -1;
static int mouse_fd = -1;
static int hpet_fd = -2;
static int fb_fd = -1;

static keyboard_event_t key_event;
static uint64_t start_fs = 0;
static int mouse_debug_dx = 0;
static int mouse_debug_dy = 0;
static int mouse_debug_buttons = 0;
static uint64_t mouse_debug_last_fs = 0;

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

//crude font

enum {
    FONT_0 = 0,
    FONT_1,
    FONT_2,
    FONT_3,
    FONT_4,
    FONT_5,
    FONT_6,
    FONT_7,
    FONT_8,
    FONT_9,
    FONT_F,
    FONT_P,
    FONT_S,
    FONT_COLON,
    FONT_DOT,
    FONT_MINUS,
    FONT_M,
    FONT_X,
    FONT_Y,
    FONT_B,
    FONT_GLYPH_COUNT
};

static const uint8_t font8x8[FONT_GLYPH_COUNT][8] = {
    {0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C},
    {0x08, 0x18, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x3C, 0x42, 0x02, 0x02, 0x3C, 0x40, 0x40, 0x7E},
    {0x3C, 0x42, 0x02, 0x1C, 0x02, 0x02, 0x42, 0x3C},
    {0x04, 0x0C, 0x14, 0x24, 0x44, 0x7E, 0x04, 0x04},
    {0x7E, 0x40, 0x40, 0x7C, 0x02, 0x02, 0x42, 0x3C},
    {0x3C, 0x40, 0x40, 0x7C, 0x42, 0x42, 0x42, 0x3C},
    {0x7E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x20, 0x20},
    {0x3C, 0x42, 0x42, 0x3C, 0x42, 0x42, 0x42, 0x3C},
    {0x3C, 0x42, 0x42, 0x3E, 0x02, 0x02, 0x42, 0x3C},
    {0x7E, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x40},
    {0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x40, 0x40},
    {0x3E, 0x40, 0x40, 0x3C, 0x02, 0x02, 0x02, 0x7C},
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00},
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},
    {0x42, 0x66, 0x5A, 0x42, 0x42, 0x42, 0x42, 0x42},
    {0x42, 0x24, 0x18, 0x18, 0x18, 0x18, 0x24, 0x42},
    {0x42, 0x24, 0x18, 0x18, 0x08, 0x08, 0x08, 0x08},
    {0x7C, 0x42, 0x42, 0x7C, 0x42, 0x42, 0x42, 0x7C}
};

static int front_glyph_for_char(char ch) {
    if (ch >= '0' && ch <= '9') return FONT_0 + (ch - '0');
    if (ch == 'F') return FONT_F;
    if (ch == 'P') return FONT_P;
    if (ch == 'S') return FONT_S;
    if (ch == ':') return FONT_COLON;
    if (ch == '.') return FONT_DOT;
    if (ch == '-') return FONT_MINUS;
    if (ch == 'M') return FONT_M;
    if (ch == 'X') return FONT_X;
    if (ch == 'Y') return FONT_Y;
    if (ch == 'B') return FONT_B;
    return -1;
}

static void draw_front_glyph(int x, int y, int glyph, uint32_t color) {
    if (glyph < 0 || glyph >= FONT_GLYPH_COUNT)
        return;

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            if (font8x8[glyph][i] & (1 << (7 - j))) {
                int tx = x + j;
                int ty = y + i;
                if (tx < DOOMGENERIC_RESX && ty < DOOMGENERIC_RESY) {
                    DG_ScreenBuffer[ty * DOOMGENERIC_RESX + tx] = (pixel_t)color;
                }
            }
        }
    }
}

static void draw_front_text(int x, int y, const char *text, uint32_t color) {
    int cx = x;
    while (*text) {
        int glyph = front_glyph_for_char(*text++);
        if (glyph >= 0) {
            draw_front_glyph(cx, y, glyph, color);
        }
        cx += 8;
    }
}

static int draw_front_unsigned(int x, int y, uint32_t value, uint32_t color) {
    char digits[10];
    int count = 0;

    if (value == 0) {
        draw_front_glyph(x, y, FONT_0, color);
        return 8;
    }

    while (value > 0 && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    for (int i = count - 1; i >= 0; i--) {
        draw_front_glyph(x, y, FONT_0 + (digits[i] - '0'), color);
        x += 8;
    }

    return count * 8;
}

static int draw_front_signed(int x, int y, int value, uint32_t color) {
    int width = 0;

    if (value < 0) {
        draw_front_glyph(x, y, FONT_MINUS, color);
        x += 8;
        width += 8;
        value = -value;
    }

    width += draw_front_unsigned(x, y, (uint32_t)value, color);
    return width;
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

void DG_Init(void)
{
    uint32_t flags = TTY_FLAGS_DOOM;
    tty_fd = _open("/dev/tty0", O_RDWR);
    if (tty_fd >= 0) _ioctl(tty_fd, TTY_IOCTL_SET_FLAGS, &flags);
    _ioctl(0, TTY_IOCTL_SET_FLAGS, &flags);
    mouse_fd = _open("/dev/mouse", O_RDONLY);

    fb_fd = _open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { fb0 = NULL; return; }
    if (_ioctl(fb_fd, FB_IOC_GET_INFO, &fb_info_local) < 0) { fb0 = NULL; return; }

    fb0 = (uint32_t*)malloc(fb_info_local.height * fb_info_local.pitch);
    fb_pitch_pixels = fb_info_local.pitch >> 2;
    start_fs = dg_now_fs();
    
    // Clear screen once to black
    memset(fb0, 0, fb_info_local.height * fb_info_local.pitch);
}

void DG_DrawFrame(void)
{
    if (!fb0 || !DG_ScreenBuffer) return;

    // --- Rolling Average FPS ---
    #define FPS_AVG_WINDOW 60
    static uint64_t delta_buffer[FPS_AVG_WINDOW] = {0};
    static int delta_idx = 0;
    static uint64_t rolling_sum = 0;
    static int sample_count = 0;
    static uint64_t last_frame_fs = 0;
    uint64_t current_fs_now = dg_now_fs();
    uint32_t fps_scaled = 0;

    if (last_frame_fs != 0) {
        uint64_t delta = current_fs_now - last_frame_fs;
        rolling_sum -= delta_buffer[delta_idx];
        delta_buffer[delta_idx] = delta;
        rolling_sum += delta;
        delta_idx = (delta_idx + 1) % FPS_AVG_WINDOW;
        if (sample_count < FPS_AVG_WINDOW) sample_count++;
        if (rolling_sum > 0) {
            uint64_t avg_delta = rolling_sum / sample_count;
            if (avg_delta > 0)
                fps_scaled = (uint32_t)((femtosecondsPerSecond * 10) / avg_delta);
        }
    }
    last_frame_fs = current_fs_now;

    // UI Drawing
    uint32_t fps_int = fps_scaled / 10;
    uint32_t color = 0xFFFFFF;
    draw_front_glyph(5, 5, FONT_F, color);
    draw_front_glyph(13, 5, FONT_P, color);
    draw_front_glyph(21, 5, FONT_S, color);
    color = (fps_int < 20) ? 0xCC0000 : (fps_int < 40 ? 0xCCCC00 : 0x00CC00);
    draw_front_glyph(45, 5, FONT_0 + ((fps_int / 10) % 10), color);
    draw_front_glyph(53, 5, FONT_0 + (fps_int % 10), color);

    // mouse debug, i wont need this forever but itll sit here for now, maybe use a preprocessor to disable it?
    uint32_t mouse_color = 0x666666;
    if (mouse_debug_last_fs != 0 && (current_fs_now - mouse_debug_last_fs) < (femtosecondsPerSecond / 4))
        mouse_color = 0x55CCFF;

    int cx = 5;
    draw_front_text(cx, 17, "M", mouse_color);
    cx += 8;
    draw_front_text(cx, 17, "B:", mouse_color);
    cx += 16;
    cx += draw_front_unsigned(cx, 17, (uint32_t)(mouse_debug_buttons & 0x7), mouse_color);
    cx += 8;
    draw_front_text(cx, 17, "X:", mouse_color);
    cx += 16;
    cx += draw_front_signed(cx, 17, mouse_debug_dx, mouse_color);
    cx += 8;
    draw_front_text(cx, 17, "Y:", mouse_color);
    cx += 16;
    draw_front_signed(cx, 17, mouse_debug_dy, mouse_color);

    uint32_t sw = fb_info_local.width;
    uint32_t sh = fb_info_local.height;
    uint32_t rw = (sw > 1280) ? 1280 : sw;
    uint32_t rh = (sh > 800) ? 800 : sh;
    uint32_t ox = (sw > rw) ? (sw - rw) / 2 : 0;
    uint32_t oy = (sh > rh) ? (sh - rh) / 2 : 0;

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

    static uint32_t* line_buf = NULL;
    static uint32_t last_sw = 0;
    if (sw != last_sw) {
        if (line_buf) free(line_buf);
        line_buf = (uint32_t*)malloc(sw * sizeof(uint32_t));
        last_sw = sw;
    }

    for (uint32_t y = 0; y < rh; y++) {
        uint32_t* dst = &fb0[(y + oy) * fb_pitch_pixels];
        const pixel_t* src_row = &DG_ScreenBuffer[(src_y_fp >> 16) * DOOMGENERIC_RESX];
        src_y_fp += y_ratio;

        for (uint32_t x = 0; x < rw; x++) {
            line_buf[x + ox] = src_row[x_map[x]];
        }
        
        memcpy(dst + ox, line_buf + ox, rw * sizeof(uint32_t));
    }

    if (fb_fd >= 0) {
        _ioctl(fb_fd, FB_IOC_FLIP, fb0);
    }
}

void DG_SleepMs(uint32_t ms) {
    uint64_t start = dg_now_fs();
    if (!start)
        return;

    uint64_t target = start + (uint64_t)ms * femtosecondsPerMillisecond;
    while (dg_now_fs() < target) {
        for (volatile int i = 0; i < 500; i++);
    }
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
    if (_read(0, &key_event, sizeof(key_event)) != (int)sizeof(key_event)) return 0;
    unsigned char key = map_key(&key_event);
    if (key == 0) return 0;
    *pressed = (key_event.action != KEY_RELEASE);
    *doomKey = key;
    return 1;
}

int DG_GetMouse(int *buttons, int *dx, int *dy) {
    mouse_event_t mouse_event;
    int doom_buttons = 0;

    if (!buttons || !dx || !dy || mouse_fd < 0)
        return 0;

    if (_read(mouse_fd, &mouse_event, sizeof(mouse_event)) != (int)sizeof(mouse_event))
        return 0;

    if (mouse_event.buttons & MOUSE_BTN_LEFT) doom_buttons |= 1;
    if (mouse_event.buttons & MOUSE_BTN_RIGHT) doom_buttons |= 2;
    if (mouse_event.buttons & MOUSE_BTN_MIDDLE) doom_buttons |= 4;

    *buttons = doom_buttons;
    *dx = (int)mouse_event.dx;
    *dy = (int)mouse_event.dy;

    mouse_debug_buttons = doom_buttons;
    mouse_debug_dx = *dx;
    mouse_debug_dy = *dy;
    mouse_debug_last_fs = dg_now_fs();

    return 1;
}

void DG_SetWindowTitle(const char* title) { (void)title; }