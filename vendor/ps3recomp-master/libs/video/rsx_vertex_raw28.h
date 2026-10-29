/* One guarded interleaved vertex ABI. CPU snapshots raw bytes; the existing
 * vertex shader decodes them. No guest-memory alias remains live on the GPU. */
#ifndef PS3RECOMP_RSX_VERTEX_RAW28_H
#define PS3RECOMP_RSX_VERTEX_RAW28_H
#include "rsx_vertex_compact.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define RSX_RAW28_MASK 0xC10Du
#define RSX_RAW28_STRIDE 28u
typedef struct rsx_raw28_ia_attr { u32 attr, offset, components; } rsx_raw28_ia_attr;
static const rsx_raw28_ia_attr rsx_raw28_ia[5] = {
    {0,0,3},{2,12,1},{8,24,1},{14,16,1},{15,20,1}
};

typedef struct rsx_raw28_source {
    const u8* bytes;
    u32 first, last;
} rsx_raw28_source;

/* Same frequency<=1 rule as rsx_vertex_element_index, including RSX's
 * twenty-bit element wrap (not merely host u32 overflow). */
static u32 rsx_raw28_element(rsx_vertex_ref ref)
{
    return (ref.vertex_id+ref.base_index)&0x000fffffu;
}

static int rsx_raw28_plan(const rsx_vertex_fetch_plan* p,
                         const rsx_vertex_ref* refs, u32 count,
                         rsx_raw28_source* out)
{
    static const u8 attrs[5] = {0, 2, 8, 14, 15};
    static const u8 types[5] = {2, 6, 3, 6, 6};
    static const u8 sizes[5] = {3, 1, 2, 1, 1};
    static const u8 offsets[5] = {0, 12, 24, 16, 20};
    static const u32 white[4] = {0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u};
    if (!p || !out || !refs || !count || p->inline_data || !p->guest_ptr ||
        p->layout.mask != RSX_RAW28_MASK || p->layout.count != 6 ||
        p->layout.stride != 96 || p->attr[3].desc.size ||
        memcmp(p->attr[3].default_value, white, sizeof white)) return 0;
    const rsx_dsp_vertex_attr* a0 = &p->attr[0].desc;
    for (u32 i=0; i<5; i++) {
        const rsx_dsp_vertex_attr* a = &p->attr[attrs[i]].desc;
        if (a->type != types[i] || a->size != sizes[i] || a->stride != 28 ||
            a->frequency != 0 || a->location != a0->location ||
            (u64)a0->offset + offsets[i] > UINT_MAX ||
            a->offset != a0->offset + offsets[i]) return 0;
    }
    u32 first=UINT_MAX, last=0;
    for (u32 i=0; i<count; i++) {
        const u32 e=rsx_raw28_element(refs[i]);
        if (e<first) first=e;
        if (e>last) last=e;
    }
    const u64 start=(u64)p->base_offset+a0->offset+(u64)first*28u;
    const u64 end=(u64)p->base_offset+a0->offset+((u64)last+1u)*28u;
    if (start>UINT_MAX || end>0x100000000ull || end<=start || end-start>UINT_MAX)
        return 0;
    const u8* bytes=p->guest_ptr(p->guest_user,a0->location,(u32)start,(u32)(end-start));
    if (!bytes) return 0;
    out->bytes=bytes; out->first=first; out->last=last;
    return 1;
}

static void rsx_raw28_copy(const rsx_raw28_source* s,
                          const rsx_vertex_ref* refs, u32 count, u8* out)
{
    for (u32 i=0; i<count; i++) {
        const u32 e=rsx_raw28_element(refs[i]);
        memcpy(out+(u64)i*28u,s->bytes+(u64)(e-s->first)*28u,28u);
    }
}

/* Diagnostic readers still expose the canonical float4 ABI. */
static int rsx_raw28_decode_attr(const u8* vertex, u32 attr, float out[4])
{
    switch(attr) {
    case 0: return rsx_vertex_decode_element(2,3,vertex,out);
    case 2: return rsx_vertex_decode_element(6,1,vertex+12,out);
    case 8: return rsx_vertex_decode_element(3,2,vertex+24,out);
    case 14:return rsx_vertex_decode_element(6,1,vertex+16,out);
    case 15:return rsx_vertex_decode_element(6,1,vertex+20,out);
    case 3: out[0]=out[1]=out[2]=out[3]=1.0f; return 1;
    default:return 0;
    }
}

static int rsx_raw28_replace(char* text, u32 cap, const char* from, const char* to)
{
    char* at=strstr(text,from);
    if(!at || strstr(at+strlen(from),from)) return 0;
    const size_t n=strlen(text), a=strlen(from), b=strlen(to);
    if(n-a+b+1>cap) return 0;
    memmove(at+b,at+a,strlen(at+a)+1); memcpy(at,to,b); return 1;
}

/* CMP division uses integer rational rounding, avoiding approximate HLSL
 * reciprocal multiplication. HALF follows the CPU's signed-zero denorm flush
 * and NaN payload construction rather than f16tof32's different semantics. */
static const char rsx_raw28_hlsl_helpers[] =
"uint raw28_be(uint x){return ((x&255u)<<24)|((x&65280u)<<8)|((x>>8)&65280u)|(x>>24);}\n"
"float raw28_half(uint h){uint s=(h&32768u)<<16,e=(h>>10)&31u,m=h&1023u;return asfloat(s|(e==0u?0u:e==31u?(2139095040u|(m<<13)):(((e+112u)<<23)|(m<<13))));}\n"
"float raw28_norm(int x,uint n){if(x==0)return 0.0;uint a=(uint)abs(x),k=(uint)firstbithigh(a),d=(1u<<n)-1u,N=a<<(23u-k),q=N+N/d,r=N%d; q+=(r*2u>d)?1u:0u;int e=(int)k-(int)n;if(q>=16777216u){q>>=1;e++;}return asfloat((x<0?2147483648u:0u)|((uint)(e+127)<<23)|(q&8388607u));}\n"
"float4 raw28_cmp(uint le){uint w=raw28_be(le);int x=(int)(w<<21)>>21,y=(int)(w<<10)>>21,z=(int)w>>22;return float4(raw28_norm(x,10u),raw28_norm(y,10u),raw28_norm(z,9u),1.0);}\n"
"float4 raw28_pos(uint3 p){return float4(asfloat(uint3(raw28_be(p.x),raw28_be(p.y),raw28_be(p.z))),1.0);}\n"
"float4 raw28_uv(uint p){uint w=raw28_be(p);return float4(raw28_half(w>>16),raw28_half(w&65535u),0.0,1.0);}\n";

static int rsx_raw28_shader(char* text, u32 cap)
{
    static const char* from[]={
        "    float4 a0:ATTR0;\n","    float4 a2:ATTR2;\n",
        "    float4 a3:ATTR3;\n","    float4 a8:ATTR8;\n",
        "    float4 a14:ATTR14;\n","    float4 a15:ATTR15;\n",
        "    v[0]=input.a0;\n","    v[2]=input.a2;\n",
        "    v[3]=input.a3;\n","    v[8]=input.a8;\n",
        "    v[14]=input.a14;\n","    v[15]=input.a15;\n"};
    static const char* to[]={
        "    uint3 a0:ATTR0;\n","    uint a2:ATTR2;\n",
        "","    uint a8:ATTR8;\n",
        "    uint a14:ATTR14;\n","    uint a15:ATTR15;\n",
        "    v[0]=raw28_pos(input.a0);\n","    v[2]=raw28_cmp(input.a2);\n",
        "    v[3]=float4(1,1,1,1);\n","    v[8]=raw28_uv(input.a8);\n",
        "    v[14]=raw28_cmp(input.a14);\n","    v[15]=raw28_cmp(input.a15);\n"};
    /* Work privately so an unrecognized decompiler output cannot leave a
     * partially adapted shader eligible for use. */
    const size_t n=strlen(text), extra=sizeof(rsx_raw28_hlsl_helpers)-1;
    if(n+extra+1>cap) return 0;
    char* copy=(char*)malloc(cap); if(!copy)return 0;
    memcpy(copy,text,n+1);
    int ok=1;
    for(u32 i=0;i<sizeof(from)/sizeof(from[0]);i++)
        if(!rsx_raw28_replace(copy,cap,from[i],to[i])){ok=0;break;}
    if(ok){
        const size_t rewritten=strlen(copy);
        if(rewritten+extra+1>cap)ok=0;
        else {memmove(copy+extra,copy,rewritten+1);memcpy(copy,rsx_raw28_hlsl_helpers,extra);memcpy(text,copy,rewritten+extra+1);}
    }
    free(copy); return ok;
}
#endif
