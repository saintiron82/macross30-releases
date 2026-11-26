#include <atomic>
#include "macross_fps_limit.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint32_t vm_read32(uint64_t);
extern "C" bool macross_runtime_has_measured_step(void);

namespace {
std::atomic<int> fps_mode{MACROSS_FPS_30};
}

extern "C" unsigned macross_runtime_frame_vblanks(void)
{
    return macross_fps_mode_vblanks(fps_mode.load(std::memory_order_acquire));
}

extern "C" int macross_runtime_set_fps_unlocked(int unlocked)
{
    const int requested_mode = unlocked == MACROSS_FPS_UNLIMITED || unlocked == MACROSS_FPS_60 ? unlocked : MACROSS_FPS_30;
    const char* dt = std::getenv("MACROSS_DT_STEP");
    const bool original = dt && std::strcmp(dt, "0") == 0;
    bool compatible = true;
    if (requested_mode != MACROSS_FPS_30 && !original) {
        compatible = vm_read32(0x005590D0u) == 0x912B0178u &&
            vm_read32(0x005590D4u) == 0x800B0178u &&
            vm_read32(0x005590D8u) == 0x2B800001u &&
            vm_read32(0x005590DCu) == 0x409D000Cu &&
            macross_runtime_has_measured_step();
    }
    const int enabled = !original && compatible ? requested_mode : MACROSS_FPS_30;
    fps_mode.store(enabled, std::memory_order_release);
    std::fprintf(stderr, "[runtime-options] fps_limit=%d effective_vblanks=%u measured=%d compatible=%d\n",
                 macross_fps_mode_to_limit(enabled), macross_fps_mode_vblanks(enabled), int(!original), int(compatible));
    return enabled;
}
