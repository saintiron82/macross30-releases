#pragma once
#include "ppu_recomp.h"

extern "C" {
// Optional registry interception after ppu_recomp_register(). Direct lifted
// calls bypass that registry; use the entry probe below to cover those too.
void macross_font_probe_register(void);
// Add at the start of func_0019B7B4 for complete coverage. Observes only;
// guest registers, memory and parser behavior are left unchanged.
void macross_font_probe_entry(ppu_context* ctx);
}
