#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    GKEY_NONE = 0,
    GKEY_UP,
    GKEY_DOWN,
    GKEY_LEFT,
    GKEY_RIGHT,
    GKEY_PGUP,
    GKEY_PGDN,
    GKEY_OK,        /* Enter */
    GKEY_SPACE,     /* Space */
    GKEY_BACK,      /* ESC */
    GKEY_BACKSPACE, /* Backspace */
    GKEY_1,
    GKEY_2,
    GKEY_3,
    GKEY_4,
    GKEY_JUMP,      /* G */
    GKEY_THEME,     /* T */
    GKEY_RETRY,     /* R */
    GKEY_HELP,      /* H / ? */
    GKEY_CHAR       /* 其他數字或字元 */
} GameKey;

int  hal_get_width(void);
int  hal_get_height(void);
int  hal_is_xrgb(void);
int  hal_get_last_keycode(void);
uint32_t hal_get_seed(void);

void hal_clear(uint32_t rgb);
void hal_put_pixel(int x, int y, uint32_t rgb);
void hal_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void hal_draw_rect(int x, int y, int w, int h, uint32_t rgb);
void hal_fill_round_rect(int x, int y, int w, int h, int r, uint32_t rgb);
void hal_draw_round_rect(int x, int y, int w, int h, int r, uint32_t rgb);
void hal_fill_circle(int cx, int cy, int r, uint32_t rgb);
void hal_draw_circle(int cx, int cy, int r, uint32_t rgb);
void hal_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb);
void hal_fill_poly(const int *xs, const int *ys, int n, uint32_t rgb);
void hal_dim_rect(int rx, int ry, int rw, int rh);

void hal_set_text_bg(uint32_t rgb);
void hal_draw_text_utf8(int x, int y, const char *utf8_text, int max_px_w, uint32_t rgb);
void hal_draw_text_utf8_len(int x, int y, const char *utf8_text, int byte_len, int max_px_w, uint32_t rgb);
void hal_clear_text_in_rect(int rx, int ry, int rw, int rh);
void hal_present(void);

const char *hal_get_save_path(void);
void hal_request_quit(void);

#endif
