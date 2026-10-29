#ifndef MACROSS_SPU_FUNCTION_PERF_H
#define MACROSS_SPU_FUNCTION_PERF_H
#include <stdint.h>
struct spu_context;
typedef struct macross_spu_function_scope {
    uint64_t wall_start, cpu_start;
    uint32_t bucket;
    unsigned sampled, cpu_valid;
} macross_spu_function_scope;
#ifdef __cplusplus
extern "C" {
#endif
macross_spu_function_scope macross_spu_function_begin(const struct spu_context*, uint32_t pc);
void macross_spu_function_end(macross_spu_function_scope*);
#ifdef __cplusplus
}
#endif
#if defined(MACROSS_SPU_FUNCTION_PROFILE)
#if !defined(__clang__)
#error "SPU function profiling requires Clang cleanup support"
#endif
/* Declaration at function scope: a do/while wrapper would end it immediately.
 * No musttail calls may coexist with a live cleanup scope. */
#define MACROSS_SPU_PERF_SCOPE(ctx,pc) \
    macross_spu_function_scope _macross_spu_perf_scope \
    __attribute__((cleanup(macross_spu_function_end))) = macross_spu_function_begin((ctx),(pc))
#else
#define MACROSS_SPU_PERF_SCOPE(ctx,pc) do { } while (0)
#endif
#endif
