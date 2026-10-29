/* SPU post-effects whose output is never shown once the renderer runs the
 * post-process on the GPU (internal render scale): the Edge MLAA task's
 * compute kernels (tools/build/patch_spu_postfx_skip.py) and the posttask bloom
 * chain (spu_posttask_native/guard.c). The renderer stamps g_pp_gpu_last_ms
 * each time it applies the GPU post-process; within the last second, skip. */
#include <windows.h>

extern volatile unsigned long g_pp_gpu_last_ms;

int macross_spu_postfx_skip(void)
{
    const ULONGLONG now = GetTickCount64();
    if (!g_pp_gpu_last_ms) return 0;
    return (unsigned long)now - g_pp_gpu_last_ms <= 1000u;
}
