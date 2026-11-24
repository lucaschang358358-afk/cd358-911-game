/*
 * 911 緊急調度員 2.5D v3 — CD-358 專屬版核心 (1,000 關 · 真隨機選項 · 純按鍵)
 * - 全整數 2.5D 透視繪圖（無浮點數瓶頸，在 ARM7/ARM9 上完全不卡）
 * - 自動適配 320x240 / 480x320 / 800x480 螢幕
 * - 內建 1,000 關分塊壓縮劇本，每輪 [1]/[2] 位置真隨機洗牌
 */

#include "game_911.h"
#include "cases_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SCR_STANDBY = 0,
    SCR_CALL,
    SCR_DISPATCH,
    SCR_RESULT,
    SCR_JUMP,
    SCR_HELP,
    SCR_QUIT
} GameScreen;

typedef struct {
    int who; /* 0=Dispatcher, 1=Caller, 2=System Warning */
    int is_bad;
    char text[160];
} ChatMsg;

#define MAX_CHAT_LOG 10

static GameScreen g_screen = SCR_STANDBY;
static GameScreen g_ret_screen = SCR_STANDBY;

/* 玩家存檔資料 */
static int g_level = 0;      /* 0..TOTAL_CASES-1 */
static int g_wins = 0;
static int g_stars = 0;
static int g_attempts = 0;
static int g_night = 0;

/* 關卡執行狀態 */
static Case911 g_case;
static int g_case_ok = 0;
static int g_round = 0;       /* 0=Q1, 1=Q2 */
static int g_sel = 0;
static int g_swap[2] = {0, 0};/* 1 表示 [1]=壞選項, [2]=好選項 */
static int g_bad_cnt = 0;
static int g_chosen_unit = 0;
static int g_is_win = 0;
static int g_earned_stars = 0;
static char g_fail_reason[160] = {0};

static ChatMsg g_chat[MAX_CHAT_LOG];
static int g_chat_len = 0;

/* 動畫與彈窗狀態 */
static unsigned g_anim_tick = 0;
static uint32_t g_rng_state = 0x12345678u;
static int g_jump_target = 0;
static char g_jump_digits[8] = {0};

/* 單位配色與名稱 */
static const char *UNIT_NAME[4]  = { "警車", "救護車", "消防車", "掛斷" };
static const uint32_t UNIT_COL[4]= { 0x2563EB, 0x059669, 0xEA580C, 0x475569 };
static const uint32_t UNIT_DK[4] = { 0x1D4ED8, 0x047857, 0xC2410C, 0x334155 };

/* 簡易正弦/餘弦查表 (16 方向, *100) 用於 2.5D 雷達掃描線 */
static const int COS16[16] = { 100,  92,  71,  38,   0, -38, -71, -92, -100, -92, -71, -38,   0,  38,  71,  92 };
static const int SIN16[16] = {   0,  38,  71,  92, 100,  92,  71,  38,    0, -38, -71, -92, -100, -92, -71, -38 };

static uint32_t rng_next(void) {
    uint32_t x = g_rng_state;
    if (x == 0) x = 0x9110358u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

static const char *get_rank_name(int wins) {
    if (wins >= 600) return "傳奇總局長";
    if (wins >= 300) return "911 副局長";
    if (wins >= 150) return "區域督察";
    if (wins >= 70)  return "資深指揮官";
    if (wins >= 30)  return "值班組長";
    if (wins >= 10)  return "初級調度員";
    return "實習調度員";
}

/* =========================================================================
 * 存檔讀寫
 * ========================================================================= */

static void save_progress(void) {
    const char *sp = hal_get_save_path();
    if (!sp || !sp[0]) return;
    FILE *f = fopen(sp, "wb");
    if (!f) return;
    fprintf(f, "911V3|%d|%d|%d|%d|%d\n", g_level, g_wins, g_stars, g_attempts, g_night);
    fclose(f);
}

static void load_progress(void) {
    const char *sp = hal_get_save_path();
    if (!sp || !sp[0]) return;
    FILE *f = fopen(sp, "rb");
    if (!f) return;
    int lv = 0, w = 0, st = 0, att = 0, ng = 0;
    if (fscanf(f, "911V3|%d|%d|%d|%d|%d", &lv, &w, &st, &att, &ng) == 5) {
        if (lv >= 0 && lv < TOTAL_CASES) g_level = lv;
        if (w >= 0) g_wins = w;
        if (st >= 0) g_stars = st;
        if (att >= 0) g_attempts = att;
        g_night = ng ? 1 : 0;
    }
    fclose(f);
}

/* =========================================================================
 * UTF-8 自動換行與截斷工具
 * ========================================================================= */

static int utf8_char_u(const unsigned char *s, size_t rem, int *out_u) {
    unsigned char c = s[0];
    if ((c & 0x80) == 0) { if (out_u) *out_u = 1; return 1; }
    if ((c & 0xE0) == 0xC0 && rem >= 2) { if (out_u) *out_u = 2; return 2; }
    if ((c & 0xF0) == 0xE0 && rem >= 3) { if (out_u) *out_u = 2; return 3; }
    if ((c & 0xF8) == 0xF0 && rem >= 4) { if (out_u) *out_u = 2; return 4; }
    if (out_u) *out_u = 1;
    return 1;
}

static int utf8_total_units(const char *s) {
    if (!s) return 0;
    size_t len = strlen(s), pos = 0;
    int u = 0;
    while (pos < len) {
        int cu = 0, cl = utf8_char_u((const unsigned char *)(s + pos), len - pos, &cu);
        u += cu;
        pos += (size_t)cl;
    }
    return u;
}

/* 將 UTF-8 字串依 max_u (半形字寬單位, 每單位約 8px) 拆成最多 max_lines 行 */
static int wrap_utf8(const char *src, int max_u, int max_lines,
                     int *starts, int *lens) {
    if (!src || max_u < 4 || max_lines <= 0) return 0;
    size_t slen = strlen(src), pos = 0;
    int ln = 0;
    while (pos < slen && ln < max_lines) {
        size_t s0 = pos;
        int u = 0;
        while (pos < slen) {
            if (src[pos] == '\n') { pos++; break; }
            int cu = 0, cl = utf8_char_u((const unsigned char *)(src + pos), slen - pos, &cu);
            if (u + cu > max_u && pos > s0) break;
            u += cu;
            pos += (size_t)cl;
        }
        starts[ln] = (int)s0;
        lens[ln] = (int)(pos - s0);
        if (lens[ln] > 0 && src[s0 + (size_t)lens[ln] - 1] == '\n') lens[ln]--;
        ln++;
    }
    return ln;
}

static void draw_centered_text(int cx, int y, const char *s, int max_w, uint32_t col) {
    int px = utf8_total_units(s) * 8;
    if (px > max_w) px = max_w;
    int x = cx - px / 2;
    if (x < 2) x = 2;
    hal_draw_text_utf8(x, y, s, max_w, col);
}

/* =========================================================================
 * 遊戲流程控制
 * ========================================================================= */

static void chat_push(int who, int is_bad, const char *txt) {
    if (!txt) return;
    if (g_chat_len >= MAX_CHAT_LOG) {
        for (int i = 0; i + 1 < MAX_CHAT_LOG; i++) g_chat[i] = g_chat[i + 1];
        g_chat_len = MAX_CHAT_LOG - 1;
    }
    ChatMsg *m = &g_chat[g_chat_len++];
    m->who = who;
    m->is_bad = is_bad;
    snprintf(m->text, sizeof(m->text), "%s", txt);
}

static void load_current_case(void) {
    g_case_ok = cases_load(g_level, &g_case);
}

static void start_call(void) {
    load_current_case();
    if (!g_case_ok) return;
    g_screen = SCR_CALL;
    g_round = 0;
    g_sel = 0;
    g_bad_cnt = 0;
    /* 每一輪獨立 50/50 隨機打亂 [1] 與 [2] 選項順序！ */
    g_swap[0] = (int)(rng_next() & 1u);
    g_swap[1] = (int)(rng_next() & 1u);
    g_chat_len = 0;
    chat_push(0, 0, "911，請問你有什麼緊急狀況？");
    chat_push(1, 0, g_case.open_msg);
}

static void get_round_option(int r, int btn_idx, const char **out_q, const char **out_r, int *out_good) {
    const char *gq = (r == 0) ? g_case.q1_good : g_case.q2_good;
    const char *bq = (r == 0) ? g_case.q1_bad  : g_case.q2_bad;
    const char *gr = (r == 0) ? g_case.r1_good : g_case.r2_good;
    const char *br = (r == 0) ? g_case.r1_bad  : g_case.r2_bad;
    int pick_good = g_swap[r] ? (btn_idx == 1) : (btn_idx == 0);
    if (out_q) *out_q = pick_good ? gq : bq;
    if (out_r) *out_r = pick_good ? gr : br;
    if (out_good) *out_good = pick_good;
}

static void choose_dialogue(int btn_idx) {
    const char *q = NULL, *r = NULL;
    int is_good = 0;
    get_round_option(g_round, btn_idx, &q, &r, &is_good);
    chat_push(0, !is_good, q);
    chat_push(1, 0, r);
    if (!is_good) {
        g_bad_cnt++;
        chat_push(2, 1, "! 應對不專業，評分 -1星");
    }
    if (g_round == 0) {
        g_round = 1;
        g_sel = 0;
    } else {
        if (g_bad_cnt >= 2) {
            g_attempts++;
            g_is_win = 0;
            g_earned_stars = 0;
            g_chosen_unit = 3;
            snprintf(g_fail_reason, sizeof(g_fail_reason),
                     "連續兩次錯誤指示，錯失黃金救援時機！");
            g_anim_tick = 0;
            g_screen = SCR_RESULT;
            save_progress();
        } else {
            g_round = 2;
            g_sel = 0;
            g_screen = SCR_DISPATCH;
        }
    }
}

static void choose_dispatch(int unit_idx) {
    g_chosen_unit = unit_idx;
    g_attempts++;
    g_is_win = (unit_idx == g_case.ans);
    if (g_is_win) {
        g_earned_stars = 3 - g_bad_cnt;
        if (g_earned_stars < 1) g_earned_stars = 1;
        g_wins++;
        g_stars += g_earned_stars;
        g_fail_reason[0] = '\0';
    } else {
        g_earned_stars = 0;
        snprintf(g_fail_reason, sizeof(g_fail_reason), "%s", g_case.lose_msg);
    }
    g_anim_tick = 0;
    g_screen = SCR_RESULT;
    save_progress();
}

static void next_after_result(void) {
    if (g_is_win) {
        g_level = (g_level + 1) % TOTAL_CASES;
        save_progress();
    }
    load_current_case();
    g_screen = SCR_STANDBY;
    g_anim_tick = 0;
}

void game_init(void) {
    g_rng_state = hal_get_seed();
    load_progress();
    load_current_case();
    g_screen = SCR_STANDBY;
    g_anim_tick = 0;
}

void game_shutdown(void) {
    save_progress();
}

int game_needs_anim(void) {
    return (g_screen == SCR_STANDBY || g_screen == SCR_RESULT);
}

void game_tick_anim(void) {
    g_anim_tick++;
}

/* =========================================================================
 * 純按鍵事件處理
 * ========================================================================= */

void game_on_key(GameKey key, char raw_char) {
    int is_ok = (key == GKEY_OK || key == GKEY_SPACE);
    /* 混入按鍵時序增加隨機熵，確保選項分布完全不可預測 */
    g_rng_state ^= (g_anim_tick * 2654435761u) ^ ((uint32_t)key << 11) ^ (uint32_t)(unsigned char)raw_char;
    (void)rng_next();

    /* 離開確認 */
    if (g_screen == SCR_QUIT) {
        if (is_ok) hal_request_quit();
        else if (key == GKEY_BACK || key == GKEY_BACKSPACE) g_screen = SCR_STANDBY;
        return;
    }

    /* 說明頁 */
    if (g_screen == SCR_HELP) {
        if (key == GKEY_BACK || key == GKEY_HELP || is_ok || key == GKEY_BACKSPACE) {
            g_screen = g_ret_screen;
        }
        return;
    }

    /* 跳關彈窗 (支援直接打數字 1~1000 或方向鍵) */
    if (g_screen == SCR_JUMP) {
        if (key == GKEY_BACK || key == GKEY_JUMP) { g_screen = g_ret_screen; return; }
        if (raw_char >= '0' && raw_char <= '9') {
            size_t l = strlen(g_jump_digits);
            if (l + 1 < sizeof(g_jump_digits)) {
                g_jump_digits[l] = raw_char;
                g_jump_digits[l + 1] = '\0';
                int v = atoi(g_jump_digits) - 1;
                if (v < 0) v = 0;
                if (v >= TOTAL_CASES) v = TOTAL_CASES - 1;
                g_jump_target = v;
            }
            return;
        }
        if (key == GKEY_BACKSPACE) {
            size_t l = strlen(g_jump_digits);
            if (l > 0) {
                g_jump_digits[l - 1] = '\0';
                if (g_jump_digits[0]) {
                    int v = atoi(g_jump_digits) - 1;
                    if (v < 0) v = 0;
                    if (v >= TOTAL_CASES) v = TOTAL_CASES - 1;
                    g_jump_target = v;
                }
            }
            return;
        }
        if (key == GKEY_LEFT)       { g_jump_digits[0] = '\0'; g_jump_target -= 10; }
        else if (key == GKEY_RIGHT) { g_jump_digits[0] = '\0'; g_jump_target += 10; }
        else if (key == GKEY_UP)    { g_jump_digits[0] = '\0'; g_jump_target -= 50; }
        else if (key == GKEY_DOWN)  { g_jump_digits[0] = '\0'; g_jump_target += 50; }
        else if (key == GKEY_PGUP)  { g_jump_digits[0] = '\0'; g_jump_target -= 1; }
        else if (key == GKEY_PGDN)  { g_jump_digits[0] = '\0'; g_jump_target += 1; }
        else if (is_ok) {
            if (g_jump_target < 0) g_jump_target = 0;
            if (g_jump_target >= TOTAL_CASES) g_jump_target = TOTAL_CASES - 1;
            g_level = g_jump_target;
            save_progress();
            load_current_case();
            g_screen = SCR_STANDBY;
            return;
        }
        if (g_jump_target < 0) g_jump_target = 0;
        if (g_jump_target >= TOTAL_CASES) g_jump_target = TOTAL_CASES - 1;
        return;
    }

    /* 全域快捷鍵 */
    if (key == GKEY_THEME) { g_night = !g_night; save_progress(); return; }
    if (key == GKEY_HELP)  { g_ret_screen = g_screen; g_screen = SCR_HELP; return; }
    if (key == GKEY_JUMP)  {
        g_ret_screen = g_screen;
        g_jump_target = g_level;
        g_jump_digits[0] = '\0';
        g_screen = SCR_JUMP;
        return;
    }
    if (key == GKEY_RETRY && g_screen != SCR_STANDBY) { start_call(); return; }

    /* 各主畫面操作 */
    if (g_screen == SCR_STANDBY) {
        if (is_ok) start_call();
        else if (key == GKEY_BACK) g_screen = SCR_QUIT;
        return;
    }
    if (g_screen == SCR_CALL) {
        if (key == GKEY_BACK || key == GKEY_BACKSPACE) { g_screen = SCR_STANDBY; return; }
        if (key == GKEY_UP || key == GKEY_LEFT) g_sel = 0;
        else if (key == GKEY_DOWN || key == GKEY_RIGHT) g_sel = 1;
        else if (key == GKEY_1) choose_dialogue(0);
        else if (key == GKEY_2) choose_dialogue(1);
        else if (is_ok) choose_dialogue(g_sel);
        return;
    }
    if (g_screen == SCR_DISPATCH) {
        if (key == GKEY_BACK || key == GKEY_BACKSPACE) { g_screen = SCR_STANDBY; return; }
        if (key == GKEY_LEFT)       g_sel = (g_sel + 3) % 4;
        else if (key == GKEY_RIGHT) g_sel = (g_sel + 1) % 4;
        else if (key == GKEY_UP || key == GKEY_DOWN) g_sel = (g_sel + 2) % 4;
        else if (key == GKEY_1) choose_dispatch(0);
        else if (key == GKEY_2) choose_dispatch(1);
        else if (key == GKEY_3) choose_dispatch(2);
        else if (key == GKEY_4) choose_dispatch(3);
        else if (is_ok) choose_dispatch(g_sel);
        return;
    }
    if (g_screen == SCR_RESULT) {
        if (is_ok || key == GKEY_BACK || key == GKEY_BACKSPACE) next_after_result();
        return;
    }
}

/* =========================================================================
 * 2.5D 畫面渲染（320x240 ~ 800x480 全自適應）
 * ========================================================================= */

static void render_standby(int w, int h) {
    uint32_t wall_bg = g_night ? 0x0F172A : 0x1E3A5F;
    hal_clear(wall_bg);

    /* 1. 2.5D 調度室雷達監控大牆 */
    int mw = (w * 74) / 100, mh = (h * 33) / 100;
    int mx = (w - mw) / 2, my = 24;
    hal_fill_round_rect(mx, my, mw, mh, 5, 0x091926);
    hal_draw_round_rect(mx, my, mw, mh, 5, 0x38BDF8);
    for (int gx = mx + 16; gx < mx + mw - 4; gx += 18) {
        hal_draw_line(gx, my + 2, gx, my + mh - 3, 0x0E4A6B);
    }
    int rcx = mx + 30, rcy = my + mh / 2;
    int rr = (mh / 2) - 6; if (rr > 24) rr = 24; if (rr < 10) rr = 10;
    hal_draw_circle(rcx, rcy, rr, 0x22D3EE);
    hal_draw_circle(rcx, rcy, (rr * 55) / 100, 0x0E7490);
    int aidx = (int)(g_anim_tick & 15u);
    hal_draw_line(rcx, rcy, rcx + (rr * COS16[aidx]) / 100, rcy + (rr * SIN16[aidx]) / 100, 0x67E8F9);

    int bar_x0 = rcx + rr + 14;
    int max_bh = mh - 16; if (max_bh < 8) max_bh = 8;
    for (int i = 0; i < 8 && bar_x0 + i * 12 + 8 < mx + mw - 6; i++) {
        int bh = 5 + ((i * 7 + (int)(g_anim_tick / 2u)) % max_bh);
        hal_fill_rect(bar_x0 + i * 12, my + mh - 6 - bh, 7, bh, (i & 1) ? 0x38BDF8 : 0x22C55E);
    }

    /* 2. 2.5D 透視木紋調度桌 */
    int desk_y = (h * 58) / 100;
    int dxs[5] = { 0, w / 5, w, w, 0 };
    int dys[5] = { desk_y + 22, desk_y, desk_y, h, h };
    hal_fill_poly(dxs, dys, 5, g_night ? 0x78350F : 0xF59E0B);
    int txs[4] = { 0, w / 5, w, w };
    int tys[4] = { desk_y + 22, desk_y, desk_y, desk_y + 5 };
    hal_fill_poly(txs, tys, 4, g_night ? 0x92400E : 0xFBBF24);

    /* 3. 2.5D 斜角桌上電腦螢幕 ＋ 閃爍紅警燈 */
    int mxs[4] = { 16, 84, 84, 16 };
    int mys[4] = { desk_y - 24, desk_y - 10, desk_y + 32, desk_y + 20 };
    hal_fill_poly(mxs, mys, 4, 0x0F172A);
    int sxs[4] = { 20, 80, 80, 20 };
    int sys[4] = { desk_y - 19, desk_y - 7, desk_y + 27, desk_y + 17 };
    hal_fill_poly(sxs, sys, 4, 0x14532D);

    int flash = ((g_anim_tick / 4u) & 1u) == 0;
    if (flash) hal_fill_circle(52, desk_y - 22, 14, 0x7F1D1D);
    hal_fill_rect(44, desk_y - 19, 16, 5, 0x475569);
    hal_fill_circle(52, desk_y - 21, 6, flash ? 0xFF3B30 : 0x991B1B);

    /* 4. 2.5D 調度員角色（藍制服 + 金警徽 + 耳麥） */
    int dx = (w * 76) / 100, dy = desk_y - 10;
    hal_fill_round_rect(dx - 16, dy + 2, 32, 32, 8, 0x2563EB);
    hal_fill_rect(dx + 4, dy + 11, 5, 6, 0xFACC15);
    hal_fill_circle(dx, dy - 11, 13, 0xFED7AA);
    hal_fill_rect(dx - 12, dy - 24, 24, 9, 0x1E293B);
    hal_draw_circle(dx, dy - 12, 14, 0x0F172A);
    hal_fill_rect(dx - 16, dy - 15, 4, 9, 0x334155);
    hal_fill_rect(dx - 5, dy - 12, 3, 3, 0x0F172A);
    hal_fill_rect(dx + 2, dy - 12, 3, 3, 0x0F172A);

    /* 5. 頂部關卡膠囊（絕不劇透案件類型！） */
    int pw = 176, ph = 20, px = (w - pw) / 2;
    hal_fill_round_rect(px, 2, pw, ph, 8, 0x0F172A);
    hal_draw_round_rect(px, 2, pw, ph, 8, 0x64748B);
    hal_set_text_bg(0x0F172A);
    char lbuf[64];
    snprintf(lbuf, sizeof(lbuf), "Level %d · 緊急來電", g_level + 1);
    draw_centered_text(w / 2, 4, lbuf, pw - 8, 0xF8FAFC);

    /* 6. 來電閃爍提示牌 */
    int bx = (w * 30) / 100, by = desk_y - 26;
    uint32_t abg = flash ? 0xDC2626 : 0x991B1B;
    hal_fill_round_rect(bx, by, 108, 22, 5, abg);
    hal_draw_round_rect(bx, by, 108, 22, 5, 0xFCA5A5);
    hal_set_text_bg(abg);
    draw_centered_text(bx + 54, by + 3, "911 響鈴中..", 100, 0xFFFFFF);

    /* 7. 底部大紅色 Start / 接聽按鈕 ＋ 快捷鍵提示 */
    int sw = 156, sh = 28, sx = (w - sw) / 2, sy = h - sh - 22;
    hal_fill_round_rect(sx, sy + 2, sw, sh, 7, 0x7F1D1D);
    hal_fill_round_rect(sx, sy, sw, sh, 7, 0xEF4444);
    hal_draw_round_rect(sx, sy, sw, sh, 7, 0xFFFFFF);
    hal_set_text_bg(0xEF4444);
    draw_centered_text(w / 2, sy + 6, "接聽電話 [Enter]", sw - 10, 0xFFFFFF);

    hal_fill_rect(0, h - 18, w, 18, 0x0F172A);
    hal_set_text_bg(0x0F172A);
    char fbuf[80];
    snprintf(fbuf, sizeof(fbuf), "G跳關 T日夜 H說明(%d星) ESC離開", g_stars);
    draw_centered_text(w / 2, h - 17, fbuf, w - 8, 0x94A3B8);
}

static void draw_monitor_frame(int w, int h, int *out_ix, int *out_iy, int *out_iw, int *out_ih, int *out_desk_y) {
    hal_clear(g_night ? 0x090D16 : 0x1E293B);
    int desk_y = h - (h <= 260 ? 58 : 74);
    hal_fill_rect(0, desk_y, w, h - desk_y, g_night ? 0x451A03 : 0xD97706);

    /* 2.5D 鍵盤輪廓 */
    int kxs[4] = { (w * 18) / 100, (w * 82) / 100, (w * 88) / 100, (w * 12) / 100 };
    int kys[4] = { desk_y + 6, desk_y + 6, h - 2, h - 2 };
    hal_fill_poly(kxs, kys, 4, 0x1E293B);

    /* 電腦螢幕外框與底座 */
    int pad = (w <= 340) ? 5 : 10;
    int mon_h = desk_y - 5;
    hal_fill_round_rect(pad, 3, w - pad * 2, mon_h, 6, 0x0F172A);
    hal_draw_round_rect(pad, 3, w - pad * 2, mon_h, 6, 0x475569);
    hal_fill_rect(w / 2 - 18, 3 + mon_h, 36, 8, 0x334155);

    int ix = pad + 4, iw = w - (pad + 4) * 2;
    hal_fill_rect(ix, 6, iw, 20, 0x111827);
    hal_set_text_bg(0x111827);
    hal_draw_text_utf8(ix + 4, 8, "911 Emergency", iw / 2, 0xEF4444);

    char lvbuf[40];
    int rem_s = 3 - g_bad_cnt; if (rem_s < 0) rem_s = 0;
    snprintf(lvbuf, sizeof(lvbuf), "Lv.%d %s", g_level + 1,
             rem_s == 3 ? "***" : (rem_s == 2 ? "**-" : (rem_s == 1 ? "*--" : "---")));
    int lw = utf8_total_units(lvbuf) * 8;
    hal_draw_text_utf8(ix + iw - lw - 4, 8, lvbuf, lw + 4, 0xFACC15);

    *out_ix = ix;
    *out_iy = 28;
    *out_iw = iw;
    *out_ih = mon_h - 26;
    *out_desk_y = desk_y;
}

static void render_call(int w, int h) {
    int ix, iy, iw, ih, desk_y;
    draw_monitor_frame(w, h, &ix, &iy, &iw, &ih, &desk_y);

    int max_bw = (iw * 84) / 100;
    int max_u = (max_bw - 12) / 8;
    if (max_u < 12) max_u = 12;
    int lh = 17;

    /* 由下往上繪製最新對話泡泡 */
    int cur_y = iy + ih - 3;
    for (int i = g_chat_len - 1; i >= 0; i--) {
        const ChatMsg *m = &g_chat[i];
        int st[6], ln[6];
        int nlines = wrap_utf8(m->text, max_u, 5, st, ln);
        if (nlines <= 0) continue;
        int bh = nlines * lh + 6;
        cur_y -= (bh + 4);
        if (cur_y < iy) break;

        if (m->who == 2) {
            hal_fill_round_rect(ix + 8, cur_y, iw - 16, bh, 4, 0x7F1D1D);
            hal_draw_round_rect(ix + 8, cur_y, iw - 16, bh, 4, 0xF87171);
            hal_set_text_bg(0x7F1D1D);
            draw_centered_text(ix + iw / 2, cur_y + 3, m->text, iw - 24, 0xFEF08A);
            continue;
        }

        int is_d = (m->who == 0);
        int bw = max_bw;
        if (nlines == 1) {
            int est = utf8_total_units(m->text) * 8 + 14;
            if (est < bw) bw = est;
            if (bw < 84) bw = 84;
        }
        int bx = is_d ? (ix + iw - bw - 3) : (ix + 3);
        uint32_t bbg = is_d ? (m->is_bad ? 0x991B1B : 0xEF4444) : 0xF8FAFC;
        uint32_t bfg = is_d ? 0xFFFFFF : 0x0F172A;

        hal_fill_round_rect(bx, cur_y, bw, bh, 5, bbg);
        hal_set_text_bg(bbg);
        for (int li = 0; li < nlines; li++) {
            hal_draw_text_utf8_len(bx + 6, cur_y + 3 + li * lh,
                                   m->text + st[li], ln[li], bw - 10, bfg);
        }
    }

    /* 底部 2 顆隨機洗牌後的大紅選項按鈕 */
    int btn_h = (h <= 260) ? 24 : 30;
    int btn_w = w - 14;
    int sy = desk_y + 4;
    for (int idx = 0; idx < 2; idx++) {
        const char *q = NULL;
        get_round_option(g_round, idx, &q, NULL, NULL);
        int by = sy + idx * (btn_h + 4);
        int sel = (g_sel == idx);
        uint32_t bg = sel ? 0xEF4444 : 0xB91C1C;
        hal_fill_round_rect(7, by + 2, btn_w, btn_h, 6, 0x7F1D1D);
        hal_fill_round_rect(7, by, btn_w, btn_h, 6, bg);
        hal_draw_round_rect(7, by, btn_w, btn_h, 6, sel ? 0xFEF08A : 0xFCA5A5);
        hal_set_text_bg(bg);
        char label[128];
        snprintf(label, sizeof(label), "[%d] %s", idx + 1, q ? q : "");
        hal_draw_text_utf8(13, by + (btn_h - 16) / 2, label, btn_w - 12, 0xFFFFFF);
    }
}

static void render_dispatch(int w, int h) {
    int ix, iy, iw, ih, desk_y;
    draw_monitor_frame(w, h, &ix, &iy, &iw, &ih, &desk_y);

    int y = iy + 4;
    hal_set_text_bg(0x0F172A);
    hal_draw_text_utf8(ix + 6, y, "請依對話線索選擇派遣單位：", iw - 12, 0xFACC15);
    y += 20;

    /* 顯示最後 2 則非系統對話作為提示摘要 */
    int shown = 0;
    int idxs[2] = {-1, -1};
    for (int i = g_chat_len - 1; i >= 0 && shown < 2; i--) {
        if (g_chat[i].who != 2) idxs[1 - shown++] = i;
    }
    int max_u = (iw - 22) / 8; if (max_u < 12) max_u = 12;
    for (int k = 0; k < 2; k++) {
        if (idxs[k] < 0) continue;
        const ChatMsg *m = &g_chat[idxs[k]];
        int is_d = (m->who == 0);
        char linebuf[180];
        snprintf(linebuf, sizeof(linebuf), "%s%s", is_d ? "我：" : "報案：", m->text);
        int st[4], ln[4];
        int nl = wrap_utf8(linebuf, max_u, 3, st, ln);
        int bh = nl * 17 + 6;
        if (y + bh > iy + ih) break;
        uint32_t bg = is_d ? 0x7F1D1D : 0xF1F5F9;
        uint32_t fg = is_d ? 0xFECACA : 0x0F172A;
        hal_fill_round_rect(ix + 4, y, iw - 8, bh, 5, bg);
        hal_set_text_bg(bg);
        for (int li = 0; li < nl; li++) {
            hal_draw_text_utf8_len(ix + 9, y + 3 + li * 17, linebuf + st[li], ln[li], iw - 18, fg);
        }
        y += bh + 5;
    }

    /* 底部 2x2 派遣單位按鈕 */
    int gap = 6;
    int bw = (w - 16 - gap) / 2;
    int bh = (h <= 260) ? 24 : 30;
    int sy = desk_y + 4;
    for (int idx = 0; idx < 4; idx++) {
        int col = idx & 1, row = idx >> 1;
        int bx = 8 + col * (bw + gap);
        int by = sy + row * (bh + 4);
        int sel = (g_sel == idx);
        hal_fill_round_rect(bx, by + 2, bw, bh, 6, UNIT_DK[idx]);
        hal_fill_round_rect(bx, by, bw, bh, 6, UNIT_COL[idx]);
        hal_draw_round_rect(bx, by, bw, bh, 6, sel ? 0xFEF08A : 0xCBD5E1);
        hal_set_text_bg(UNIT_COL[idx]);
        char lb[32];
        snprintf(lb, sizeof(lb), "[%d] %s", idx + 1, UNIT_NAME[idx]);
        draw_centered_text(bx + bw / 2, by + (bh - 16) / 2, lb, bw - 8, 0xFFFFFF);
    }
}

static void draw_25d_vehicle(int cx, int cy, int unit_idx) {
    int bob = ((g_anim_tick & 3u) == 1 || (g_anim_tick & 3u) == 2) ? 1 : -1;
    int y = cy + bob;
    int flash = ((g_anim_tick / 3u) & 1u) == 0;

    if (unit_idx == 3) {
        hal_fill_round_rect(cx - 38, y - 20, 76, 40, 8, 0x334155);
        hal_draw_round_rect(cx - 38, y - 20, 76, 40, 8, 0x94A3B8);
        hal_set_text_bg(0x334155);
        draw_centered_text(cx, y - 8, g_is_win ? "已掛斷" : "通話失敗", 70, 0xF8FAFC);
        return;
    }

    uint32_t body = (unit_idx == 0) ? 0x1E3A8A : (unit_idx == 1 ? 0xF8FAFC : 0xDC2626);
    uint32_t side = (unit_idx == 0) ? 0x172554 : (unit_idx == 1 ? 0xE2E8F0 : 0xB91C1C);
    uint32_t strp = (unit_idx == 0) ? 0xFACC15 : (unit_idx == 1 ? 0xEF4444 : 0xFEF08A);

    /* 2.5D 立體車身（正面 + 右側透視面 + 車頂） */
    hal_fill_rect(cx - 34, y - 15, 46, 34, body);
    int sxs[4] = { cx + 12, cx + 36, cx + 36, cx + 12 };
    int sys[4] = { y - 15,  y - 7,   y + 19,  y + 19 };
    hal_fill_poly(sxs, sys, 4, side);
    int rxs[4] = { cx - 34, cx - 10, cx + 36, cx + 12 };
    int rys[4] = { y - 15,  y - 23,  y - 7,   y - 15 };
    hal_fill_poly(rxs, rys, 4, 0xCBD5E1);

    /* 車身條紋與擋風玻璃 */
    hal_fill_rect(cx - 34, y + 5, 46, 6, strp);
    int wxs[4] = { cx + 14, cx + 33, cx + 33, cx + 14 };
    int wys[4] = { y - 11,  y - 5,   y + 4,   y + 2 };
    hal_fill_poly(wxs, wys, 4, 0x38BDF8);

    /* 車身識別符號 */
    if (unit_idx == 1) {
        /* 醫療藍十字 */
        hal_fill_rect(cx - 14, y - 11, 6, 14, 0x2563EB);
        hal_fill_rect(cx - 18, y - 7,  14, 6, 0x2563EB);
    } else if (unit_idx == 0) {
        hal_set_text_bg(body);
        draw_centered_text(cx - 11, y - 11, "911", 40, 0xF8FAFC);
    } else {
        hal_set_text_bg(body);
        draw_centered_text(cx - 11, y - 11, "FIRE", 40, 0xFEF08A);
    }

    /* 輪胎 */
    hal_fill_circle(cx - 18, y + 20, 7, 0x0F172A);
    hal_fill_circle(cx + 22, y + 20, 7, 0x0F172A);
    hal_fill_circle(cx - 18, y + 20, 3, 0x94A3B8);
    hal_fill_circle(cx + 22, y + 20, 3, 0x94A3B8);

    /* 車頂閃爍警燈 */
    if (flash) hal_fill_circle(cx + 2, y - 21, 11, (unit_idx == 0) ? 0x1D4ED8 : 0x991B1B);
    hal_fill_rect(cx - 5, y - 23, 14, 5, flash ? 0xEF4444 : 0x3B82F6);
}

static void render_result(int w, int h) {
    /* 天空、草地、捲動公路 */
    hal_clear(g_is_win ? 0x7DD3FC : 0x334155);
    int grass_y = (h * 46) / 100;
    int road_y  = (h * 58) / 100;
    hal_fill_rect(0, grass_y, w, h - grass_y, g_is_win ? 0x4ADE80 : 0x475569);
    hal_fill_rect(0, road_y, w, 34, 0x334155);

    int off = (int)((g_anim_tick * 6u) % 32u);
    for (int rx = -off; rx < w; rx += 32) {
        hal_fill_rect(rx, road_y + 15, 16, 3, 0xFEF08A);
    }

    if (g_is_win) {
        int span = w + 40;
        for (int i = 0; i < 4; i++) {
            int tx = ((i * 95 - (int)(g_anim_tick * 2u)) % span + span) % span - 20;
            hal_fill_rect(tx - 3, grass_y - 12, 6, 16, 0x92400E);
            hal_fill_circle(tx, grass_y - 18, 12, 0x16A34A);
        }
    }

    /* 2.5D 立體車輛滑入中央 */
    int vx = 36 + (int)(g_anim_tick * 10u);
    if (vx > w / 2) vx = w / 2;
    draw_25d_vehicle(vx, road_y - 4, g_chosen_unit);

    /* 斜條紅底過關／失敗橫幅 */
    int bxs[4] = { 0, w, w, 0 };
    int bys[4] = { 24, 14, 42, 52 };
    uint32_t ban_col = g_is_win ? 0xEF4444 : 0x991B1B;
    hal_fill_poly(bxs, bys, 4, ban_col);
    hal_set_text_bg(ban_col);
    char hdr[64];
    if (g_is_win) {
        snprintf(hdr, sizeof(hdr), "LEVEL %d 過關! %s", g_level + 1,
                 g_earned_stars == 3 ? "***" : (g_earned_stars == 2 ? "**-" : "*--"));
    } else {
        snprintf(hdr, sizeof(hdr), "LEVEL %d 任務失敗", g_level + 1);
    }
    draw_centered_text(w / 2, 24, hdr, w - 16, 0xFFFFFF);

    /* 結果說明卡片（揭曉案件分類） */
    char msg[200];
    snprintf(msg, sizeof(msg), "[%s] %s",
             g_case_ok ? g_case.cat : "案件",
             g_is_win ? (g_case_ok ? g_case.win_msg : "過關！") : g_fail_reason);
    int max_u = (w - 32) / 8; if (max_u < 14) max_u = 14;
    int st[4], ln[4];
    int nl = wrap_utf8(msg, max_u, 3, st, ln);
    int ch = nl * 17 + 8;
    int cy = h - ch - 38;
    hal_fill_round_rect(10, cy, w - 20, ch, 6, 0x0F172A);
    hal_draw_round_rect(10, cy, w - 20, ch, 6, g_is_win ? 0x4ADE80 : 0xF87171);
    hal_set_text_bg(0x0F172A);
    for (int li = 0; li < nl; li++) {
        hal_draw_text_utf8_len(16, cy + 4 + li * 17, msg + st[li], ln[li], w - 32, 0xF8FAFC);
    }

    /* 黃色繼續／重試按鈕 */
    int bw = 152, bh = 26, bx = (w - bw) / 2, by = h - bh - 6;
    hal_fill_round_rect(bx, by + 2, bw, bh, 7, 0xB45309);
    hal_fill_round_rect(bx, by, bw, bh, 7, 0xFACC15);
    hal_draw_round_rect(bx, by, bw, bh, 7, 0xFFFFFF);
    hal_set_text_bg(0xFACC15);
    draw_centered_text(w / 2, by + 5, g_is_win ? "下一關 [Enter]" : "重試本關 [Enter]", bw - 8, 0x0F172A);
}

static void render_jump_modal(int w, int h) {
    int mw = w - 24; if (mw > 320) mw = 320;
    int mh = 122;
    int mx = (w - mw) / 2, my = (h - mh) / 2;
    hal_clear_text_in_rect(mx - 4, my - 4, mw + 8, mh + 8);
    hal_dim_rect(0, 0, w, h);
    hal_fill_round_rect(mx, my, mw, mh, 8, 0x1E293B);
    hal_draw_round_rect(mx, my, mw, mh, 8, 0x38BDF8);
    hal_set_text_bg(0x1E293B);
    hal_draw_text_utf8(mx + 12, my + 10, "跳轉關卡 (1 ~ 1000)", mw - 24, 0xF8FAFC);

    char b[48];
    if (g_jump_digits[0]) snprintf(b, sizeof(b), "第 %s_ / %d 關", g_jump_digits, TOTAL_CASES);
    else snprintf(b, sizeof(b), "第 %d / %d 關", g_jump_target + 1, TOTAL_CASES);
    hal_draw_text_utf8(mx + 12, my + 38, b, mw - 24, 0xFACC15);
    hal_draw_text_utf8(mx + 12, my + 68, "直接打數字鍵，或 ←→±10 ↑↓±50", mw - 24, 0x94A3B8);
    hal_draw_text_utf8(mx + 12, my + 92, "[Enter]跳轉   [ESC]取消", mw - 24, 0x94A3B8);
}

static void render_help_modal(int w, int h) {
    int mw = w - 16; if (mw > 420) mw = 420;
    int mh = h - 16; if (mh > 224) mh = 224;
    int mx = (w - mw) / 2, my = (h - mh) / 2;
    hal_clear_text_in_rect(mx - 4, my - 4, mw + 8, mh + 8);
    hal_dim_rect(0, 0, w, h);
    hal_fill_round_rect(mx, my, mw, mh, 8, 0x1E293B);
    hal_draw_round_rect(mx, my, mw, mh, 8, 0xFACC15);
    hal_set_text_bg(0x1E293B);

    int y = my + 8, lh = 19;
    char l1[80], l2[80], l3[80];
    snprintf(l1, sizeof(l1), "職級：%s  過關：%d  總星：%d", get_rank_name(g_wins), g_wins, g_stars);
    snprintf(l2, sizeof(l2), "實機：%dx%d (%s) 鍵碼:%d",
             hal_get_width(), hal_get_height(), hal_is_xrgb() ? "XRGB" : "565", hal_get_last_keycode());
    snprintf(l3, sizeof(l3), "觸控：已停用(純按鍵)  關卡：%d", TOTAL_CASES);

    hal_draw_text_utf8(mx + 10, y,        "【911 緊急調度員 2.5D v3】", mw - 20, 0xFACC15);
    hal_draw_text_utf8(mx + 10, y + lh*1, l1, mw - 20, 0xF8FAFC);
    hal_draw_text_utf8(mx + 10, y + lh*2, "對話：[1]/[2]或↑↓選(位置每輪隨機)", mw - 20, 0xE2E8F0);
    hal_draw_text_utf8(mx + 10, y + lh*3, "派遣：1警車 2救護 3消防 4掛斷", mw - 20, 0xE2E8F0);
    hal_draw_text_utf8(mx + 10, y + lh*4, "快捷：G跳關 T日夜 R重玩 ESC退", mw - 20, 0xE2E8F0);
    hal_draw_text_utf8(mx + 10, y + lh*5, "規則：2題對話全錯或派錯車即失敗", mw - 20, 0xFCA5A5);
    hal_draw_text_utf8(mx + 10, y + lh*6, l2, mw - 20, 0x94A3B8);
    hal_draw_text_utf8(mx + 10, y + lh*7, l3, mw - 20, 0x94A3B8);
    hal_draw_text_utf8(mx + 10, my + mh - 20, "按 [Enter] 或 [ESC] 關閉", mw - 20, 0x38BDF8);
}

static void render_quit_modal(int w, int h) {
    int mw = w - 28; if (mw > 300) mw = 300;
    int mh = 96;
    int mx = (w - mw) / 2, my = (h - mh) / 2;
    hal_clear_text_in_rect(mx - 4, my - 4, mw + 8, mh + 8);
    hal_dim_rect(0, 0, w, h);
    hal_fill_round_rect(mx, my, mw, mh, 8, 0x1E293B);
    hal_draw_round_rect(mx, my, mw, mh, 8, 0xEF4444);
    hal_set_text_bg(0x1E293B);
    hal_draw_text_utf8(mx + 12, my + 12, "離開 911 緊急調度員？", mw - 24, 0xEF4444);
    hal_draw_text_utf8(mx + 12, my + 38, "進度已自動儲存至 C:\\cd358_911_v3.sav", mw - 24, 0xF8FAFC);
    hal_draw_text_utf8(mx + 12, my + 66, "[Enter]確定離開   [ESC]繼續遊玩", mw - 24, 0x94A3B8);
}

void game_render(void) {
    int w = hal_get_width(), h = hal_get_height();
    GameScreen base = g_screen;
    if (g_screen == SCR_JUMP || g_screen == SCR_HELP) base = g_ret_screen;
    else if (g_screen == SCR_QUIT) base = SCR_STANDBY;

    if (base == SCR_STANDBY)       render_standby(w, h);
    else if (base == SCR_CALL)     render_call(w, h);
    else if (base == SCR_DISPATCH) render_dispatch(w, h);
    else if (base == SCR_RESULT)   render_result(w, h);

    if (g_screen == SCR_JUMP)      render_jump_modal(w, h);
    else if (g_screen == SCR_HELP) render_help_modal(w, h);
    else if (g_screen == SCR_QUIT) render_quit_modal(w, h);

    hal_present();
}
