/*
 * 無敵 CD-358 (Besta RTOS / Project Muteki) 911 緊急調度員 2.5D 純按鍵平台層
 * - 完全停用觸控事件輪詢（防損壞觸控面板誤觸）
 * - 自動偵測實機 LCD 解析度 (320x240 / 480x320 / 800x480) 與像素格式 (RGB565 / XRGB8888)
 * - 內建 2.5D 掃描線多邊形填充引擎 (hal_fill_poly) + 雙緩衝單次 memcpy 刷屏，零閃爍不卡頓
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if __has_include(<muteki/common.h>)
#include <muteki/common.h>
#endif
#if __has_include(<muteki/threading.h>)
#include <muteki/threading.h>
#endif
#if __has_include(<muteki/utf16.h>)
#include <muteki/utf16.h>
#endif

#if __has_include(<muteki/draw/font.h>)
#include <muteki/draw/font.h>
#elif __has_include(<muteki/ui/font.h>)
#include <muteki/ui/font.h>
#elif __has_include(<muteki/font.h>)
#include <muteki/font.h>
#endif

#if __has_include(<muteki/draw/lcd.h>)
#include <muteki/draw/lcd.h>
#elif __has_include(<muteki/ui/lcd.h>)
#include <muteki/ui/lcd.h>
#elif __has_include(<muteki/lcd.h>)
#include <muteki/lcd.h>
#endif

#if __has_include(<muteki/draw/surface.h>)
#include <muteki/draw/surface.h>
#elif __has_include(<muteki/ui/surface.h>)
#include <muteki/ui/surface.h>
#elif __has_include(<muteki/surface.h>)
#include <muteki/surface.h>
#endif

#if __has_include(<muteki/ui/canvas.h>)
#include <muteki/ui/canvas.h>
#endif
#if __has_include(<muteki/ui/common.h>)
#include <muteki/ui/common.h>
#endif
#if __has_include(<muteki/ui.h>)
#include <muteki/ui.h>
#endif

#if __has_include(<muteki/ui/framework/event.h>)
#include <muteki/ui/framework/event.h>
#elif __has_include(<muteki/ui/event.h>)
#include <muteki/ui/event.h>
#elif __has_include(<muteki/event.h>)
#include <muteki/event.h>
#endif

#include "hal.h"
#include "game_911.h"

#define MAX_TEXT_QUEUE 96

typedef struct {
    int x;
    int y;
    int max_w;
    uint32_t fg;
    uint32_t bg;
    char text[192];
} TextCmd;

static lcd_t *g_lcd = NULL;
static int g_w = 320;
static int g_h = 240;
static int g_is_xrgb = 0;
static void *g_backbuf = NULL;
static size_t g_fb_bytes = 0;

static TextCmd g_text_q[MAX_TEXT_QUEUE];
static int g_text_q_len = 0;
static uint32_t g_text_bg = 0x0F172A;
static int g_last_keycode = 0;
static int g_running = 1;

static inline uint16_t rgb888_to_rgb565(uint32_t rgb) {
    uint16_t r = (uint16_t)((rgb >> 16) & 0xFF);
    uint16_t g = (uint16_t)((rgb >> 8) & 0xFF);
    uint16_t b = (uint16_t)(rgb & 0xFF);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint32_t dim75_rgb(uint32_t rgb) {
    return (((rgb & 0xFEFEFEu) >> 1) + ((rgb & 0xFCFCFCu) >> 2)) & 0xFFFFFFu;
}

static size_t utf8_to_utf16(const char *src, int src_len, UTF16 *dst, size_t max_dst) {
    size_t out = 0; int i = 0;
    while (i < src_len && out + 1 < max_dst) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp = 0;
        if ((c & 0x80) == 0) { cp = c; i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < src_len) {
            cp = ((c & 0x1F) << 6) | (src[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < src_len) {
            cp = ((c & 0x0F) << 12) | ((src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < src_len) {
            cp = 0x3F; i += 4;
        } else { cp = 0x3F; i += 1; }
        dst[out++] = (UTF16)cp;
    }
    dst[out] = 0;
    return out;
}

int hal_get_width(void)        { return g_w; }
int hal_get_height(void)       { return g_h; }
int hal_is_xrgb(void)          { return g_is_xrgb; }
int hal_get_last_keycode(void) { return g_last_keycode; }

uint32_t hal_get_seed(void) {
    uint32_t t = (uint32_t)time(NULL);
    return t ^ ((uint32_t)(uintptr_t)&t >> 3) ^ 0x9110358u;
}

void hal_put_pixel(int x, int y, uint32_t rgb) {
    if (!g_backbuf || x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    if (g_is_xrgb) ((uint32_t *)g_backbuf)[y * g_w + x] = rgb;
    else ((uint16_t *)g_backbuf)[y * g_w + x] = rgb888_to_rgb565(rgb);
}

void hal_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!g_backbuf || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = (x + w > g_w) ? g_w : (x + w);
    int y1 = (y + h > g_h) ? g_h : (y + h);
    int span = x1 - x0;
    if (span <= 0 || y0 >= y1) return;

    if (g_is_xrgb) {
        uint32_t *fb = (uint32_t *)g_backbuf;
        for (int py = y0; py < y1; py++) {
            uint32_t *row = fb + py * g_w + x0;
            int n = span;
            while (n >= 4) { row[0] = rgb; row[1] = rgb; row[2] = rgb; row[3] = rgb; row += 4; n -= 4; }
            while (n-- > 0) *row++ = rgb;
        }
    } else {
        uint16_t c16 = rgb888_to_rgb565(rgb);
        uint32_t c32 = ((uint32_t)c16 << 16) | (uint32_t)c16;
        uint16_t *fb = (uint16_t *)g_backbuf;
        for (int py = y0; py < y1; py++) {
            uint16_t *row = fb + py * g_w + x0;
            int n = span;
            if (((uintptr_t)row & 2u) && n > 0) { *row++ = c16; n--; }
            uint32_t *row32 = (uint32_t *)row;
            while (n >= 4) { row32[0] = c32; row32[1] = c32; row32 += 2; n -= 4; }
            row = (uint16_t *)row32;
            while (n-- > 0) *row++ = c16;
        }
    }
}

void hal_clear(uint32_t rgb) {
    g_text_q_len = 0;
    g_text_bg = rgb;
    hal_fill_rect(0, 0, g_w, g_h, rgb);
}

void hal_draw_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    hal_fill_rect(x, y, w, 1, rgb);
    hal_fill_rect(x, y + h - 1, w, 1, rgb);
    hal_fill_rect(x, y, 1, h, rgb);
    hal_fill_rect(x + w - 1, y, 1, h, rgb);
}

void hal_fill_round_rect(int x, int y, int w, int h, int r, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    if (r <= 1) { hal_fill_rect(x, y, w, h, rgb); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    hal_fill_rect(x + r, y, w - r * 2, h, rgb);
    hal_fill_rect(x, y + r, r, h - r * 2, rgb);
    hal_fill_rect(x + w - r, y + r, r, h - r * 2, rgb);
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        int cy = r - 1 - dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + cy * cy <= r2) dx++;
        int inset = r - dx;
        if (inset < 0) inset = 0;
        hal_fill_rect(x + inset, y + dy, w - inset * 2, 1, rgb);
        hal_fill_rect(x + inset, y + h - 1 - dy, w - inset * 2, 1, rgb);
    }
}

void hal_draw_round_rect(int x, int y, int w, int h, int r, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    if (r <= 1) { hal_draw_rect(x, y, w, h, rgb); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    hal_fill_rect(x + r, y, w - r * 2, 1, rgb);
    hal_fill_rect(x + r, y + h - 1, w - r * 2, 1, rgb);
    hal_fill_rect(x, y + r, 1, h - r * 2, rgb);
    hal_fill_rect(x + w - 1, y + r, 1, h - r * 2, rgb);
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        int cy = r - 1 - dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + cy * cy <= r2) dx++;
        int inset = r - dx;
        if (inset < 0) inset = 0;
        hal_put_pixel(x + inset, y + dy, rgb);
        hal_put_pixel(x + w - 1 - inset, y + dy, rgb);
        hal_put_pixel(x + inset, y + h - 1 - dy, rgb);
        hal_put_pixel(x + w - 1 - inset, y + h - 1 - dy, rgb);
    }
}

void hal_fill_circle(int cx, int cy, int r, uint32_t rgb) {
    if (r <= 0) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy; if (py < 0 || py >= g_h) continue;
        int dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= r2) dx++;
        hal_fill_rect(cx - dx, py, dx * 2 + 1, 1, rgb);
    }
}

void hal_draw_circle(int cx, int cy, int r, uint32_t rgb) {
    if (r <= 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        hal_put_pixel(cx + x, cy + y, rgb); hal_put_pixel(cx + y, cy + x, rgb);
        hal_put_pixel(cx - y, cy + x, rgb); hal_put_pixel(cx - x, cy + y, rgb);
        hal_put_pixel(cx - x, cy - y, rgb); hal_put_pixel(cx - y, cy - x, rgb);
        hal_put_pixel(cx + y, cy - x, rgb); hal_put_pixel(cx + x, cy - y, rgb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x + 1); }
    }
}

void hal_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        hal_put_pixel(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 高速整數掃描線多邊形填滿（2.5D 透視桌面、斜角螢幕、立體車體、斜條橫幅） */
void hal_fill_poly(const int *xs, const int *ys, int n, uint32_t rgb) {
    if (!xs || !ys || n < 3 || n > 16) return;
    int min_y = ys[0], max_y = ys[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }
    if (min_y < 0) min_y = 0;
    if (max_y >= g_h) max_y = g_h - 1;
    int nodes[16];
    for (int py = min_y; py <= max_y; py++) {
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int y0 = ys[i], y1 = ys[j];
            if ((y0 <= py && y1 > py) || (y1 <= py && y0 > py)) {
                int ix = xs[i] + ((py - y0) * (xs[j] - xs[i])) / (y1 - y0);
                if (cnt < 16) nodes[cnt++] = ix;
            }
        }
        for (int a = 0; a < cnt; a++) {
            for (int b = a + 1; b < cnt; b++) {
                if (nodes[b] < nodes[a]) { int t = nodes[a]; nodes[a] = nodes[b]; nodes[b] = t; }
            }
        }
        for (int a = 0; a + 1 < cnt; a += 2) {
            if (nodes[a + 1] > nodes[a]) {
                hal_fill_rect(nodes[a], py, nodes[a + 1] - nodes[a], 1, rgb);
            }
        }
    }
}

void hal_dim_rect(int rx, int ry, int rw, int rh) {
    if (!g_backbuf || rw <= 0 || rh <= 0) return;
    int x0 = rx < 0 ? 0 : rx, y0 = ry < 0 ? 0 : ry;
    int x1 = (rx + rw > g_w) ? g_w : (rx + rw);
    int y1 = (ry + rh > g_h) ? g_h : (ry + rh);
    if (x0 >= x1 || y0 >= y1) return;

    if (g_is_xrgb) {
        uint32_t *fb = (uint32_t *)g_backbuf;
        for (int py = y0; py < y1; py++) {
            uint32_t *row = fb + py * g_w + x0;
            for (int n = x1 - x0; n > 0; n--) {
                uint32_t c = *row;
                *row++ = ((c & 0xFEFEFEu) >> 1) + ((c & 0xFCFCFCu) >> 2);
            }
        }
    } else {
        uint16_t *fb = (uint16_t *)g_backbuf;
        for (int py = y0; py < y1; py++) {
            uint16_t *row = fb + py * g_w + x0;
            for (int n = x1 - x0; n > 0; n--) {
                uint16_t c = *row;
                uint16_t r = (uint16_t)((((c >> 11) & 31) * 3) >> 2);
                uint16_t g = (uint16_t)((((c >> 5)  & 63) * 3) >> 2);
                uint16_t b = (uint16_t)(((c & 31) * 3) >> 2);
                *row++ = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
    for (int i = 0; i < g_text_q_len; i++) {
        g_text_q[i].fg = dim75_rgb(g_text_q[i].fg);
        g_text_q[i].bg = dim75_rgb(g_text_q[i].bg);
    }
}

void hal_set_text_bg(uint32_t rgb) { g_text_bg = rgb; }

void hal_draw_text_utf8_len(int x, int y, const char *utf8_text, int byte_len, int max_px_w, uint32_t rgb) {
    if (!utf8_text || byte_len <= 0 || g_text_q_len >= MAX_TEXT_QUEUE) return;
    if (y < -18 || y >= g_h || x >= g_w) return;
    TextCmd *tc = &g_text_q[g_text_q_len++];
    tc->x = x; tc->y = y;
    tc->max_w = (max_px_w > 0 && x + max_px_w <= g_w) ? max_px_w : (g_w - x);
    tc->fg = rgb; tc->bg = g_text_bg;
    size_t cp = (size_t)byte_len;
    if (cp >= sizeof(tc->text)) cp = sizeof(tc->text) - 1;
    memcpy(tc->text, utf8_text, cp);
    tc->text[cp] = '\0';
}

void hal_draw_text_utf8(int x, int y, const char *utf8_text, int max_px_w, uint32_t rgb) {
    if (!utf8_text || !utf8_text[0]) return;
    hal_draw_text_utf8_len(x, y, utf8_text, (int)strlen(utf8_text), max_px_w, rgb);
}

void hal_clear_text_in_rect(int rx, int ry, int rw, int rh) {
    int out = 0;
    for (int i = 0; i < g_text_q_len; i++) {
        TextCmd *tc = &g_text_q[i];
        int overlap = (tc->y + 18 > ry) && (tc->y < ry + rh) &&
                      (tc->x < rx + rw) && (tc->x + tc->max_w > rx);
        if (!overlap) {
            if (out != i) g_text_q[out] = *tc;
            out++;
        }
    }
    g_text_q_len = out;
}

void hal_present(void) {
    if (!g_lcd || !g_lcd->surface || !g_lcd->surface->buffer || !g_backbuf) return;
    memcpy(g_lcd->surface->buffer, g_backbuf, g_fb_bytes);
    if (g_text_q_len > 0) {
        SetFontType(MONOSPACE_CJK);
        UTF16 u16[192];
        for (int i = 0; i < g_text_q_len; i++) {
            TextCmd *tc = &g_text_q[i];
            int dx = tc->x < 0 ? 0 : tc->x;
            int mw = tc->max_w;
            if (dx + mw > g_w) mw = g_w - dx;
            if (mw <= 8) continue;
            utf8_to_utf16(tc->text, (int)strlen(tc->text), u16, 192);
            rgbSetColor(tc->fg);
            rgbSetBkColor(tc->bg);
            WriteAlignString(dx, tc->y, u16, mw, STR_ALIGN_LEFT, PRINT_NONE);
        }
        g_text_q_len = 0;
    }
}

const char *hal_get_save_path(void) { return "C:\\cd358_911_v3.sav"; }
void hal_request_quit(void) { g_running = 0; }

static GameKey map_key(short code, char *out_char) {
    *out_char = (code >= 0x20 && code < 0x7F) ? (char)code : '\0';
    switch (code) {
        case 1:   return GKEY_BACK;
        case 2:   return GKEY_LEFT;
        case 3:   return GKEY_UP;
        case 4:   return GKEY_RIGHT;
        case 5:   return GKEY_DOWN;
        case 6:   return GKEY_PGUP;
        case 7:   return GKEY_PGDN;
        case 8:   return GKEY_BACKSPACE;
        case 10:
        case 13:  return GKEY_OK;
        case 32:  return GKEY_SPACE;
        case 49:  return GKEY_1;
        case 50:  return GKEY_2;
        case 51:  return GKEY_3;
        case 52:  return GKEY_4;
        case 63:
        case 72: case 104: return GKEY_HELP;
        case 71: case 103: return GKEY_JUMP;
        case 82: case 114: return GKEY_RETRY;
        case 84: case 116: return GKEY_THEME;
        default:
            if (code >= 0x20 && code < 0x7F) return GKEY_CHAR;
            return GKEY_NONE;
    }
}

int main(void) {
    g_lcd = GetActiveLCD();
    if (!g_lcd || !g_lcd->surface || !g_lcd->surface->buffer) return 1;

    g_w = (int)g_lcd->width;
    g_h = (int)g_lcd->height;
    g_is_xrgb = (g_lcd->surface->depth == LCD_SURFACE_PIXFMT_XRGB);
    g_fb_bytes = (size_t)g_w * (size_t)g_h * (g_is_xrgb ? 4u : 2u);
    g_backbuf = malloc(g_fb_bytes);
    if (!g_backbuf) return 1;

    game_init();
    game_render();

    ui_event_t uievent = {0};
    int anim_div = 0;

    while (g_running) {
        int handled = 0;
        /* 純鍵盤事件輪詢，完全不讀觸控 */
        while (TestKeyEvent(&uievent) && GetEvent(&uievent)) {
            if (uievent.event_type == UI_EVENT_TYPE_KEY) {
                g_last_keycode = (int)uievent.key_code0;
                char ch = 0;
                GameKey k = map_key(uievent.key_code0, &ch);
                if (k != GKEY_NONE) {
                    game_on_key(k, ch);
                    handled = 1;
                }
            }
            ClearEvent(&uievent);
        }

        if (handled && g_running) {
            game_render();
            anim_div = 0;
        } else if (game_needs_anim()) {
            OSSleep(35);
            anim_div++;
            if (anim_div >= 2) {
                anim_div = 0;
                game_tick_anim();
                game_render();
            }
        } else {
            OSSleep(35);
        }
    }

    game_shutdown();
    free(g_backbuf);
    return 0;
}
