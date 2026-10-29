/* Whole-span mutation detector. xxHash v0.8.3 is unmodified upstream code;
 * its BSD-2-Clause notice/provenance live in third_party/xxhash.h. */
#ifndef RSX_TEXTURE_HASH_H
#define RSX_TEXTURE_HASH_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define XXH_INLINE_ALL
#if defined(_M_X64) || defined(__x86_64__)
/* x64 SSE2 baseline; do not make AVX2/AVX512 a runtime requirement. */
#define XXH_VECTOR XXH_SSE2
#endif
#include "third_party/xxhash.h"
enum { RSX_TEXTURE_HASH_FNV=0,RSX_TEXTURE_HASH_XXH3=1 };
static inline int rsx_texture_hash_parse_mode(const char* value){
    return value&&!strcmp(value,"xxh3")?RSX_TEXTURE_HASH_XXH3:RSX_TEXTURE_HASH_FNV;
}
static inline uint64_t rsx_texture_hash_bytes(const void* data,size_t span,int mode){
    if(mode==RSX_TEXTURE_HASH_XXH3)return XXH3_64bits(data,span);
    /* Preserve the original native-word FNV detector bit-for-bit. */
    const unsigned char* src=(const unsigned char*)data;
    uint64_t hash=UINT64_C(1469598103934665603);
    size_t i=0;
    for(;i+8<=span;i+=8){uint64_t word;memcpy(&word,src+i,sizeof(word));hash^=word;hash*=UINT64_C(1099511628211);}
    for(;i<span;i++){hash^=src[i];hash*=UINT64_C(1099511628211);}
    return hash;
}
#endif
