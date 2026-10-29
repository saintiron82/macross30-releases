#pragma once
#include <stdint.h>

extern "C" int rsx_null_backend_subtitles_enabled(uint32_t* epoch);

// Caption rendering only. Guest queue, lifetime, audio and face updates continue
// while hidden; the next draw reads the same atomic option as movie subtitles.
inline bool macross_caption_visible() {
    return rsx_null_backend_subtitles_enabled(nullptr) != 0;
}
