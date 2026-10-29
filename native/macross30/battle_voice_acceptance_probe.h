#pragma once
#include <stdint.h>
struct ppu_context;
extern "C" {
void macross_battle_voice_probe_request(ppu_context* ctx, uint32_t filename_ea);
// LR001C4C18: r29 is the high-level stream; r3 is the low-level open result.
void macross_battle_voice_probe_open(ppu_context* ctx, uint32_t stream, int32_t result);
}
