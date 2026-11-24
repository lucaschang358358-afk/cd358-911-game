#ifndef GAME_911_H
#define GAME_911_H

#include "hal.h"

void game_init(void);
void game_shutdown(void);
void game_on_key(GameKey key, char raw_char);
int  game_needs_anim(void);
void game_tick_anim(void);
void game_render(void);

#endif
