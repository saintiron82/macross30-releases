#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void macross_lua_precall_probe(const void* context, uint32_t state,
                               uint64_t lr, uint64_t toc);
void macross_lua_protected_probe(const void* context, uint32_t state,
    uint32_t jump_buffer, uint64_t result, uint64_t stack, uint64_t lr,
    uint64_t toc, int after_setjmp);
void macross_lua_error_probe(const void* context, uint32_t state,
    uint32_t status, uint64_t stack, uint64_t lr);
void macross_lua_obj_probe(const void* context, int phase, uint32_t output,
    uint32_t name, uint32_t field, uint32_t entity, int32_t fieldenum);
void macross_lua_pilot_probe(const void* context, uint32_t output, uint32_t id,
    uint32_t field, uint32_t toc);
void macross_entity_probe(const void* context, uint32_t stage,
    uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7,
    uint64_t lr, uint64_t toc);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
struct MacrossPilotProbe {
    const void* context;
    uint32_t output, id, field, toc;
    ~MacrossPilotProbe() { macross_lua_pilot_probe(context, output, id, field, toc); }
};
#endif
