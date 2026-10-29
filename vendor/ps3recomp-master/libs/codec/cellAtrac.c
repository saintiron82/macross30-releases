/*
 * ps3recomp - cellAtrac HLE
 *
 * ATRAC3plus RIFF decoding with bounded append streaming and looping. Guest
 * staging buffers are copied into an owned compressed-data cache; only bytes
 * supplied through SetData/AddStreamData are available to the decoder.
 *
 * Sample positions. FFmpeg's decoder output is a linear stream of 2048-sample
 * frames. The file's first audible sample sits at index
 *     first = fact[1] + 0x170
 * (0x170 is ATRAC3plus's own decoder delay -- the same constant PPSSPP uses),
 * and fact[0] audible samples follow. `smpl` loop points count from fact[2]
 * (fact[1] when the chunk has no third word). Checked on The Simpsons Arcade
 * Game's three tracks: first + fact[0] lands exactly on the last frame
 * boundary of the data chunk, and each loop spans the whole track.
 *
 * Plain ATRAC3 and second-buffer APIs are not implemented. Streaming caches
 * retain already supplied compressed bytes so loops never access overwritten
 * guest staging memory. Each cache is limited to 256 MiB.
 */

#include "cellAtrac.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../third_party/at3_standalone/at3_bridge.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"
#include "ps3emu/ps3_ftrace.h"

#define AT3P_SAMPLES   2048
#define AT3P_DELAY     0x170
#define ATRAC_MAX_SLOTS 16

typedef struct {
    u32 channels, block_align;
    u32 data_off, data_bytes;  /* the data chunk, relative to the file start */
    s32 first, end;            /* decoder-stream indices; end is exclusive   */
    s32 loop_start, loop_end;  /* same space, loop_end exclusive; -1 = none  */
} AtracTrackInfo;

typedef struct {
    u32 handle_ea;             /* 0 = free */
    u8* file;                 /* bounded host cache: only [0,loaded) is valid */
    u32 loaded, total, buffer_ea, buffer_bytes, offered;
    AtracTrackInfo t;
    void* dec;
    s32 pos;                   /* next decoder-stream index to output */
    s32 loop_num;              /* -1 forever, 0 none, n more laps */
    s32 next_frame;            /* frame the decoder expects next */
    s32 pcm_frame;             /* frame held in pcm[], -1 = none */
    float pcm[2][AT3P_SAMPLES];
    u32 diag_decode, diag_error, diag_info, diag_add;
    int diag_enabled;
    u64 identity_session, identity_decode;
    u32 identity_rows;
    int identity_enabled, identity_full_done;
} AtracSlot;

static AtracSlot s_slots[ATRAC_MAX_SLOTS];

/* Observation only. Bounds cap source hashing and total session output. */
static INIT_ONCE s_atrac_identity_once = INIT_ONCE_STATIC_INIT;
static int s_atrac_identity_ready, s_atrac_identity_on, s_atrac_identity_full;
static u64 s_atrac_identity_sessions;
static BOOL WINAPI atrac_identity_init(PINIT_ONCE once, PVOID arg, PVOID* context)
{
    (void)once; (void)arg; (void)context;
    const char* e = getenv("ATRAC_PCM_IDENTITY");
    s_atrac_identity_on = e && !strcmp(e, "1");
    e = getenv("ATRAC_PCM_IDENTITY_FULL");
    s_atrac_identity_full = e && !strcmp(e, "1");
    __atomic_store_n(&s_atrac_identity_ready, 1, __ATOMIC_RELEASE);
    return TRUE;
}
static int atrac_identity_on(void)
{
    if (!__atomic_load_n(&s_atrac_identity_ready, __ATOMIC_ACQUIRE))
        InitOnceExecuteOnce(&s_atrac_identity_once, atrac_identity_init, NULL, NULL);
    return s_atrac_identity_on;
}
static u64 atrac_identity_hash(const u8* bytes, u32 count)
{
    u64 hash = 14695981039346656037ULL;
    for (u32 i = 0; i < count; i++) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}
static void atrac_identity_full(AtracSlot* s)
{
    if (!s->identity_enabled || s->identity_full_done ||
        !s_atrac_identity_full || s->loaded != s->total) return;
    s->identity_full_done = 1;
    if (s->total > 262144u) return;
    const u64 hash = atrac_identity_hash(s->file, s->total);
    fprintf(stderr, "[atrac-identity] event=full_source session=%llu handle=%08X monotonic_us=%llu bytes=%u fnv1a64=%016llX scope=full_supplied_compressed_asset\n",
        (unsigned long long)s->identity_session, s->handle_ea,
        (unsigned long long)(ps3_ft_now() / 1000u), s->total, (unsigned long long)hash);
}
static void atrac_identity_source(AtracSlot* s)
{
    if (!atrac_identity_on()) return;
    s->identity_session = __atomic_add_fetch(&s_atrac_identity_sessions, 1u, __ATOMIC_RELAXED);
    if (s->identity_session > 128u) return;
    s->identity_enabled = 1;
    const u32 bytes = s->loaded < 65536u ? s->loaded : 65536u;
    const u64 hash = atrac_identity_hash(s->file, bytes);
    fprintf(stderr, "[atrac-identity] event=set_data session=%llu handle=%08X monotonic_us=%llu buffer_ea=%08X received=%u total=%u hash_bytes=%u fnv1a64=%016llX scope=supplied_prefix_only channels=%u frame_bytes=%u\n",
        (unsigned long long)s->identity_session, s->handle_ea,
        (unsigned long long)(ps3_ft_now() / 1000u), s->buffer_ea,
        s->loaded, s->total, bytes, (unsigned long long)hash,
        s->t.channels, s->t.block_align);
    atrac_identity_full(s);
}
static void atrac_identity_pcm(AtracSlot* s, u32 output, u32 samples)
{
    if (!s->identity_enabled) return;
    const u64 count = ++s->identity_decode;
    if (s->identity_rows >= 32u || (count > 4u && (count & (count - 1u)))) return;
    s->identity_rows++;
    const u32 values = samples * s->t.channels;
    const u32 bytes = values * 4u;
    u32 nonzero = 0, nonfinite = 0;
    float peak = 0;
    /* Read exactly the bytes successfully written to the guest by Decode. */
    for (u32 i = 0; i < values; i++) {
        const u32 bits = vm_read32(output + i * 4u);
        float value; memcpy(&value, &bits, 4);
        if (!isfinite(value)) { nonfinite++; continue; }
        if (value != 0.0f) nonzero++;
        const float magnitude = fabsf(value); if (magnitude > peak) peak = magnitude;
    }
    const u64 hash = atrac_identity_hash(vm_base + output, bytes);
    fprintf(stderr, "[atrac-identity] event=pcm session=%llu handle=%08X monotonic_us=%llu decode_count=%llu output_ea=%08X samples=%u channels=%u bytes=%u nonzero=%u nonfinite=%u peak=%.9g fnv1a64=%016llX byte_order=guest_big_endian pos_after=%d\n",
        (unsigned long long)s->identity_session, s->handle_ea,
        (unsigned long long)(ps3_ft_now() / 1000u), (unsigned long long)count,
        output, samples, s->t.channels, bytes, nonzero, nonfinite, (double)peak,
        (unsigned long long)hash, s->pos);
}

/* Opt-in bounded producer diagnostics. Filter by guest handle in hex, or
 * use "all". Counters belong to the slot and reset with SetData. */
static int atrac_diag_enabled(const AtracSlot* s)
{
    const char* value = getenv("ATRAC_STREAM_DIAG");
    if (!value || !*value) return 0;
    if (!strcmp(value, "all")) return 1;
    char* end = NULL;
    unsigned long handle = strtoul(value, &end, 16);
    return end != value && !*end && handle == s->handle_ea;
}

static void atrac_diag(AtracSlot* s, const char* event, u32* count, s32 result, u32 amount)
{
    if (!s->diag_enabled) return;
    const u32 n = ++*count;
    if (n > 4 && (n & (n - 1))) return;
    fprintf(stderr, "[atrac-stream] handle=%08X event=%s count=%u result=%08X amount=%u pos=%d next_frame=%d loaded=%u total=%u offered=%u\n",
            s->handle_ea, event, n, (u32)result, amount, s->pos,
            s->next_frame, s->loaded, s->total, s->offered);
}

static u32 le32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

/* The ATRAC3plus subformat GUID of a WAVE_FORMAT_EXTENSIBLE fmt chunk. */
static const u8 k_at3p_guid[16] = { 0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                    0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62 };

/* Parses a RIFF .at3 header. Returns 0 or a CELL_ATRAC_ERROR_*. */
int atrac_parse_riff(const u8* f, u32 bytes, AtracTrackInfo* t)
{
    memset(t, 0, sizeof(*t));
    t->loop_start = t->loop_end = -1;
    if (bytes < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4))
        return (int)CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
    u32 total = 0, off1 = 0, off2 = 0, has_fact = 0, lstart = 0, lend = 0, has_loop = 0;
    u32 o = 12;
    while (o <= bytes && bytes - o >= 8 && !t->data_off) {
        const u8* c = f + o;
        const u32 cs = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            if (o + 8 + 40 > bytes || cs < 40 || le16(c + 8) != 0xFFFE ||
                memcmp(c + 8 + 24, k_at3p_guid, 16))
                return (int)CELL_ATRAC_ERROR_UNKNOWN_FORMAT;   /* ATRAC3plus only */
            t->channels = le16(c + 10);
            t->block_align = le16(c + 20);
        } else if (!memcmp(c, "fact", 4) && cs >= 8 && o + 16 <= bytes) {
            has_fact = 1;
            total = le32(c + 8);
            off1 = le32(c + 12);
            off2 = (cs >= 12 && o + 20 <= bytes) ? le32(c + 16) : off1;
        } else if (!memcmp(c, "smpl", 4) && cs >= 36 + 24 && o + 8 + 60 <= bytes &&
                   le32(c + 8 + 28)) {
            has_loop = 1;
            lstart = le32(c + 8 + 36 + 8);
            lend = le32(c + 8 + 36 + 12);
        } else if (!memcmp(c, "data", 4)) {
            t->data_off = o + 8;
            t->data_bytes = cs;
        }
        if (t->data_off) break; /* data payload may intentionally be partial */
        if (cs > bytes - o - 8) return (int)CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
        o += 8 + cs;
        if (cs & 1) {
            if (o == bytes) return (int)CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
            o++; /* RIFF chunks are word-aligned */
        }
    }
    if (!t->data_off || !t->block_align || t->channels < 1 || t->channels > 2)
        return (int)CELL_ATRAC_ERROR_ILLEGAL_DATA;
    const s32 stream = (s32)(t->data_bytes / t->block_align) * AT3P_SAMPLES;
    t->first = (s32)off1 + AT3P_DELAY;
    t->end = has_fact ? t->first + (s32)total : stream;
    if (t->end > stream) t->end = stream;
    if (has_loop && lend >= lstart && lstart >= off2) {
        t->loop_start = t->first + (s32)(lstart - off2);
        t->loop_end = t->first + (s32)(lend - off2) + 1;
        if (t->loop_end > t->end) t->loop_end = t->end;
    }
    return 0;
}

static AtracSlot* slot_of(u32 handle_ea)
{
    for (int i = 0; i < ATRAC_MAX_SLOTS; i++)
        if (s_slots[i].handle_ea == handle_ea) return &s_slots[i];
    return NULL;
}

static s32 remain_frames(const AtracSlot* s)
{
    if (s->loaded == s->total) return CELL_ATRAC_ALLDATA_IS_ON_MEMORY;
    const u32 available = s->loaded > s->t.data_off ?
        (s->loaded - s->t.data_off) / s->t.block_align : 0;
    const u32 frame = (u32)(s->pos / AT3P_SAMPLES);
    return available > frame ? (s32)(available - frame) : 0;
}

/* Puts decoder-stream frame k in s->pcm, decoding forward from wherever the
 * decoder is. The decoder overlaps adjacent frames, so a jump backwards
 * restarts one frame early and discards that frame's output. */
static int decode_frame(AtracSlot* s, s32 k)
{
    if (s->pcm_frame == k) return 0;
    if (s->next_frame > k) {
        at3p_flush(s->dec);
        s->next_frame = k > 0 ? k - 1 : 0;
    }
    while (s->next_frame <= k) {
        const u32 at = s->t.data_off + (u32)s->next_frame * s->t.block_align;
        if (at > s->loaded || s->t.block_align > s->loaded - at)
            return (s32)CELL_ATRAC_ERROR_NODATA_IN_BUFFER;
        int n = 0;
        if (at3p_decode(s->dec, s->pcm[0], s->pcm[1], &n, s->file + at,
                        (int)s->t.block_align) < 0 || n != AT3P_SAMPLES) {
            /* A corrupt packet is an error, never fabricated successful PCM. */
            return (s32)CELL_ATRAC_ERROR_ILLEGAL_DATA;
        }
        s->next_frame++;
    }
    s->pcm_frame = k;
    return 0;
}

static int looping(const AtracSlot* s) { return s->t.loop_start >= 0 && s->loop_num != 0; }

s32 cellAtracSetDataAndGetMemSize(u32 pHandle, u32 pucBufferAddr, u32 uiReadByte,
                                  u32 uiBufferByte, u32 puiWorkMemByte)
{
    if (!pHandle || !pucBufferAddr) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    if (!uiBufferByte || uiReadByte > uiBufferByte)
        return (s32)CELL_ATRAC_ERROR_READSIZE_OVER_BUFFER;
    if ((u64)pucBufferAddr + uiBufferByte > 0x100000000ULL)
        return (s32)CELL_ATRAC_ERROR_API_FAIL;
    AtracSlot* s = slot_of(pHandle);
    if (!s) s = slot_of(0);
    if (!s) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    AtracTrackInfo t;
    const u8* file = vm_base + pucBufferAddr;
    int rc = atrac_parse_riff(file, uiReadByte, &t);
    if (rc) {
        fprintf(stderr, "[cellAtrac] SetData: not an ATRAC3plus file (0x%08X)\n", (u32)rc);
        return rc;
    }
    /* Cap each cache independently; never trust a RIFF size as an allocation
     * request without checking addition/limits. Guest refill remains bounded
     * by uiBufferByte; cached compressed bytes permit exact loop/seek replay. */
    const u64 data_end = (u64)t.data_off + t.data_bytes;
    const u64 riff_end = (u64)le32(file + 4) + 8;
    const u64 total64 = riff_end > data_end ? riff_end : data_end;
    if (total64 > 256u * 1024u * 1024u || total64 < uiReadByte)
        return (s32)CELL_ATRAC_ERROR_ILLEGAL_DATA;
    u8* cache = (u8*)malloc((size_t)total64);
    if (!cache) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    memcpy(cache, file, uiReadByte);
    if (s->dec) at3p_close(s->dec);
    free(s->file);
    memset(s, 0, sizeof(*s));
    s->handle_ea = pHandle;
    s->diag_enabled = atrac_diag_enabled(s);
    atrac_identity_source(s);
    s->file = cache;
    s->loaded = uiReadByte;
    s->total = (u32)total64;
    s->buffer_ea = pucBufferAddr;
    s->buffer_bytes = uiBufferByte;
    s->t = t;
    s->pos = t.first;
    s->next_frame = 0;         /* start of the stream, like the console */
    s->pcm_frame = -1;
    if (puiWorkMemByte) vm_write32(puiWorkMemByte, 0x1000);
    fprintf(stderr, "[cellAtrac] SetData handle=0x%08X ch=%u frames=%u first=%d end=%d loop=[%d,%d)\n",
            pHandle, t.channels, t.data_bytes / t.block_align, t.first, t.end,
            t.loop_start, t.loop_end);
    fprintf(stderr, "[cellAtrac] buffer=0x%08X capacity=%u received=%u total=%u data_off=%u frame_bytes=%u\n",
            pucBufferAddr, uiBufferByte, s->loaded, s->total,
            t.data_off, t.block_align);
    return CELL_OK;
}

s32 cellAtracCreateDecoder(u32 pHandle, u32 pucWorkMem, u32 uiPpuThreadPriority,
                           u32 uiSpuThreadPriority)
{
    (void)pucWorkMem; (void)uiPpuThreadPriority; (void)uiSpuThreadPriority;
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->dec) return (s32)CELL_ATRAC_ERROR_DECODER_WAS_CREATED;
    int ba = (int)s->t.block_align;
    s->dec = at3p_open((int)s->t.channels, &ba);
    return s->dec ? CELL_OK : (s32)CELL_ATRAC_ERROR_API_FAIL;
}

s32 cellAtracCreateDecoderExt(u32 pHandle, u32 pucWorkMem, u32 uiPpuThreadPriority,
                              u32 pExtRes)
{
    (void)pExtRes;
    return cellAtracCreateDecoder(pHandle, pucWorkMem, uiPpuThreadPriority, 0);
}

s32 cellAtracDeleteDecoder(u32 pHandle)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->dec) at3p_close(s->dec);
    free(s->file);
    memset(s, 0, sizeof(*s));
    return CELL_OK;
}

/* One call returns at most one frame's worth of samples per channel,
 * interleaved big-endian floats. */
s32 cellAtracDecode(u32 pHandle, u32 pfOutAddr, u32 puiSamples, u32 puiFinishflag,
                    u32 piRemainFrame)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (!s->dec) return (s32)CELL_ATRAC_ERROR_NO_DECODER;
    if (puiSamples) vm_write32(puiSamples, 0);
    if (puiFinishflag) vm_write32(puiFinishflag, 0);
    if (piRemainFrame) vm_write32(piRemainFrame, (u32)remain_frames(s));
    if (s->pos >= (looping(s) ? s->t.loop_end : s->t.end)) {
        if (!looping(s)) {
            if (puiFinishflag) vm_write32(puiFinishflag, 1);
            return (s32)CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED;
        }
        s->pos = s->t.loop_start;
        if (s->loop_num > 0) s->loop_num--;
    }
    const s32 limit = looping(s) ? s->t.loop_end : s->t.end;
    const s32 k = s->pos / AT3P_SAMPLES, at = s->pos - k * AT3P_SAMPLES;
    s32 n = AT3P_SAMPLES - at;
    if (n > limit - s->pos) n = limit - s->pos;
    const int rc = decode_frame(s, k);
    if (rc) {
        atrac_diag(s, "decode_error", &s->diag_error, rc, 0);
        return rc;
    }
    const u32 ch = s->t.channels;
    for (s32 i = 0; i < n; i++)
        for (u32 c = 0; c < ch; c++) {
            u32 bits;
            memcpy(&bits, &s->pcm[c][at + i], 4);
            vm_write32(pfOutAddr + ((u32)i * ch + c) * 4, bits);
        }
    s->pos += n;
    atrac_identity_pcm(s, pfOutAddr, (u32)n);
    atrac_diag(s, "decode_ok", &s->diag_decode, 0, (u32)n);
    if (puiSamples) vm_write32(puiSamples, (u32)n);
    if (puiFinishflag) vm_write32(puiFinishflag, !looping(s) && s->pos >= s->t.end);
    if (piRemainFrame) vm_write32(piRemainFrame, (u32)remain_frames(s));
    return CELL_OK;
}

s32 cellAtracGetRemainFrame(u32 pHandle, u32 piRemainFrame)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (piRemainFrame) vm_write32(piRemainFrame, (u32)remain_frames(s));
    return CELL_OK;
}

/* The SDK returns vacant compressed-input BYTES through a guest u32*.
 * Our append cache owns all previously submitted bytes, so the staging buffer
 * can accept up to its full capacity, capped by the unsupplied file tail.
 * Querying vacancy does not grant an AddStreamData offer or advance playback. */
s32 cellAtracGetVacantSize(u32 pHandle, u32 puiVacantSize)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const u32 left = s->total - s->loaded;
    if (puiVacantSize) vm_write32(puiVacantSize,
        left < s->buffer_bytes ? left : s->buffer_bytes);
    return CELL_OK;
}

s32 cellAtracGetStreamDataInfo(u32 pHandle, u32 ppucWritePointer,
                               u32 puiWritableByte, u32 puiReadPosition)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const u32 left = s->total - s->loaded;
    s->offered = left < s->buffer_bytes ? left : s->buffer_bytes;
    /* The initial bytes have already been copied to owned storage. The whole
     * guest staging buffer may therefore be reused without corrupting frames. */
    if (ppucWritePointer) vm_write32(ppucWritePointer, s->buffer_ea);
    if (puiWritableByte) vm_write32(puiWritableByte, s->offered);
    if (puiReadPosition) vm_write32(puiReadPosition, s->loaded);
    atrac_diag(s, "stream_info", &s->diag_info, 0, s->offered);
    return CELL_OK;
}

s32 cellAtracAddStreamData(u32 pHandle, u32 uiAddByte)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (uiAddByte > s->offered || uiAddByte > s->total - s->loaded)
        return (s32)CELL_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    memcpy(s->file + s->loaded, vm_base + s->buffer_ea, uiAddByte);
    s->loaded += uiAddByte;
    atrac_identity_full(s);
    s->offered = 0;
    atrac_diag(s, "stream_add", &s->diag_add, 0, uiAddByte);
    return CELL_OK;
}

s32 cellAtracGetChannel(u32 pHandle, u32 puiChannel)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    vm_write32(puiChannel, s->t.channels);
    return CELL_OK;
}

s32 cellAtracGetMaxSample(u32 pHandle, u32 puiMaxSample)
{
    if (!slot_of(pHandle)) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    vm_write32(puiMaxSample, AT3P_SAMPLES);
    return CELL_OK;
}

/* The API counts samples from the first audible one. */
s32 cellAtracGetNextSample(u32 pHandle, u32 puiNextSample)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const s32 k = s->pos / AT3P_SAMPLES;
    vm_write32(puiNextSample, (u32)(AT3P_SAMPLES - (s->pos - k * AT3P_SAMPLES)));
    return CELL_OK;
}

s32 cellAtracGetSoundInfo(u32 pHandle, u32 piEndSample, u32 piLoopStartSample,
                          u32 piLoopEndSample)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const AtracTrackInfo* t = &s->t;
    vm_write32(piEndSample, (u32)(t->end - t->first - 1));
    vm_write32(piLoopStartSample, (u32)(t->loop_start < 0 ? -1 : t->loop_start - t->first));
    vm_write32(piLoopEndSample, (u32)(t->loop_end < 0 ? -1 : t->loop_end - t->first - 1));
    return CELL_OK;
}

s32 cellAtracGetNextDecodePosition(u32 pHandle, u32 puiSamplePosition)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->pos >= s->t.end) return (s32)CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED;
    vm_write32(puiSamplePosition, (u32)(s->pos - s->t.first));
    return CELL_OK;
}

s32 cellAtracGetBitrate(u32 pHandle, u32 puiBitrate)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    /* kbps: block_align bytes per 2048 samples at 48 kHz. */
    vm_write32(puiBitrate, s->t.block_align * 8u * 48000u / AT3P_SAMPLES / 1000u);
    return CELL_OK;
}

s32 cellAtracGetLoopInfo(u32 pHandle, u32 piLoopNum, u32 puiLoopStatus)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    vm_write32(piLoopNum, (u32)s->loop_num);
    vm_write32(puiLoopStatus, s->t.loop_start >= 0);
    return CELL_OK;
}

s32 cellAtracSetLoopNum(u32 pHandle, s32 iLoopNum)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->t.loop_start < 0 && iLoopNum != 0) return (s32)CELL_ATRAC_ERROR_UNSET_LOOP_NUM;
    s->loop_num = iLoopNum;
    return CELL_OK;
}

s32 cellAtracResetPlayPosition(u32 pHandle, u32 uiSample, u32 uiWriteByte)
{
    (void)uiWriteByte;
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const s32 p = s->t.first + (s32)uiSample;
    if (p >= s->t.end) return (s32)CELL_ATRAC_ERROR_ILLEGAL_SAMPLE;
    s->pos = p;
    return CELL_OK;
}
