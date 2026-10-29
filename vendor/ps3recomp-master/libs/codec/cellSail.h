/*
 * ps3recomp - cellSail HLE
 *
 * High-level media player wrapping PAMF/demux/decode pipeline.
 * Stub — player lifecycle tracked, no actual media playback.
 */

#ifndef PS3RECOMP_CELL_SAIL_H
#define PS3RECOMP_CELL_SAIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_SAIL_ERROR_NOT_INITIALIZED       0x80610701
#define CELL_SAIL_ERROR_ALREADY_INITIALIZED   0x80610702
#define CELL_SAIL_ERROR_INVALID_ARGUMENT      0x80610703
#define CELL_SAIL_ERROR_INVALID_STATE         0x80610704
#define CELL_SAIL_ERROR_NOT_SUPPORTED         0x80610705
#define CELL_SAIL_ERROR_NOT_FOUND             0x80610706
#define CELL_SAIL_ERROR_OUT_OF_MEMORY         0x80610707

/* Constants */
#define CELL_SAIL_PLAYER_MAX    4

/* Player state */
#define CELL_SAIL_PLAYER_STATE_CLOSED     0
#define CELL_SAIL_PLAYER_STATE_INITIALIZED 1
#define CELL_SAIL_PLAYER_STATE_BOOT_TRANSITION 2
#define CELL_SAIL_PLAYER_STATE_RUNNING    3
#define CELL_SAIL_PLAYER_STATE_PAUSE      4
#define CELL_SAIL_PLAYER_STATE_FINISHED   5

/* Stream types */
#define CELL_SAIL_STREAM_VIDEO   0
#define CELL_SAIL_STREAM_AUDIO   1
#define CELL_SAIL_STREAM_USER    2

/* Types */
typedef u32 CellSailPlayerHandle;

typedef struct CellSailPlayerAttribute {
    s32 threadPriority;
    u32 threadStackSize;
    u32 reserved[4];
} CellSailPlayerAttribute;

typedef struct CellSailPlayerResource {
    void* memAddr;
    u32 memSize;
    u32 reserved;
} CellSailPlayerResource;

typedef struct CellSailStreamInfo {
    u32 type;
    u32 codecType;
    u32 width;
    u32 height;
    u32 sampleRate;
    u32 channels;
    u32 reserved[4];
} CellSailStreamInfo;

typedef void (*CellSailPlayerCallback)(CellSailPlayerHandle handle,
                                         s32 event, s32 param,
                                         void* arg);

/* Functions */
s32 cellSailInit(void);
s32 cellSailTerm(void);

s32 cellSailPlayerCreate(const CellSailPlayerAttribute* attr,
                           const CellSailPlayerResource* resource,
                           CellSailPlayerCallback callback,
                           void* callbackArg,
                           CellSailPlayerHandle* handle);
s32 cellSailPlayerDestroy(CellSailPlayerHandle handle);

s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam);
s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, const char* path);
s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle);

s32 cellSailPlayerStart(CellSailPlayerHandle handle);
s32 cellSailPlayerStop(CellSailPlayerHandle handle);
s32 cellSailPlayerPause(CellSailPlayerHandle handle);

s32 cellSailPlayerGetState(CellSailPlayerHandle handle, s32* state);
s32 cellSailPlayerGetStreamNum(CellSailPlayerHandle handle, u32* streamNum);
s32 cellSailPlayerGetStreamInfo(CellSailPlayerHandle handle, u32 streamIndex,
                                  CellSailStreamInfo* info);

s32 cellSailPlayerSetSoundAdapter(CellSailPlayerHandle handle, u32 index, void* adapter);
s32 cellSailPlayerSetGraphicsAdapter(CellSailPlayerHandle handle, u32 index, void* adapter);

s32 cellSailPlayerCancel(CellSailPlayerHandle handle);
s32 cellSailPlayerIsPaused(CellSailPlayerHandle handle);
s32 cellSailPlayerSetRepeatMode(CellSailPlayerHandle handle, s32 repeatMode, void* command);

/* Descriptor management (for multi-stream sources) */
typedef u32 CellSailDescriptorHandle;

s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* mediaInfo,
                                     char* uri,
                                     CellSailDescriptorHandle* desc);
s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc);
s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc);
s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle desc);

s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,
                                       void* dbAddr, u32 dbSize,
                                       u64 arg);
s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc, void* dbAddr);
s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc);
s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable);
s32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc, char* uri, u32 maxLen);

/* Memory allocator */
typedef struct CellSailMemAllocator {
    void* (*alloc)(void* arg, u32 boundary, u32 size);
    void  (*free)(void* arg, u32 boundary, void* ptr);
    void* arg;
} CellSailMemAllocator;

/* Mirrors RPCS3's CellSailMemAllocatorFuncs: a guest struct of two callback
 * pointers, passed as one pointer arg (not two separate function pointers). */
typedef struct CellSailMemAllocatorFuncs {
    void* pAlloc;
    void* pFree;
} CellSailMemAllocatorFuncs;

/* Named pFuncs (not "callbacks") so the import-bridge generator's naming
 * heuristic (gen_imports.py param_marshal) treats this as a real guest
 * struct pointer needing host-pointer translation, not a raw callback value. */
s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pUserParam, u32 pAttribute, u32 pResource);
/* Synchronous teardown; raw guest EA, NID 0x18B4629D. */
s32 cellSailPlayerFinalize(u32 pSelf);
s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg);
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo);
/* Guest audio format is 24 bytes: s8 coding/chNum, BE s16 sampleNum,
 * BE s32 fs/chLayout/reserved0, BE s64 reserved1. All pointers below are EAs. */
s32 cellSailSoundAdapterFinalize(u32 pSelf);
s32 cellSailSoundAdapterSetPreferredFormat(u32 pSelf, u32 pFormat);
s32 cellSailSoundAdapterGetFormat(u32 pSelf, u32 pFormat);
s32 cellSailSoundAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 output);
s32 cellSailSoundAdapterUpdateAvSync(u32 pSelf, u32 tag, u64 time);
s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     CellSailMemAllocatorFuncs* pFuncs, u32 pArg);

/* Optional title-owned real playback. NULL keeps the SDK bring-up behavior.
 * Every address below is a guest EA. Callback tables are copied during init
 * because callers commonly pass temporary stack arrays. */
typedef struct CellSailPlaybackHooks {
    void (*allocator_initialize)(u32 self, u32 callbacks, u32 arg);
    void (*initialize)(u32 self, u32 allocator);
    void (*graphics_initialize)(u32 self, u32 callbacks, u32 arg);
    void (*preferred_format)(u32 self, u32 format);
    s32 (*set_graphics)(u32 player, u32 index, u32 adapter);
    s32 (*descriptor)(u32 player, u32 descriptor, const char* uri);
    s32 (*open)(u32 player);
    s32 (*start)(u32 player);
    s32 (*stop)(u32 player);
    void (*close)(u32 player);
    void (*destroy)(u32 player);
    s32 (*get_frame)(u32 adapter, u32 info, u32 previous, u32 flip_time, u64 flags);
    s32 (*get_format)(u32 adapter, u32 format);
    s32 (*pts_to_time)(u32 adapter, u64 pts, u32 output);
    s32 (*sound_get_frame)(u32 adapter, u32 samples, u32 info);
    void (*sound_initialize)(u32 self, u32 callbacks, u32 arg);
    void (*sound_preferred_format)(u32 self, u32 format);
    s32 (*set_sound)(u32 player, u32 index, u32 adapter);
    s32 (*sound_get_format)(u32 adapter, u32 format);
    s32 (*sound_pts_to_time)(u32 adapter, u64 pts, u32 output);
    s32 (*sound_update_av_sync)(u32 adapter, u32 tag, u64 time);
    s32 (*sound_finalize)(u32 adapter);
    s32 (*create_database)(u32 player,u32 descriptor,u32 db,u32 size,const char* uri,u64 arg);
    void (*destroy_database)(u32 descriptor,u32 db);
} CellSailPlaybackHooks;
extern CellSailPlaybackHooks g_cell_sail_playback;
void cellSailPlaybackNotifyEos(u32 player);
void cellSailPlaybackNotifyError(u32 player, s32 error);
s32 cellSailGraphicsAdapterGetFrame2(u32 self, u32 info, u32 previous, u32 flip_time, u64 flags);
s32 cellSailGraphicsAdapterGetFormat(u32 self, u32 format);
s32 cellSailGraphicsAdapterPtsToTimePosition(u32 self, u64 pts, u32 output);
s32 cellSailGraphicsAdapterUpdateAvSync(u32 self, u64 time);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SAIL_H */
