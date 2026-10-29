/*
 * ps3recomp - cellPngEnc HLE implementation
 *
 * The libpngenc calls a title actually makes: Open, WaitForInput,
 * EncodePicture, WaitForOutput, GetStreamInfo, Reset, Close (+ QueryAttr).
 * The earlier version implemented an invented Create/Encode/Destroy API, so
 * none of these were registered: every call fell through to the "unresolved
 * import -> CELL_OK" stub and did nothing. Macross 30 builds the save icon
 * (ICON0.PNG) this way; it then read a stream info nobody wrote -- a NULL
 * stream address and a stale code address as the size -- and the save flow
 * went wrong (a broken icon write, and the next save in the same battle never
 * opening).
 *
 * Encoding is synchronous inside EncodePicture (stb_image_write); the result
 * waits in a small per-handle queue for WaitForOutput / GetStreamInfo, which
 * is the order the API promises. Output to a guest buffer is supported; file
 * output reports CELL_PNGENC_ERROR_STREAM_FILE_OPEN in the stream info.
 */

#include "cellPngEnc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_read*/vm_write*, vm_ptr8 */

#define STB_IMAGE_WRITE_IMPLEMENTATION   /* also serves cellJpgEnc.c */
#define STBIW_ASSERT(x)
#include "stb_image_write.h"

#define PNGENC_HANDLE_BASE 0x50EC0100u   /* handle values given to the guest */

typedef struct {
    s32 state;
    u32 location, file_name, stream_addr, limit_size, stream_size, processed_line;
    u64 user_data;
} PngEncStream;

typedef struct {
    int in_use;
    u32 max_w, max_h;
    PngEncStream queue[CELL_PNGENC_QUEUE_DEPTH];
    unsigned head, count;
} PngEncHandle;

static PngEncHandle s_handles[CELL_PNGENC_HANDLE_MAX];

static PngEncHandle* pngenc_get(u32 handle)
{
    const u32 i = handle - PNGENC_HANDLE_BASE;
    return i < CELL_PNGENC_HANDLE_MAX && s_handles[i].in_use ? &s_handles[i] : NULL;
}

typedef struct { u8* data; u32 size, capacity; int failed; } PngWriteContext;

static void png_write_callback(void* context, void* data, int size)
{
    PngWriteContext* ctx = (PngWriteContext*)context;
    if (ctx->failed) return;
    if (ctx->size + (u32)size > ctx->capacity) {
        u32 cap = ctx->capacity * 2 + (u32)size + 4096u;
        u8* grown = (u8*)realloc(ctx->data, cap);
        if (!grown) { ctx->failed = 1; return; }
        ctx->data = grown; ctx->capacity = cap;
    }
    memcpy(ctx->data + ctx->size, data, (size_t)size);
    ctx->size += (u32)size;
}

static int pngenc_channels(u32 color_space)
{
    switch (color_space) {
    case CELL_PNGENC_COLOR_SPACE_GRAYSCALE:       return 1;
    case CELL_PNGENC_COLOR_SPACE_GRAYSCALE_ALPHA: return 2;
    case CELL_PNGENC_COLOR_SPACE_RGB:             return 3;
    case CELL_PNGENC_COLOR_SPACE_RGBA:
    case CELL_PNGENC_COLOR_SPACE_ARGB:            return 4;
    default:                                      return 0;
    }
}

s32 cellPngEncQueryAttr(u32 config, u32 attr)
{
    if (!config || !attr) return (s32)CELL_PNGENC_ERROR_ARG;
    const u32 w = vm_read32(config + 0), h = vm_read32(config + 4);
    /* Work memory the title must hand to Open: the encoder keeps nothing in it
     * here, but report a plausible size for the configured maximum picture. */
    const u32 mem = 0x10000u + (w && h && w <= 8192 && h <= 8192 ? w * 4u * 2u : 0x8000u);
    vm_write32(attr + 0, mem);
    vm_ptr8(attr + 4)[0] = (u8)CELL_PNGENC_QUEUE_DEPTH;   /* cmdQueueDepth */
    vm_write32(attr + 8, 0x00010000u);                   /* versionUpper */
    vm_write32(attr + 12, 0);                            /* versionLower */
    printf("[cellPngEnc] QueryAttr max=%ux%u -> memSize=%u\n", w, h, mem);
    return CELL_OK;
}

s32 cellPngEncOpen(u32 config, u32 resource, u32 handle)
{
    if (!config || !resource || !handle) return (s32)CELL_PNGENC_ERROR_ARG;
    for (u32 i = 0; i < CELL_PNGENC_HANDLE_MAX; i++) {
        if (s_handles[i].in_use) continue;
        memset(&s_handles[i], 0, sizeof s_handles[i]);
        s_handles[i].in_use = 1;
        s_handles[i].max_w = vm_read32(config + 0);
        s_handles[i].max_h = vm_read32(config + 4);
        vm_write32(handle, PNGENC_HANDLE_BASE + i);
        printf("[cellPngEnc] Open max=%ux%u -> handle %08X\n", s_handles[i].max_w, s_handles[i].max_h,
               PNGENC_HANDLE_BASE + i);
        return CELL_OK;
    }
    return (s32)CELL_PNGENC_ERROR_FATAL;
}

s32 cellPngEncOpenEx(u32 config, u32 resource, u32 handle)
{
    return cellPngEncOpen(config, resource, handle);
}

s32 cellPngEncClose(u32 handle)
{
    PngEncHandle* h = pngenc_get(handle);
    if (!h) return (s32)CELL_PNGENC_ERROR_ARG;
    printf("[cellPngEnc] Close %08X\n", handle);
    h->in_use = 0;
    return CELL_OK;
}

s32 cellPngEncWaitForInput(u32 handle, u32 block)
{
    (void)block;
    PngEncHandle* h = pngenc_get(handle);
    if (!h) return (s32)CELL_PNGENC_ERROR_ARG;
    /* Encoding happens inside EncodePicture, so the input side is free unless
     * finished results are still waiting to be collected. */
    return h->count < CELL_PNGENC_QUEUE_DEPTH ? CELL_OK : (s32)CELL_PNGENC_ERROR_BUSY;
}

s32 cellPngEncEncodePicture(u32 handle, u32 picture, u32 encode_param, u32 output_param)
{
    PngEncHandle* h = pngenc_get(handle);
    if (!h || !picture || !encode_param || !output_param) return (s32)CELL_PNGENC_ERROR_ARG;
    if (h->count >= CELL_PNGENC_QUEUE_DEPTH) return (s32)CELL_PNGENC_ERROR_BUSY;

    const u32 w = vm_read32(picture + 0), ph = vm_read32(picture + 4), pitch = vm_read32(picture + 8);
    const u32 in_cs = vm_read32(picture + 12), depth = vm_read32(picture + 16);
    const u32 pixels = vm_read32(picture + 24);
    const u64 user = vm_read64(picture + 32);
    const u32 out_cs = vm_read32(encode_param + 4), level = vm_read32(encode_param + 8);
    const u32 location = vm_read32(output_param + 0), file_name = vm_read32(output_param + 4);
    const u32 stream_addr = vm_read32(output_param + 8), limit = vm_read32(output_param + 12);

    PngEncStream st;
    memset(&st, 0, sizeof st);
    st.location = location; st.file_name = file_name; st.stream_addr = stream_addr;
    st.limit_size = limit; st.user_data = user;

    const int in_ch = pngenc_channels(in_cs);
    int out_ch = pngenc_channels(out_cs);
    if (!out_ch || out_cs == CELL_PNGENC_COLOR_SPACE_ARGB) out_ch = in_ch == 4 ? 4 : in_ch;
    if (!w || !ph || !pixels || !in_ch || depth != 8 || pitch < w * (u32)in_ch || w > 8192 || ph > 8192) {
        printf("[cellPngEnc] EncodePicture rejected: %ux%u pitch=%u cs=%u depth=%u pixels=%08X\n",
               w, ph, pitch, in_cs, depth, pixels);
        return (s32)CELL_PNGENC_ERROR_ARG;
    }

    /* Repack the guest rows into tightly packed output channels. */
    u8* img = (u8*)malloc((size_t)w * ph * (size_t)out_ch);
    if (!img) return (s32)CELL_PNGENC_ERROR_FATAL;
    unsigned long long sum = 0;
    for (u32 y = 0; y < ph; y++) {
        const u8* src = vm_ptr8(pixels + y * pitch);
        u8* dst = img + (size_t)y * w * (size_t)out_ch;
        for (u32 x = 0; x < w; x++) {
            const u8* p = src + (size_t)x * (size_t)in_ch;
            u8 r, g, b, a;
            switch (in_cs) {
            case CELL_PNGENC_COLOR_SPACE_ARGB: a = p[0]; r = p[1]; g = p[2]; b = p[3]; break;
            case CELL_PNGENC_COLOR_SPACE_RGBA: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
            case CELL_PNGENC_COLOR_SPACE_RGB:  r = p[0]; g = p[1]; b = p[2]; a = 255; break;
            case CELL_PNGENC_COLOR_SPACE_GRAYSCALE_ALPHA: r = g = b = p[0]; a = p[1]; break;
            default: r = g = b = p[0]; a = 255; break;
            }
            sum += (unsigned)r + g + b;
            u8* d = dst + (size_t)x * (size_t)out_ch;
            if (out_ch == 4) { d[0] = r; d[1] = g; d[2] = b; d[3] = a; }
            else if (out_ch == 3) { d[0] = r; d[1] = g; d[2] = b; }
            else if (out_ch == 2) { d[0] = (u8)((r + g + b) / 3); d[1] = a; }
            else d[0] = (u8)((r + g + b) / 3);
        }
    }
    if (level <= 9) stbi_write_png_compression_level = (int)(level ? level : 1);

    PngWriteContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.capacity = w * ph * (u32)out_ch / 2u + 4096u;
    ctx.data = (u8*)malloc(ctx.capacity);
    const int ok = ctx.data && stbi_write_png_to_func(png_write_callback, &ctx, (int)w, (int)ph, out_ch, img,
                                                      (int)(w * (u32)out_ch)) && !ctx.failed;
    free(img);

    if (!ok) {
        st.state = (s32)CELL_PNGENC_ERROR_FATAL;
    } else if (location != CELL_PNGENC_LOCATION_BUFFER) {
        st.state = (s32)CELL_PNGENC_ERROR_STREAM_FILE_OPEN;   /* file output not wired */
    } else if (!stream_addr || ctx.size > limit) {
        st.state = (s32)CELL_PNGENC_ERROR_STREAM_OVERFLOW;
    } else {
        memcpy(vm_ptr8(stream_addr), ctx.data, ctx.size);
        st.state = CELL_OK;
        st.stream_size = ctx.size;
        st.processed_line = ph;
    }
    /* pixels/pitch: where the picture came from (picture+24); a black icon
     * means those guest bytes were never written (save capture). */
    printf("[cellPngEnc] EncodePicture %ux%u cs=%u->%u level=%u pixels=%08X pitch=%u -> %u bytes to %08X (limit %u) state=%08X mean=%llu\n",
           w, ph, in_cs, out_cs, level, pixels, pitch, ok ? ctx.size : 0u, stream_addr, limit, (u32)st.state,
           sum / ((unsigned long long)w * ph * 3u));
    fflush(stdout);
    free(ctx.data);

    h->queue[(h->head + h->count) % CELL_PNGENC_QUEUE_DEPTH] = st;
    h->count++;
    return CELL_OK;
}

s32 cellPngEncWaitForOutput(u32 handle, u32 stream_info_num, u32 block)
{
    PngEncHandle* h = pngenc_get(handle);
    if (!h || !stream_info_num) return (s32)CELL_PNGENC_ERROR_ARG;
    /* Results are ready as soon as EncodePicture returns; with nothing queued
     * a blocking wait would never end, so report EMPTY either way. */
    vm_write32(stream_info_num, h->count);
    (void)block;
    return h->count ? CELL_OK : (s32)CELL_PNGENC_ERROR_EMPTY;
}

s32 cellPngEncGetStreamInfo(u32 handle, u32 stream_info)
{
    PngEncHandle* h = pngenc_get(handle);
    if (!h || !stream_info) return (s32)CELL_PNGENC_ERROR_ARG;
    if (!h->count) return (s32)CELL_PNGENC_ERROR_EMPTY;
    const PngEncStream* st = &h->queue[h->head];
    vm_write32(stream_info + 0, (u32)st->state);
    vm_write32(stream_info + 4, st->location);
    vm_write32(stream_info + 8, st->file_name);
    vm_write32(stream_info + 12, st->stream_addr);
    vm_write32(stream_info + 16, st->limit_size);
    vm_write32(stream_info + 20, st->stream_size);
    vm_write32(stream_info + 24, st->processed_line);
    vm_write32(stream_info + 28, 0);
    vm_write64(stream_info + 32, st->user_data);
    h->head = (h->head + 1) % CELL_PNGENC_QUEUE_DEPTH;
    h->count--;
    return CELL_OK;
}

s32 cellPngEncReset(u32 handle)
{
    PngEncHandle* h = pngenc_get(handle);
    if (!h) return (s32)CELL_PNGENC_ERROR_ARG;
    h->head = h->count = 0;
    return CELL_OK;
}
