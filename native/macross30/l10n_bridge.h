#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Register exact guest-context handlers after generic SDK registration. */
void macross_l10n_register(void);
void macross_l10n_install(void);
/* Explicit byte lengths, no implicit terminator. src_len is never modified.
 * dst==NULL measures required bytes. Exhaustion writes nothing and preserves
 * caller capacity, matching RPCS3's Windows generic conversion contract. */
int32_t macross_l10n_convert_bytes(uint32_t source_code, uint32_t target_code,
    const unsigned char* source, uint32_t source_bytes, unsigned char* target,
    uint32_t target_capacity, uint32_t* target_bytes);
#ifdef __cplusplus
}
#endif
