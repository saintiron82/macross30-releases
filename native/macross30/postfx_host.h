#ifndef MACROSS_POSTFX_HOST_H
#define MACROSS_POSTFX_HOST_H
/* Host replacement for the two post-processing SPU tasksets (posttask image
 * 318051041 and Edge MLAA image 452987026) while the renderer runs the
 * post-process on the GPU. Protocol: postfx_host_core.h and
 * native/analysis/postfx-task-protocol.md.
 *
 * MACROSS_POSTFX_HOST:
 *   unset / compare  the SPU runs as before; the host predicts every protocol
 *                    write of each frame and logs "[postfx-host]" differences
 *                    when the PPU's event-flag wait returns (nothing written)
 *   1 / host         under the GPU post-process (macross_spu_postfx_skip) the
 *                    kick's SendSignals are not delivered; the host does the
 *                    claim/complete protocol, notify labels, event flag and
 *                    wake counter instead. Otherwise the SPU path, unchanged.
 *   0 / off          nothing
 * A file named no_postfx_host in the working directory forces off. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { MACROSS_POSTFX_OFF = 0, MACROSS_POSTFX_HOST = 1, MACROSS_POSTFX_COMPARE = 2 };
int macross_postfx_host_mode(void);

/* Hooks installed into libs/spurs/cellSpurs.c at static-init time.
 * send_signal: 1 = the signal was taken (do not deliver it). */
int macross_postfx_host_send_signal(uint32_t taskset_ea, uint32_t task_id);
void macross_postfx_host_ef_wait_done(uint32_t flag_ea);

#ifdef __cplusplus
}
#endif
#endif
