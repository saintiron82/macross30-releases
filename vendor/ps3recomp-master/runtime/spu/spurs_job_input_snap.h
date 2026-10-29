/* Job input snapshot: the descriptor and its small input DMA segments, copied
 * when the chain walker queues a JOBLIST, so a job that runs later still sees
 * what the PPU published even if the PPU has reused that memory meanwhile
 * (entering the ship, geom3's 16-byte config block was overwritten by the
 * main allocator before its job ran). Large segments (vertex/index data) are
 * persistent mesh data and are read live. Set per job on the worker thread by
 * cellSpurs.c; read by spu_run_spurs_job. */
#ifndef SPURS_JOB_INPUT_SNAP_H
#define SPURS_JOB_INPUT_SNAP_H
#include <stdint.h>
#define SJ_SNAP_MAX_DMA 32u
#define SJ_SNAP_SMALL_SEG 1024u
typedef struct sj_snapshot {
    uint32_t job_ea, dsz, n_dma;
    const uint8_t* desc;                       /* dsz bytes of the descriptor */
    const uint8_t* seg[SJ_SNAP_MAX_DMA];       /* copied small segments, else NULL */
} sj_snapshot;
#if defined(_MSC_VER)
extern __declspec(thread) const sj_snapshot* g_sj_snap;
#else
extern __thread const sj_snapshot* g_sj_snap;
#endif
#endif
