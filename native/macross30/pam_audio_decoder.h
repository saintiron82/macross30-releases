#ifndef MACROSS_PAM_AUDIO_DECODER_H
#define MACROSS_PAM_AUDIO_DECODER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MacrossPamAudioDecoder MacrossPamAudioDecoder;
typedef struct MacrossPamAudioFrame {
 const float* interleaved; /* Decoder-owned until next pull/close. */
 uint32_t samples_per_channel, channels, sample_rate, frame_index;
 int64_t pts90k;
 int32_t pts_valid;
} MacrossPamAudioFrame;
/* Single-thread owned; reads original PAMF incrementally, never a PCM sidecar. */
MacrossPamAudioDecoder* macross_pam_audio_open(const char* path,char* error,size_t capacity);
/* 1 actual decoded AU, 0 drained EOF, -1 sticky error. */
int macross_pam_audio_next_frame(MacrossPamAudioDecoder*,MacrossPamAudioFrame*);
const char* macross_pam_audio_last_error(const MacrossPamAudioDecoder*);
void macross_pam_audio_close(MacrossPamAudioDecoder*);
#ifdef __cplusplus
}
#endif
#endif
