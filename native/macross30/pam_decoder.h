#ifndef MACROSS_PAM_DECODER_H
#define MACROSS_PAM_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MacrossPamDecoder MacrossPamDecoder;

typedef struct MacrossPamFrame {
    /* Contiguous I420: width*height Y, ceil(width/2)*ceil(height/2) U, then V.
     * Owned by the decoder; valid until the next next_frame call or close.
     * Copy into the guest's frame buffer before requesting another frame. */
    const uint8_t* i420;
    size_t size;
    uint32_t width;
    uint32_t height;
    int64_t pts;             /* MPEG/PAMF stream ticks, time base 1/90000. */
    int32_t pts_valid;
    uint32_t frame_index;
} MacrossPamFrame;

/* Paths are UTF-8. dll_directory names the directory containing the existing
 * avutil-61.dll, avcodec-63.dll and avformat-63.dll plus their dependencies.
 * This prototype accepts exactly FFmpeg 9.0.1 and the reviewed ABI versions.
 * Returns NULL on failure and writes a diagnostic into error when supplied.
 * A decoder is single-thread owned; separate decoders may run independently. */
MacrossPamDecoder* macross_pam_open(const char* media_path, const char* dll_directory,
                                  char* error, size_t error_capacity);

/* 1 = decoded frame, 0 = genuine drained EOF, -1 = error. No synthetic frames
 * or EOF are generated. A failed decoder stays failed until closed. */
int macross_pam_next_frame(MacrossPamDecoder* decoder, MacrossPamFrame* frame);
const char* macross_pam_last_error(const MacrossPamDecoder* decoder);
void macross_pam_close(MacrossPamDecoder* decoder);

#ifdef __cplusplus
}
#endif
#endif
