/*
 * ps3recomp - cellPngEnc HLE
 *
 * The real libpngenc interface: Open -> (WaitForInput) -> EncodePicture ->
 * WaitForOutput -> GetStreamInfo -> Close. Encoding is done synchronously in
 * EncodePicture; the queue only hands the result back in the order the API
 * promises. Guest structures are big-endian; the offsets below are their
 * layout in guest memory.
 */

#ifndef PS3RECOMP_CELL_PNGENC_H
#define PS3RECOMP_CELL_PNGENC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_PNGENC_ERROR_ARG               0x80611291
#define CELL_PNGENC_ERROR_SEQ               0x80611292
#define CELL_PNGENC_ERROR_BUSY              0x80611293
#define CELL_PNGENC_ERROR_EMPTY             0x80611294
#define CELL_PNGENC_ERROR_RESET             0x80611295
#define CELL_PNGENC_ERROR_FATAL             0x80611296
#define CELL_PNGENC_ERROR_STREAM_ABORT      0x806112A1
#define CELL_PNGENC_ERROR_STREAM_SKIP       0x806112A2
#define CELL_PNGENC_ERROR_STREAM_OVERFLOW   0x806112A3
#define CELL_PNGENC_ERROR_STREAM_FILE_OPEN  0x806112A4

/* CellPngEncColorSpace */
#define CELL_PNGENC_COLOR_SPACE_GRAYSCALE        1
#define CELL_PNGENC_COLOR_SPACE_RGB              2
#define CELL_PNGENC_COLOR_SPACE_PALETTE          4
#define CELL_PNGENC_COLOR_SPACE_GRAYSCALE_ALPHA  5
#define CELL_PNGENC_COLOR_SPACE_RGBA             6
#define CELL_PNGENC_COLOR_SPACE_ARGB             20

/* CellPngEncLocation */
#define CELL_PNGENC_LOCATION_FILE    0
#define CELL_PNGENC_LOCATION_BUFFER  1

#define CELL_PNGENC_HANDLE_MAX  4
#define CELL_PNGENC_QUEUE_DEPTH 4

/* Guest layouts (byte offsets).
 * CellPngEncConfig     { u32 maxWidth @0, maxHeight @4, maxBitDepth @8; b8 enableSpu @12;
 *                        u32 addMemSize @16; ptr exParamList @20; u32 exParamNum @24 }
 * CellPngEncAttr       { u32 memSize @0; u8 cmdQueueDepth @4; u32 versionUpper @8, versionLower @12 }
 * CellPngEncResource   { ptr memAddr @0; u32 memSize @4; s32 ppuThreadPriority @8, spuThreadPriority @12 }
 * CellPngEncPicture    { u32 width @0, height @4, pitchWidth @8, colorSpace @12, bitDepth @16;
 *                        b8 packedPixel @20; ptr pictureAddr @24; u64 userData @32 }
 * CellPngEncEncodeParam{ b8 enableSpu @0; u32 encodeColorSpace @4, compressionLevel @8, filterType @12;
 *                        ptr ancillaryChunkList @16; u32 ancillaryChunkNum @20 }
 * CellPngEncOutputParam{ u32 location @0; ptr streamFileName @4; ptr streamAddr @8; u32 limitSize @12 }
 * CellPngEncStreamInfo { s32 state @0; u32 location @4; ptr streamFileName @8; ptr streamAddr @12;
 *                        u32 limitSize @16, streamSize @20, processedLine @24; u64 userData @32 } */

/* Functions (pointer arguments are guest addresses) */
s32 cellPngEncQueryAttr(u32 config, u32 attr);
s32 cellPngEncOpen(u32 config, u32 resource, u32 handle);
s32 cellPngEncOpenEx(u32 config, u32 resource, u32 handle);
s32 cellPngEncClose(u32 handle);
s32 cellPngEncWaitForInput(u32 handle, u32 block);
s32 cellPngEncEncodePicture(u32 handle, u32 picture, u32 encode_param, u32 output_param);
s32 cellPngEncWaitForOutput(u32 handle, u32 stream_info_num, u32 block);
s32 cellPngEncGetStreamInfo(u32 handle, u32 stream_info);
s32 cellPngEncReset(u32 handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PNGENC_H */
