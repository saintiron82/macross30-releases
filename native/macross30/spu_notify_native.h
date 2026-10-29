/* spu_notify_native.h -- the Macross notify SPURS job (image 10001) without the SPU.
 *
 * Image 10001 ("Macross raw SPURS job A62F00", 3472 bytes) is a small
 * completion/notification job the game queues behind GPU/SPU work:
 *   1. PUTL a 0x7F completion marker (u64 0x000000000000007F) into every
 *      8-byte slot of a ring of items (the list in the descriptor);
 *   2. release up to n waiters from a 128-byte wait-queue line
 *      (GETLLAR/PUTLLC), waking each: PPU waiters (0xFFFF) by a
 *      sys_spu_thread_throw_event on the line's port, SPU-task waiters by
 *      cellSpursSendSignal;
 *   3. set bits on up to 17 CellSpursEventFlags (SPU-side
 *      cellSpursEventFlagSet), which can wake SPU tasks and the PPU.
 * Full description, effect order and the hook wiring:
 * native/analysis/notify-graphics-jobs-spec.md.
 *
 * Image 10002 (the 30 KB "graphics" job) is Edge Animation; it is not done
 * natively here -- macross_notify_job_native always returns MN_SPU for it.
 *
 * Trust: every code path (signature of the effects a job produces) is first
 * checked against the SPU -- the native job is built aside, the SPU job runs
 * and its PUT / PUTLLC / event stream is captured and compared.
 * MN_TRUST_AFTER matches take that path native; one mismatch keeps it on the
 * SPU for the rest of the run. NOTIFY_NATIVE=0: off (every job on the SPU,
 * nothing compared). NOTIFY_VERIFY=1: every job on the SPU, every job
 * compared. A no_notify_native file in the working directory turns the
 * native path off while it exists (checked once a second). */
#ifndef SPU_NOTIFY_NATIVE_H
#define SPU_NOTIFY_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MN_FAILED = -1,   /* native ran and the job asserted half-way (as the SPU
                         job would have): fail the job like a halted SPU job */
    MN_SPU    =  0,   /* run the SPU job (a shadow comparison may be armed --
                         see macross_notify_shadow_active) */
    MN_DONE   =  1    /* done natively; every guest effect has been applied */
};

/* Called by spu_run_spurs_job before staging, for images 10001 and 10002.
 *   desc/desc_size  the descriptor as the job would see it (queue-time
 *                   snapshot when there is one, else guest memory at job_ea)
 *   n_dma           input DMA list entries (header +0x0A / 8)
 *   spu_num         g_spurs_job_spu_num (LS 0x1C8 for jobs that ask)
 *   ea_bin/size_bin the job binary (its bytes are what the job reads from the
 *                   SPURS kernel area of local store, LS 0x80..0x1CF)
 *   mbox/mbox_intr  (out, may be NULL) what a native job left in its outbound
 *                   and interrupt mailboxes -- the values spurs_job.c copies
 *                   into g_spurs_job_mbox / g_spurs_job_mbox_intr after an SPU
 *                   run (0 when empty; non-zero only when a throw_event went to
 *                   a SPURS port with no queue attached). Set for MN_DONE and
 *                   MN_FAILED.
 * Returns MN_DONE / MN_SPU / MN_FAILED. */
int macross_notify_job_native(int image_id, uint32_t job_ea, const uint8_t* desc, uint32_t desc_size,
                              uint32_t n_dma, uint32_t spu_num, uint32_t ea_bin, uint32_t size_bin,
                              uint32_t* mbox, uint32_t* mbox_intr);

/* Non-zero while the SPU run that follows an MN_SPU return must be captured
 * (set g_job_cap_on for it and forward spurs_job_cap_get/put here). */
int macross_notify_shadow_active(void);

/* Capture forwarders: same arguments as spurs_job_cap_get / _put (bit 31 of
 * ea marks GETLLAR / PUTLLC). No-ops unless a shadow is active. */
void macross_notify_cap_get(uint32_t ea, uint32_t lsa, const void* data, uint32_t size);
void macross_notify_cap_put(uint32_t ea, const void* data, uint32_t size);

/* After the SPU run of a job that returned MN_SPU: compares the captured
 * effects with the native ones when a shadow was armed, updates trust, and
 * disarms. failed: the SPU job failed (nothing compared). */
void macross_notify_job_after_spu(int image_id, int failed);

#ifdef __cplusplus
}
#endif

#endif /* SPU_NOTIFY_NATIVE_H */
