/*
 * ps3recomp - cellSail HLE implementation
 *
 * Stub. Player lifecycle and state management work.
 * No actual media playback occurs — games typically show
 * a black screen for cutscenes, which is acceptable for
 * initial recompilation testing.
 */

#include "ps3emu/guest_call.h"
#include "cellSail.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_read32 / vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* getenv -- Clang errors on the implicit declaration MSVC allows */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */

/* Internal state */

static int s_initialized = 0;
CellSailPlaybackHooks g_cell_sail_playback = {0};

typedef struct {
    int in_use;
    s32 state;
    CellSailPlayerCallback callback;
    void* callbackArg;
} SailPlayer;

static SailPlayer s_players[CELL_SAIL_PLAYER_MAX];

/* Players registered by cellSailPlayerInitialize2, keyed by the guest struct EA
 * the title passed as `pSelf`. Declared here because the adapter setters below
 * need it -- they take that same pointer, not an index into s_players[]. */
static struct { u32 self_ea, allocator_ea, callback_ea, arg_ea; int in_use; int state; }
    s_sail_players[CELL_SAIL_PLAYER_MAX];

/* A CellSailPlayer is a GUEST POINTER, not a small integer handle.
 *
 * Every player entry point below used to validate its first argument as an
 * index (`handle >= CELL_SAIL_PLAYER_MAX`) into the handle-keyed s_players[]
 * table, which rejects every real call: a guest pointer is always larger than
 * the table. cellSailPlayerInitialize2 has always registered the player by its
 * `pSelf` EA, so the two halves of this file disagreed about what a handle is.
 *
 * That is not a harmless error return. Tokyo Jungle sets its sound adapter
 * during audio init; on failure it abandons the path, closes a resource it
 * never opened ("sgxResClose unknown ID (0)"), terminates its SGX sound system
 * and then blocks forever joining an audio thread that will not exit.
 *
 * One lookup, used by all of them, so the disagreement cannot come back. */
static int sail_player_slot(u32 self_ea)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use && s_sail_players[i].self_ea == self_ea)
            return i;
    return -1;
}

/* Lifecycle */

s32 cellSailInit(void)
{
    printf("[cellSail] Init()\n");
    if (s_initialized)
        return (s32)CELL_SAIL_ERROR_ALREADY_INITIALIZED;
    memset(s_players, 0, sizeof(s_players));
    s_initialized = 1;
    return CELL_OK;
}

s32 cellSailTerm(void)
{
    printf("[cellSail] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* Player management */

s32 cellSailPlayerCreate(const CellSailPlayerAttribute* attr,
                           const CellSailPlayerResource* resource,
                           CellSailPlayerCallback callback,
                           void* callbackArg,
                           CellSailPlayerHandle* handle)
{
    (void)attr; (void)resource;
    printf("[cellSail] PlayerCreate()\n");

    if (!s_initialized) return (s32)CELL_SAIL_ERROR_NOT_INITIALIZED;
    if (!handle) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++) {
        if (!s_players[i].in_use) {
            memset(&s_players[i], 0, sizeof(SailPlayer));
            s_players[i].in_use = 1;
            s_players[i].state = CELL_SAIL_PLAYER_STATE_INITIALIZED;
            s_players[i].callback = callback;
            s_players[i].callbackArg = callbackArg;
            vm_write32((u32)(uintptr_t)handle, (u32)i);   /* guest out-param */
            return CELL_OK;
        }
    }
    return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;
}

static void sail_descriptor_cleanup_player(u32 player);
static int sail_player_descriptor(u32 player);
/* Finalize is synchronous: release descriptor databases while the backend
 * player/allocator still exists, then destroy its decoded buffers/session.
 * Keep callback registration alive through cleanup; clear it before reusing
 * the slot. Do not invent a player completion callback during finalization. */
static void sail_player_cleanup_slot(int slot)
{
    const u32 self = s_sail_players[slot].self_ea;
    sail_descriptor_cleanup_player(self);
    /* The backend destroy hook owns stop/release too; calling close first
     * would duplicate guest buffer-release callbacks in other backends. */
    if (g_cell_sail_playback.destroy) g_cell_sail_playback.destroy(self);
    memset(&s_sail_players[slot], 0, sizeof(s_sail_players[slot]));
}

/* NID 0x18B4629D. pSelf remains a raw guest EA in the HLE bridge. A missing
 * Finalize used to fake CELL_OK, leaking all four player registrations across
 * movie transitions until SetSoundAdapter rejected the next new player EA. */
s32 cellSailPlayerFinalize(u32 pSelf)
{
    const int slot = sail_player_slot(pSelf);
    if (slot < 0) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    printf("[cellSail] PlayerFinalize(self=0x%08X)\n", pSelf);
    sail_player_cleanup_slot(slot);
    return CELL_OK;
}

s32 cellSailPlayerDestroy(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerDestroy(%u)\n", handle);
    const int slot = sail_player_slot((u32)handle);
    if (slot < 0) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_player_cleanup_slot(slot);
    return CELL_OK;
}

/* CellSailEvent is {be32 major; be32 minor} -- one 64-bit register, major in
 * the high half. Async player calls report completion as major=2
 * (CALL_COMPLETED) with minor = the CellSailPlayerCall id that finished; a
 * title that drives SAIL asynchronously blocks until its handler sees that.
 * We run every call synchronously, so notify right after each one. */
#define SAIL_EV_CALL_COMPLETED 2
enum { SAIL_CALL_BOOT = 1, SAIL_CALL_OPEN_STREAM = 2, SAIL_CALL_CLOSE_STREAM = 3,
       SAIL_CALL_START = 10, SAIL_CALL_STOP = 11 };

static void sail_notify(int slot, u32 major, u32 minor, u64 arg0, u64 arg1)
{
    if (slot < 0 || !g_ps3_guest_caller) return;
    const u32 cb = s_sail_players[slot].callback_ea;
    if (!cb) return;
    g_ps3_guest_caller(cb, s_sail_players[slot].arg_ea,
                       ((u64)major << 32) | minor, arg0, arg1, 0, 0, 0, 0);
}

/* The player also reports every state it passes through: major=3
 * (PLAYER_STATE_CHANGED), minor 0, arg0 = the new state in the SDK's
 * numbering -- not the CELL_SAIL_PLAYER_STATE_* values in cellSail.h, which
 * are this file's own bookkeeping. Midway Arcade Origins maps arg0 through a
 * nine-entry table (CLOSED=2, OPENED=4, RUNNING=6, ...) and opens nothing until
 * boot has reported CLOSED; without these events it boots the player and then
 * waits on a black screen for good. */
#define SAIL_EV_STATE_CHANGED 3
/* major=4 with arg0 = the descriptor. Macross 30 builds its movie duration
 * from this event (CreateDatabase -> PAMF start/end time); without it the
 * duration stays 0 and the title fades the opening out after ~14 frames.
 * STREAM_CLOSED (5) is not sent yet: its title handler also runs descriptor
 * bookkeeping (two imports, a counter) not verified against this HLE. */
#define SAIL_EV_STREAM_OPENED 4
enum { SAIL_ST_BOOT_TRANSITION = 1, SAIL_ST_CLOSED = 2, SAIL_ST_OPEN_TRANSITION = 3,
       SAIL_ST_OPENED = 4, SAIL_ST_START_TRANSITION = 5, SAIL_ST_RUNNING = 6,
       SAIL_ST_STOP_TRANSITION = 7, SAIL_ST_CLOSE_TRANSITION = 8 };

/* One synchronous call: transition state, completion, settled state.
 * The call token belongs to completion arg1, not either state notification. */
static void sail_call(int slot, u32 call, u32 via, u32 to, u64 userParam)
{
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, via, 0);
    sail_notify(slot, SAIL_EV_CALL_COMPLETED, call, 0, userParam);
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, to, 0);
}

s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam)
{
    printf("[cellSail] PlayerBoot(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    sail_call(_sp, SAIL_CALL_BOOT, SAIL_ST_BOOT_TRANSITION, SAIL_ST_CLOSED, userParam);
    return CELL_OK;
}

s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, const char* path)
{
    /* path is a GUEST address; printing it as a host char* faulted here the
     * same way it did in CreateDescriptor. Copy it out of guest memory. */
    char path_host[512];
    path_host[0] = '\0';
    if (path) {
        u32 ea = (u32)(uintptr_t)path;
        size_t n = 0;
        while (n < sizeof(path_host) - 1) {
            char c = (char)vm_read8(ea + (u32)n);
            if (!c) break;
            path_host[n++] = c;
        }
        path_host[n] = '\0';
    }
    printf("[cellSail] PlayerOpenStream(%u, \"%s\") - stub\n", handle,
           path ? path_host : "null");
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (g_cell_sail_playback.open) {
        s32 rc = g_cell_sail_playback.open((u32)handle);
        if (rc) {
            cellSailPlaybackNotifyError((u32)handle, rc);
            sail_notify(_sp, SAIL_EV_CALL_COMPLETED, SAIL_CALL_OPEN_STREAM, (u64)(s64)rc, 0);
            return rc;
        }
    }
    /* The backend has opened the real descriptor before OPENED is reported. */
    const int desc = sail_player_descriptor((u32)handle);
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPEN_TRANSITION, 0);
    if (desc >= 0) sail_notify(_sp, SAIL_EV_STREAM_OPENED, 0, (u64)desc, 0);
    sail_notify(_sp, SAIL_EV_CALL_COMPLETED, SAIL_CALL_OPEN_STREAM, 0, 0);
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED, 0);
    return CELL_OK;
}

s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (g_cell_sail_playback.close) g_cell_sail_playback.close((u32)handle);
    sail_call(_sp, SAIL_CALL_CLOSE_STREAM, SAIL_ST_CLOSE_TRANSITION, SAIL_ST_CLOSED, 0);
    return CELL_OK;
}

s32 cellSailPlayerStart(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerStart(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    if (g_cell_sail_playback.start) {
        s32 rc = g_cell_sail_playback.start((u32)handle);
        if (rc) {
            cellSailPlaybackNotifyError((u32)handle, rc);
            sail_notify(_sp, SAIL_EV_CALL_COMPLETED, SAIL_CALL_START, (u64)(s64)rc, 0);
            return rc;
        }
        sail_call(_sp, SAIL_CALL_START, SAIL_ST_START_TRANSITION, SAIL_ST_RUNNING, 0);
        return CELL_OK; /* Actual decoder drain will notify EOS. */
    }
    /* Immediately signal finished since we don't play anything */
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    sail_call(_sp, SAIL_CALL_START, SAIL_ST_START_TRANSITION, SAIL_ST_RUNNING, 0);
    /* Nothing decodes the stream, so it has already ended: a finished stream
     * drops the player back to OPENED, which is what a title waiting out a
     * movie (Midway's logo AVIs) watches for. */
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_STOP_TRANSITION, 0);
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED, 0);
    return CELL_OK;
}

s32 cellSailPlayerStop(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerStop(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    if (g_cell_sail_playback.stop) {
        s32 rc = g_cell_sail_playback.stop((u32)handle);
        if (rc) { cellSailPlaybackNotifyError((u32)handle, rc); return rc; }
    }
    sail_call(_sp, SAIL_CALL_STOP, SAIL_ST_STOP_TRANSITION, SAIL_ST_OPENED, 0);
    return CELL_OK;
}

s32 cellSailPlayerPause(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_PAUSE;
    return CELL_OK;
}

s32 cellSailPlayerGetState(CellSailPlayerHandle handle, s32* state)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!state) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)state, (u32)s_sail_players[_sp].state);
    return CELL_OK;
}

s32 cellSailPlayerGetStreamNum(CellSailPlayerHandle handle, u32* streamNum)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!streamNum) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)streamNum, 0);   /* no streams in stub */
    return CELL_OK;
}

s32 cellSailPlayerGetStreamInfo(CellSailPlayerHandle handle, u32 streamIndex,
                                  CellSailStreamInfo* info)
{
    (void)streamIndex;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!info) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_SAIL_ERROR_NOT_FOUND;
}

/* r3 is a CellSailPlayer* -- a GUEST POINTER, the same `pSelf` that
 * cellSailPlayerInitialize2 registered -- not a small integer handle. Validating
 * it as an index (`handle >= CELL_SAIL_PLAYER_MAX`) rejects every real call,
 * because a guest pointer is always larger than the table size.
 *
 * That is not a harmless error return: Tokyo Jungle sets its sound adapter
 * during audio init, and on failure abandons the whole path -- it closes a
 * resource it never opened ("sgxResClose unknown ID (0)"), terminates its SGX
 * sound system, and then blocks forever joining an audio thread that is not
 * going to exit. Look the player up the way Initialize2 stored it. */
s32 cellSailPlayerSetSoundAdapter(CellSailPlayerHandle handle, u32 index, void* adapter)
{
    u32 self_ea = (u32)handle;
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use && s_sail_players[i].self_ea == self_ea)
            return g_cell_sail_playback.set_sound
                ? g_cell_sail_playback.set_sound(self_ea, index, (u32)(uintptr_t)adapter) : CELL_OK;
    return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
}

/* r3 is a CellSailPlayer* -- a GUEST POINTER, the same `pSelf` that
 * cellSailPlayerInitialize2 registered -- not a small integer handle. Validating
 * it as an index (`handle >= CELL_SAIL_PLAYER_MAX`) rejects every real call,
 * because a guest pointer is always larger than the table size.
 *
 * That is not a harmless error return: Tokyo Jungle sets its sound adapter
 * during audio init, and on failure abandons the whole path -- it closes a
 * resource it never opened ("sgxResClose unknown ID (0)"), terminates its SGX
 * sound system, and then blocks forever joining an audio thread that is not
 * going to exit. Look the player up the way Initialize2 stored it. */
s32 cellSailPlayerSetGraphicsAdapter(CellSailPlayerHandle handle, u32 index, void* adapter)
{
    (void)index; (void)adapter;
    u32 self_ea = (u32)handle;
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use && s_sail_players[i].self_ea == self_ea)
            return g_cell_sail_playback.set_graphics
                ? g_cell_sail_playback.set_graphics(self_ea, index, (u32)(uintptr_t)adapter) : CELL_OK;
    return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
}

s32 cellSailPlayerCancel(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    return CELL_OK;
}

s32 cellSailPlayerIsPaused(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return 0;
    return (s_sail_players[_sp].state == CELL_SAIL_PLAYER_STATE_PAUSE) ? 1 : 0;
}

s32 cellSailPlayerSetRepeatMode(CellSailPlayerHandle handle, s32 repeatMode, void* command)
{
    (void)command;
    printf("[cellSail] SetRepeatMode(%u, %d)\n", handle, repeatMode);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    /* Real ABI returns the (now-set) repeat mode in r3, not an error code. */
    return repeatMode;
}

/* ---------------------------------------------------------------------------
 * Descriptor management
 * -----------------------------------------------------------------------*/

#define MAX_DESCRIPTORS 16

typedef struct {
    int in_use;
    s32 streamType;
    CellSailPlayerHandle player;
    char uri[512];
    u32 database;
    int added;
} SailDescriptor;

static SailDescriptor s_descs[MAX_DESCRIPTORS];

s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* mediaInfo,
                                     char* uri,
                                     CellSailDescriptorHandle* desc)
{
    (void)mediaInfo;

    /* uri is a GUEST address. Printing it as a host char* faulted the moment
     * the title opened a movie -- the log line stopped mid-token at
     * 'uri="' and the process died with 0xC0000005. Copy it out of guest
     * memory first. The descriptor has always had a uri[512] field; nothing
     * ever filled it, so the URI was discarded even when it did not crash. */
    char uri_host[512];
    uri_host[0] = '\0';
    if (uri) {
        u32 ea = (u32)(uintptr_t)uri;
        size_t n = 0;
        while (n < sizeof(uri_host) - 1) {
            char c = (char)vm_read8(ea + (u32)n);
            if (!c) break;
            uri_host[n++] = c;
        }
        uri_host[n] = '\0';
    }

    printf("[cellSail] CreateDescriptor(player=%u, type=%d, uri=\"%s\")\n", handle,
           streamType, uri ? uri_host : "null");
    if (!desc) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < MAX_DESCRIPTORS; i++) {
        if (!s_descs[i].in_use) {
            memset(&s_descs[i], 0, sizeof(SailDescriptor));
            s_descs[i].in_use = 1;
            s_descs[i].streamType = streamType;
            s_descs[i].player = handle;
            snprintf(s_descs[i].uri, sizeof(s_descs[i].uri), "%s", uri_host);
            vm_write32((u32)(uintptr_t)desc, (u32)i);   /* guest out-param */
            return CELL_OK;
        }
    }
    return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;
}

s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc)
{
    (void)handle;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if(s_descs[desc].database)cellSailDescriptorDestroyDatabase(desc,(void*)(uintptr_t)s_descs[desc].database);
    s_descs[desc].in_use = 0;
    return CELL_OK;
}

s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc)
{
    (void)handle;
    printf("[cellSail] AddDescriptor(player=%u, desc=%u)\n", handle, desc);
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_descs[desc].added = 1;
    return g_cell_sail_playback.descriptor
        ? g_cell_sail_playback.descriptor((u32)handle, (u32)desc, s_descs[desc].uri) : CELL_OK;
}

s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle desc)
{
    (void)handle;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_descs[desc].added = 0;
    return CELL_OK;
}

s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,void* dbAddr,u32 dbSize,u64 arg)
{
    u64 db=(u64)(uintptr_t)dbAddr;
    if(desc>=MAX_DESCRIPTORS||!s_descs[desc].in_use||!db||db>0xFFFFFFFFu-127u||dbSize<128)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if(s_descs[desc].streamType!=0||!g_cell_sail_playback.create_database)
        return (s32)CELL_SAIL_ERROR_NOT_SUPPORTED;
    if(s_descs[desc].database)return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s32 rc=g_cell_sail_playback.create_database((u32)s_descs[desc].player,desc,(u32)db,dbSize,s_descs[desc].uri,arg);
    if(!rc)s_descs[desc].database=(u32)db;
    return rc;
}
s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc,void* dbAddr)
{
    if(desc>=MAX_DESCRIPTORS||!s_descs[desc].in_use||!s_descs[desc].database||
       (u64)(uintptr_t)dbAddr!=s_descs[desc].database)return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if(g_cell_sail_playback.destroy_database)g_cell_sail_playback.destroy_database(desc,s_descs[desc].database);
    s_descs[desc].database=0;
    return CELL_OK;
}
s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc)
{
    if(desc>=MAX_DESCRIPTORS||!s_descs[desc].in_use)return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return s_descs[desc].streamType;
}
static void sail_descriptor_cleanup_player(u32 player)
{
    for(u32 d=0;d<MAX_DESCRIPTORS;d++)if(s_descs[d].in_use&&(u32)s_descs[d].player==player){
        if(s_descs[d].database)cellSailDescriptorDestroyDatabase(d,(void*)(uintptr_t)s_descs[d].database);
        s_descs[d].in_use=0;
    }
}
/* A descriptor added to this player, or -1. */
static int sail_player_descriptor(u32 player)
{
    for(int d=MAX_DESCRIPTORS-1;d>=0;d--)
        if(s_descs[d].in_use&&s_descs[d].added&&(u32)s_descs[d].player==player)return d;
    return -1;
}

s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable)
{
    (void)enable;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc, char* uri, u32 maxLen)
{
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use || !uri)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    strncpy(GUEST_PTR(uri, char*), s_descs[desc].uri, maxLen - 1);
    uri[maxLen - 1] = '\0';
    return CELL_OK;
}

/* Memory allocator */

/* cellSailPlayerInitialize2(pSelf, pAllocator, pCallback, pUserParam,
 *                          pAttribute, pResource)  -- NID 0x23654375
 *
 * This was unimplemented, so it took the generic "return success and touch
 * nothing" path. That is the worst answer for an initialiser: the guest
 * believes its player struct is set up and reads whatever happened to be in
 * that memory. Tokyo Jungle then dereferenced one of those stale words and
 * faulted reading guest 0x10F000B4, well past the end of main memory.
 *
 * The struct fields are not documented here, and guessing offsets to write
 * would just trade one wrong value for another. What IS safe and sufficient is
 * to ZERO it: every pointer the guest reads out is then NULL, which callers
 * check, instead of a garbage address that faults. The handles we were given
 * are kept on our side, keyed by the struct EA, so no guest layout is assumed.
 */
#define SAIL_PLAYER_CLEAR   0x200u   /* well clear of the caller's next object */
#define SAIL_ADAPTER_CLEAR  0x100u


static void sail_zero_guest(u32 ea, u32 n)
{
    for (u32 i = 0; i < n; i += 4) vm_write32(ea + i, 0);
}

s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pUserParam, u32 pAttribute, u32 pResource)
{
    (void)pAttribute; (void)pResource;
    printf("[cellSail] PlayerInitialize2(self=0x%08X alloc=0x%08X cb=0x%08X)\n",
           pSelf, pAllocator, pCallback);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    /* PS3_NO_SAIL=1: report that the player cannot be created. On hardware
     * this call spawns the SAIL player's own worker threads, and a title that
     * then waits for one of them to signal readiness will wait forever against
     * a stub that spawns nothing. Failing cleanly lets a title take its
     * "no video" path instead of deadlocking during setup. */
    if (getenv("PS3_NO_SAIL")) {
        printf("[cellSail] PS3_NO_SAIL -- reporting player creation failure\n");
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    }

    /* Reject before touching guest memory or an existing live session. The
     * same EA must be finalized before reinitialization, not registered twice. */
    if ((u64)pSelf + SAIL_PLAYER_CLEAR > 0x100000000ULL)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (sail_player_slot(pSelf) >= 0)
        return (s32)CELL_SAIL_ERROR_ALREADY_INITIALIZED;
    int slot = -1;
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (!s_sail_players[i].in_use) { slot = i; break; }
    if (slot < 0) return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;

    sail_zero_guest(pSelf, SAIL_PLAYER_CLEAR);
    memset(&s_sail_players[slot], 0, sizeof(s_sail_players[slot]));
    s_sail_players[slot].in_use       = 1;
    s_sail_players[slot].self_ea      = pSelf;
    s_sail_players[slot].allocator_ea = pAllocator;
    s_sail_players[slot].callback_ea  = pCallback;
    s_sail_players[slot].arg_ea       = pUserParam;
    if (g_cell_sail_playback.initialize) g_cell_sail_playback.initialize(pSelf, pAllocator);
    return CELL_OK;
}

/* cellSailSoundAdapterInitialize(pSelf, pCallbacks, pArg) -- NID 0x3D0D3B72.
 * Same reasoning: zero it rather than leave the guest reading stale memory. */
s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] SoundAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_zero_guest(pSelf, SAIL_ADAPTER_CLEAR);
    if (g_cell_sail_playback.sound_initialize)
        g_cell_sail_playback.sound_initialize(pSelf, pCallbacks, pArg);
    return CELL_OK;
}

/* NID 0x7EB8D6B5. CellSailSoundFrameInfo is six BE words (24 bytes):
 * buffer EA, session ID, tag, status, 64-bit PTS. These are guest EAs,
 * deliberately u32 so HLE dispatch does not translate them to host pointers.
 * With no sound decoder attached, report no frame rather than claiming PCM.
 * A future backend owns its output and return flags without modification. */
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo)
{
    if (!pSelf || !pInfo || samples > 2048 ||
        (u64)pInfo + 24 > 0x100000000ULL)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (g_cell_sail_playback.sound_get_frame)
        return g_cell_sail_playback.sound_get_frame(pSelf, samples, pInfo);
    sail_zero_guest(pInfo, 24);
    return 255; /* no frame available; not a successful decoded audio frame */
}

/* These wrappers keep raw guest EAs and 64-bit clock arguments intact.
 * Without a decoder, format/PTS conversion is explicitly unsupported; no PCM
 * pointer, session/EOS, timestamp or decoded format is invented. */
s32 cellSailSoundAdapterFinalize(u32 pSelf)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return g_cell_sail_playback.sound_finalize
        ? g_cell_sail_playback.sound_finalize(pSelf) : CELL_OK;
}
s32 cellSailSoundAdapterSetPreferredFormat(u32 pSelf, u32 pFormat)
{
    if (!pSelf || !pFormat || (u64)pFormat + 24 > 0x100000000ULL)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (g_cell_sail_playback.sound_preferred_format)
        g_cell_sail_playback.sound_preferred_format(pSelf, pFormat);
    return CELL_OK;
}
s32 cellSailSoundAdapterGetFormat(u32 pSelf, u32 pFormat)
{
    if (!pSelf || !pFormat || (u64)pFormat + 24 > 0x100000000ULL)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return g_cell_sail_playback.sound_get_format
        ? g_cell_sail_playback.sound_get_format(pSelf, pFormat) : (s32)CELL_SAIL_ERROR_NOT_SUPPORTED;
}
s32 cellSailSoundAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 output)
{
    if (!pSelf || !output || (u64)output + 8 > 0x100000000ULL)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return g_cell_sail_playback.sound_pts_to_time
        ? g_cell_sail_playback.sound_pts_to_time(pSelf, pts, output) : (s32)CELL_SAIL_ERROR_NOT_SUPPORTED;
}
s32 cellSailSoundAdapterUpdateAvSync(u32 pSelf, u32 tag, u64 time)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return g_cell_sail_playback.sound_update_av_sync
        ? g_cell_sail_playback.sound_update_av_sync(pSelf, tag, time) : CELL_OK;
}

/* cellSailGraphicsAdapterInitialize(pSelf, pCallbacks, pArg)
 *
 * The video-side twin of cellSailSoundAdapterInitialize, and it was missing --
 * so the import returned whatever was in r3 and the guest read an adapter it
 * believed was initialised. Same treatment as the sound adapter: zero the guest
 * struct so every pointer the title reads back is NULL (which callers check)
 * rather than a stale address that faults or is silently followed. */
s32 cellSailGraphicsAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    (void)pArg;
    printf("[cellSail] GraphicsAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_zero_guest(pSelf, SAIL_ADAPTER_CLEAR);
    if (g_cell_sail_playback.graphics_initialize) g_cell_sail_playback.graphics_initialize(pSelf, pCallbacks, pArg);
    return CELL_OK;
}

/* cellSailGraphicsAdapterSetPreferredFormat(pSelf, pFormat)
 *
 * Records nothing: we do not decode video, so there is no format to honour.
 * It must still answer CELL_OK for a valid adapter, because a title that reads
 * an error here concludes its graphics adapter is unusable. */
s32 cellSailGraphicsAdapterSetPreferredFormat(u32 pSelf, u32 pFormat)
{
    printf("[cellSail] GraphicsAdapterSetPreferredFormat(self=0x%08X fmt=0x%08X)\n",
           pSelf, pFormat);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (g_cell_sail_playback.preferred_format) g_cell_sail_playback.preferred_format(pSelf, pFormat);
    return CELL_OK;
}

s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     CellSailMemAllocatorFuncs* pFuncs, u32 pArg)
{
    /* Both parameters are GUEST addresses, and both structs live in GUEST
     * memory with 32-bit big-endian pointers. Dereferencing them as host
     * structs was wrong twice over: the address was never translated, and the
     * host struct has 64-bit fields, so even the offsets did not line up. The
     * values stored here are guest OPD addresses that SAIL calls back into. */
    u32 alloc_ea = (u32)(uintptr_t)allocator;
    u32 funcs_ea = (u32)(uintptr_t)pFuncs;
    printf("[cellSail] MemAllocatorInitialize(allocator=0x%08X funcs=0x%08X)\n",
           alloc_ea, funcs_ea);
    if (!alloc_ea) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (g_cell_sail_playback.allocator_initialize)
        g_cell_sail_playback.allocator_initialize(alloc_ea, funcs_ea, pArg);
    if (funcs_ea) {
        vm_write32(alloc_ea + 0, vm_read32(funcs_ea + 0));   /* pAlloc */
        vm_write32(alloc_ea + 4, vm_read32(funcs_ea + 4));   /* pFree  */
    }
    return CELL_OK;
}

/* Real playback reports completion only once its last decoded frame expires. */
void cellSailPlaybackNotifyEos(u32 player)
{
    int slot = sail_player_slot(player);
    if (slot < 0) return;
    s_sail_players[slot].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    sail_notify(slot, 8, 0, 0, 0); /* SOURCE_EOS */
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_STOP_TRANSITION, 0);
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED, 0);
}
void cellSailPlaybackNotifyError(u32 player, s32 error)
{
    int slot = sail_player_slot(player);
    if (slot >= 0) sail_notify(slot, 1, 0, (u64)(s64)error, 0);
}
s32 cellSailGraphicsAdapterGetFrame2(u32 self, u32 info, u32 previous, u32 flip_time, u64 flags)
{
    return g_cell_sail_playback.get_frame
        ? g_cell_sail_playback.get_frame(self, info, previous, flip_time, flags) : 255;
}
s32 cellSailGraphicsAdapterGetFormat(u32 self, u32 format)
{
    return g_cell_sail_playback.get_format
        ? g_cell_sail_playback.get_format(self, format) : (s32)CELL_SAIL_ERROR_NOT_SUPPORTED;
}
s32 cellSailGraphicsAdapterPtsToTimePosition(u32 self, u64 pts, u32 output)
{
    return g_cell_sail_playback.pts_to_time
        ? g_cell_sail_playback.pts_to_time(self, pts, output) : (s32)CELL_SAIL_ERROR_NOT_SUPPORTED;
}
s32 cellSailGraphicsAdapterUpdateAvSync(u32 self, u64 time)
{
    (void)self; (void)time; /* Decoder presentation uses a monotonic host clock. */
    return CELL_OK;
}
