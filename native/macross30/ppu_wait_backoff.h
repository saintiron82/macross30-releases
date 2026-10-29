#ifndef MACROSS_PPU_WAIT_BACKOFF_H
#define MACROSS_PPU_WAIT_BACKOFF_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Called only on the two reviewed zero-byte loop backedges. No guest writes. */
int macross_ppu_wait_backoff_enabled(void);
void macross_ppu_wait_backoff(uint32_t pc,uint32_t ea,uint32_t failed_reads);
#ifdef __cplusplus
}
#endif
#endif
