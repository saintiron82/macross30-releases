#ifndef MACROSS_SAIL_PLAYBACK_H
#define MACROSS_SAIL_PLAYBACK_H
#ifdef __cplusplus
extern "C" {
#endif
/* Install before guest initialization. No-op unless MACROSS_SAIL_PLAYBACK=1.
 * Requires MACROSS_VFS_ROOT and MACROSS_FFMPEG_DIR for actual open requests. */
void macross_sail_install(void);
#ifdef __cplusplus
}
#endif
#endif
