#ifndef CASES_DATA_H
#define CASES_DATA_H

#include <stdint.h>

#define TOTAL_CASES 1000

typedef struct {
    int ans; /* 0=警車, 1=救護車, 2=消防車, 3=掛斷 */
    const char *cat;
    const char *open_msg;
    const char *q1_good;
    const char *q1_bad;
    const char *r1_good;
    const char *r1_bad;
    const char *q2_good;
    const char *q2_bad;
    const char *r2_good;
    const char *r2_bad;
    const char *win_msg;
    const char *lose_msg;
} Case911;

/* 載入第 idx 關 (0..TOTAL_CASES-1)，回傳 1 成功、0 失敗 */
int cases_load(int idx, Case911 *out);

#endif
