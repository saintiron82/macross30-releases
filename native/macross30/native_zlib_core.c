/* Edge zlib inflate element decode on host buffers (see native_zlib.h).
 *
 * The inflater is stb_image's zlib decoder (public domain / MIT, vendored at
 * vendor/ps3recomp-master/libs/codec/stb_image.h, licence text in that file).
 * It is compiled here with static linkage and only the zlib part enabled, so
 * it does not collide with the copy cellPngDec.c builds. */
#include "native_zlib.h"
#include <string.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_JPEG
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_SUPPORT_ZLIB
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4505)
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

void macross_zlib_element_parse(const uint8_t raw[32], macross_zlib_element* e)
{
    e->ea_input = be32(raw);
    e->ea_output = be32(raw + 4);
    e->input_size = be32(raw + 8);
    e->output_size = be32(raw + 12);
    const uint32_t w4 = be32(raw + 16);
    e->ea_counter = w4 & ~1u;          /* 0x3220: andi r5,r86,-2 */
    e->deflate = (int)(w4 & 1u);       /* 0x3298: andi r33,r80,1 */
    e->ea_event_flag = be32(raw + 20);
    e->event_bits = be16(raw + 24);    /* 0x3220: rotqbyi r83,r86,6 */
    e->skip_begin = be16(raw + 26);    /* 0x3158: rotqbyi 8 & 0xFFFF */
    e->skip_end = be16(raw + 28);      /* 0x3158: rotqbyi 10 & 0xFFFF */
}

uint64_t macross_zlib_element_total(const macross_zlib_element* e)
{
    const uint64_t total = (uint64_t)e->skip_begin + e->output_size + e->skip_end;
    return total > 0xFFFFFFFFull ? 0 : total;
}

int macross_raw_inflate(uint8_t* out, uint32_t out_cap, const uint8_t* in, uint32_t in_size)
{
    if (out_cap > 0x7FFFFFFFu || in_size > 0x7FFFFFFFu) return -1;
    return stbi_zlib_decode_noheader_buffer((char*)out, (int)out_cap, (const char*)in, (int)in_size);
}

int macross_zlib_element_decode(const macross_zlib_element* e, const uint8_t* input,
                                uint8_t* scratch, const uint8_t** out, uint32_t* out_len)
{
    const uint64_t total64 = macross_zlib_element_total(e);
    /* 0x3158: skip_begin + skip_end > skip_begin + size + skip_end -> 0x3340 error. */
    if (!total64 && (e->skip_begin || e->output_size || e->skip_end)) return -1;
    const uint32_t total = (uint32_t)total64;
    if (!e->input_size) return -4;
    /* 0x3158: either size above 64 KiB takes edgeZlibFetchAndInflateLargeRawData
     * (0x35C8), which always inflates and streams the WHOLE stream to the
     * output EA -- the skip sizes and the stored flag are not consulted. */
    const int large = total > 0x10000u || e->input_size > 0x10000u;
    if (!large && !e->deflate) {
        /* 0x3298: stored data must be exactly the decoded size, else 0x3328. */
        if (e->input_size != total) return -2;
        *out = input + e->skip_begin;
        *out_len = e->output_size;
        return 0;
    }
    /* edgeZlibInflateRawData (0x7018): inflateInit2(-15), inflate(Z_FINISH)
     * must reach Z_STREAM_END and total_out must equal the expected size.
     * Trailing input after the final block is ignored there and here. */
    const int n = macross_raw_inflate(scratch, total, input, e->input_size);
    if (n < 0 || (uint32_t)n != total) return -3;
    if (large) {
        *out = scratch;
        *out_len = total;
    } else {
        *out = scratch + e->skip_begin;
        *out_len = e->output_size;
    }
    return 0;
}
