/* Immutable shader analysis, render-thread owned. No D3D/resource ownership.
 *
 * Address hints NEVER establish a hit: current instruction bytes are compared
 * exactly. FP inline payloads alone are excluded, using offsets returned by the
 * original FP walker. Current payload values are read/sanitized for every draw.
 *
 * Acquire pins an entry. Release it on every caller exit; pinned entries cannot
 * be evicted. Capacity/allocation/malformed failures return NULL: callers must
 * use their original parser path. No shader key/render-state policy is changed.
 * Source memory must be validated for available_bytes and stable during each
 * call (or captured in an immutable draw packet). This is not a guest-memory
 * synchronization mechanism. A hint size is only a range-validation hint.
 */
#ifndef PS3RECOMP_RSX_SHADER_METADATA_CACHE_H
#define PS3RECOMP_RSX_SHADER_METADATA_CACHE_H
#include <stdlib.h>
#include <string.h>
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"

#ifndef RSX_SMC_ENTRIES
#define RSX_SMC_ENTRIES 128u
#endif
#ifndef RSX_SMC_HINTS
#define RSX_SMC_HINTS 512u
#endif
#define RSX_SMC_MAX_BYTES 65536u
#define RSX_SMC_DEFAULT_BUDGET (4u * 1024u * 1024u)
#define RSX_SMC_VP 1u
#define RSX_SMC_FP 2u

typedef struct rsx_shader_metadata {
    u32 stage, bytes, constant_count, sampled_units, txl_mask;
    rsx_vp_input_analysis inputs;
    u64 content_hash, serial, used;
    u32 pins;
    u8* code;                 /* immutable; FP payload bytes canonicalized to 0 */
    u64 memo_serial, memo_seed, memo_hash;   /* last rsx_smc_fp_hash result */
    u32* constant_offsets;     /* ordered 16-byte payload slots */
} rsx_shader_metadata;
typedef struct rsx_shader_metadata_hint {
    u64 address_key, serial;
    u32 stage, index;
} rsx_shader_metadata_hint;
typedef struct rsx_shader_metadata_cache {
    rsx_shader_metadata entries[RSX_SMC_ENTRIES];
    rsx_shader_metadata_hint hints[RSX_SMC_HINTS];
    size_t owned_bytes, budget;
    u64 clock, serial, hint_hits, content_hits, misses, invalid, evictions;
} rsx_shader_metadata_cache;
typedef u32 (*rsx_smc_vp_txl_fn)(const u8*, u32 instruction_count);

static inline u64 rsx_smc_hash_bytes(const void* p, u32 n, u64 seed)
{
    const u8* b = (const u8*)p;
    for (u32 i=0;i<n;++i) { seed ^= b[i]; seed *= 1099511628211ull; }
    return seed;
}
static inline size_t rsx_smc_owned(const rsx_shader_metadata* e)
{ return e->code ? (size_t)e->bytes + (size_t)e->constant_count * sizeof(u32) : 0; }
static inline void rsx_smc_init(rsx_shader_metadata_cache* c, size_t budget)
{
    /* Only initialize fresh storage; clear existing storage before re-init. */
    memset(c,0,sizeof(*c)); c->budget=budget ? budget : RSX_SMC_DEFAULT_BUDGET;
}
static inline int rsx_smc_clear(rsx_shader_metadata_cache* c)
{
    for (u32 i=0;i<RSX_SMC_ENTRIES;++i) if(c->entries[i].pins) return 0;
    for (u32 i=0;i<RSX_SMC_ENTRIES;++i) {
        free(c->entries[i].code); free(c->entries[i].constant_offsets);
    }
    const size_t budget=c->budget; memset(c,0,sizeof(*c)); c->budget=budget;
    return 1;
}
static inline u32 rsx_smc_hint_index(u32 stage, u64 key)
{
    key ^= key >> 17; key *= 0x9e3779b97f4a7c15ull; key ^= key >> 31;
    return (u32)((key ^ stage) % RSX_SMC_HINTS);
}
static inline rsx_shader_metadata* rsx_smc_hint(
    rsx_shader_metadata_cache* c, u32 stage, u64 key)
{
    const rsx_shader_metadata_hint* h=&c->hints[rsx_smc_hint_index(stage,key)];
    if (!h->serial || h->stage!=stage || h->address_key!=key || h->index>=RSX_SMC_ENTRIES) return NULL;
    rsx_shader_metadata* e=&c->entries[h->index];
    return e->code && e->serial==h->serial && e->stage==stage ? e : NULL;
}
static inline u32 rsx_smc_hint_bytes(rsx_shader_metadata_cache* c,u32 stage,u64 key)
{
    const rsx_shader_metadata* e=rsx_smc_hint(c,stage,key);
    return e ? e->bytes : 0; /* NOT validation: resolve/check that range first. */
}
static inline void rsx_smc_set_hint(rsx_shader_metadata_cache* c,u64 key,rsx_shader_metadata* e)
{
    rsx_shader_metadata_hint* h=&c->hints[rsx_smc_hint_index(e->stage,key)];
    h->address_key=key; h->stage=e->stage; h->serial=e->serial;
    h->index=(u32)(e-c->entries);
}
static inline int rsx_smc_matches(const rsx_shader_metadata* e,const u8* source,u32 available)
{
    if (!e || !e->code || !source || available<e->bytes) return 0;
    if(e->stage==RSX_SMC_VP) return memcmp(e->code,source,e->bytes)==0;
    u32 begin=0;
    for(u32 i=0;i<e->constant_count;++i) {
        const u32 off=e->constant_offsets[i];
        if(memcmp(e->code+begin,source+begin,off-begin)) return 0;
        begin=off+16;
    }
    return memcmp(e->code+begin,source+begin,e->bytes-begin)==0;
}
static inline u64 rsx_smc_fp_hash(const rsx_shader_metadata* e,u64 seed)
{
    if(!e || e->stage!=RSX_SMC_FP) return 0;
    /* The entry is immutable for its serial: the same seed hashes the same. */
    if(e->memo_serial==e->serial && e->memo_seed==seed && e->memo_serial) return e->memo_hash;
    const u64 seed_in=seed;
    const u32 tag=0x31535046u;
    seed=rsx_smc_hash_bytes(&tag,sizeof tag,seed);
    u32 begin=0;
    for(u32 i=0;i<e->constant_count;++i) {
        const u32 off=e->constant_offsets[i];
        seed=rsx_smc_hash_bytes(e->code+begin,off-begin,seed); begin=off+16;
    }
    seed=rsx_smc_hash_bytes(e->code+begin,e->bytes-begin,seed);
    const u32 instructions=e->bytes/16-e->constant_count;
    seed=rsx_smc_hash_bytes(&instructions,sizeof instructions,seed);
    seed=rsx_smc_hash_bytes(&e->constant_count,sizeof e->constant_count,seed);
    rsx_shader_metadata* m=(rsx_shader_metadata*)e;   /* render-thread owned */
    m->memo_serial=e->serial; m->memo_seed=seed_in; m->memo_hash=seed;
    return seed;
}
static inline int rsx_smc_collect_constants(const rsx_shader_metadata* e,
    const u8* current,u32 available,rsx_fp_constant_block* out)
{
    if(!out || !e || e->stage!=RSX_SMC_FP || !rsx_smc_matches(e,current,available)) return -1;
    memset(out,0,sizeof(*out));
    for(u32 i=0;i<e->constant_count;++i) for(u32 component=0;component<4;++component)
        out->values[i][component]=rsx_fp_sanitize_constant_word(
            rsx_fp_read_word(current+e->constant_offsets[i]+component*4));
    out->count=e->constant_count; return (int)e->constant_count;
}
static inline rsx_shader_metadata* rsx_smc_pin(rsx_shader_metadata_cache* c,rsx_shader_metadata* e)
{
    if(e->pins==0xffffffffu) return NULL;
    ++e->pins; e->used=++c->clock; return e;
}
static inline void rsx_smc_release(rsx_shader_metadata_cache* c,const rsx_shader_metadata* entry)
{
    for(u32 i=0;i<RSX_SMC_ENTRIES;++i) if(entry==&c->entries[i]) {
        if(c->entries[i].pins) --c->entries[i].pins; return;
    }
}
static inline rsx_shader_metadata* rsx_smc_oldest(rsx_shader_metadata_cache* c)
{
    rsx_shader_metadata* oldest=NULL;
    for(u32 i=0;i<RSX_SMC_ENTRIES;++i) {
        rsx_shader_metadata* e=&c->entries[i];
        if(e->code && !e->pins && (!oldest || e->used<oldest->used)) oldest=e;
    }
    return oldest;
}
static inline void rsx_smc_evict(rsx_shader_metadata_cache* c,rsx_shader_metadata* e)
{
    c->owned_bytes-=rsx_smc_owned(e); free(e->code); free(e->constant_offsets);
    memset(e,0,sizeof(*e)); ++c->evictions;
}
static inline rsx_shader_metadata* rsx_smc_insert(rsx_shader_metadata_cache* c,
    const rsx_shader_metadata* draft,const u8* source,u64 address_key)
{
    const size_t needed=(size_t)draft->bytes+(size_t)draft->constant_count*sizeof(u32);
    if(needed>c->budget) return NULL;
    u8* code=(u8*)malloc(draft->bytes);
    u32* offsets=draft->constant_count ? (u32*)malloc(draft->constant_count*sizeof(u32)) : NULL;
    if(!code || (draft->constant_count&&!offsets)) { free(code);free(offsets);return NULL; }
    memcpy(code,source,draft->bytes);
    if(offsets) memcpy(offsets,draft->constant_offsets,draft->constant_count*sizeof(u32));
    for(u32 i=0;i<draft->constant_count;++i) memset(code+offsets[i],0,16);
    rsx_shader_metadata* slot=NULL;
    while(c->owned_bytes+needed>c->budget) {
        rsx_shader_metadata* old=rsx_smc_oldest(c);
        if(!old) { free(code);free(offsets);return NULL; } rsx_smc_evict(c,old);
    }
    for(u32 i=0;i<RSX_SMC_ENTRIES;++i) if(!c->entries[i].code) {slot=&c->entries[i];break;}
    if(!slot) { slot=rsx_smc_oldest(c); if(slot)rsx_smc_evict(c,slot); }
    if(!slot) {free(code);free(offsets);return NULL;}
    *slot=*draft;slot->code=code;slot->constant_offsets=offsets;slot->serial=++c->serial;slot->pins=0;
    c->owned_bytes+=needed;rsx_smc_set_hint(c,address_key,slot);return rsx_smc_pin(c,slot);
}
static inline rsx_shader_metadata* rsx_smc_acquire(rsx_shader_metadata_cache* c,u32 stage,
    u64 address_key,const u8* source,u32 available,rsx_smc_vp_txl_fn txl)
{
    if(!c || !source || !available || available>RSX_SMC_MAX_BYTES ||
       (stage!=RSX_SMC_FP&&stage!=RSX_SMC_VP) || (stage==RSX_SMC_VP&&!txl)) return NULL;
    rsx_shader_metadata* hint=rsx_smc_hint(c,stage,address_key);
    if(rsx_smc_matches(hint,source,available)) {++c->hint_hits;return rsx_smc_pin(c,hint);}
    /* Miss work runs against a private snapshot, not a mutable guest pointer. */
    u8* snapshot=(u8*)malloc(available);
    if(!snapshot) return NULL;memcpy(snapshot,source,available);
    rsx_shader_metadata draft;memset(&draft,0,sizeof draft);draft.stage=stage;
    u32 offsets[RSX_FP_MAX_INLINE_CONSTANTS];
    if(stage==RSX_SMC_FP) {
        const int n=rsx_fp_collect_constant_offsets(snapshot,available,offsets,
            RSX_FP_MAX_INLINE_CONSTANTS,&draft.bytes);
        if(n<0) {free(snapshot);++c->invalid;return NULL;}
        draft.constant_count=(u32)n;draft.constant_offsets=offsets;
        draft.sampled_units=rsx_fp_texture_units(snapshot,draft.bytes);
        draft.content_hash=rsx_fp_structural_hash(snapshot,draft.bytes,1469598103934665603ull);
    } else {
        const u32 n=rsx_vp_program_size_instrs(snapshot,available);
        if(!n || rsx_vp_analyze_inputs(snapshot,n*16,&draft.inputs)<0) {free(snapshot);++c->invalid;return NULL;}
        draft.bytes=n*16;draft.txl_mask=txl(snapshot,n);
        draft.content_hash=rsx_smc_hash_bytes(snapshot,draft.bytes,1469598103934665603ull);
    }
    if(!draft.content_hash) {free(snapshot);++c->invalid;return NULL;}
    draft.code=snapshot;
    if(!rsx_smc_matches(&draft,source,available)) {free(snapshot);++c->invalid;return NULL;}
    for(u32 i=0;i<RSX_SMC_ENTRIES;++i) {
        rsx_shader_metadata* e=&c->entries[i];
        if(e->code && e->stage==stage && e->bytes==draft.bytes && e->content_hash==draft.content_hash &&
           rsx_smc_matches(e,snapshot,draft.bytes)) {
            free(snapshot);++c->content_hits;rsx_smc_set_hint(c,address_key,e);return rsx_smc_pin(c,e);
        }
    }
    ++c->misses;
    rsx_shader_metadata* result=rsx_smc_insert(c,&draft,snapshot,address_key);
    free(snapshot);return result;
}
#endif
