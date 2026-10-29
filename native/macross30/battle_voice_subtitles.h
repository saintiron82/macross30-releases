#pragma once
#include <stdint.h>

struct ppu_context;
extern "C" {
// selected() observes the final filename stem, after the game's random choice.
// commit() must be reached only on the original voice function's normal exit.
void macross_battle_subtitles_selected(ppu_context* ctx, uint32_t stem_ea);
void macross_battle_subtitles_commit(ppu_context* ctx);
void macross_battle_subtitles_tick(ppu_context* ctx);
void macross_battle_subtitles_script_caption(ppu_context* ctx);
// Observer only: defer retiring our text until the next safe caption tick.
void macross_battle_subtitles_reset(ppu_context* ctx);
}
