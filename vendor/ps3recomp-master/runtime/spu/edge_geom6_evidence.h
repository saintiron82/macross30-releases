/* Bounded, opt-in evidence for the rejected geom6 position-only scheme-0
 * keys. Capture never earns trust or alters guest data. At most eight cases,
 * under 32 MiB total; no directory or file writes when disabled. */
#ifndef EDGE_GEOM6_EVIDENCE_H
#define EDGE_GEOM6_EVIDENCE_H
#include <stddef.h>
static INIT_ONCE g6e_once=INIT_ONCE_STATIC_INIT;
static int g6e_on;
static volatile LONG g6e_claims;
static __declspec(thread) struct {int active,ok;unsigned id;char prefix[96];} g6e;
static BOOL CALLBACK g6e_init(PINIT_ONCE a,PVOID b,PVOID*c) {
    (void)a;(void)b;(void)c;const char* e=getenv("EDGE_GEOM6_EVIDENCE");g6e_on=e&&strcmp(e,"1")==0;return TRUE;
}
static int g6e_write(const char* suffix,const void* data,size_t size) {
    char path[160];snprintf(path,sizeof path,"%s.%s",g6e.prefix,suffix);
    FILE*f=fopen(path,"wb");if(!f)return 0;
    const int ok=fwrite(data,1,size,f)==size;const int closed=fclose(f)==0;return ok&&closed;
}
static void g6e_begin(const spu_context*ctx,uint32_t stride,uint32_t nv,uint32_t ni,uint32_t sig,
                      uint32_t hole,const void*native,const void*indices) {
    g6e.active=0;InitOnceExecuteOnce(&g6e_once,g6e_init,NULL,NULL);if(!g6e_on)return;
    const uint64_t vb=(uint64_t)stride*nv,ib=(uint64_t)ni*2u;
    if(!vb||vb>(1u<<20)||!ib||ib>(1u<<18)||!native||!indices)return;
    const LONG id=InterlockedIncrement(&g6e_claims);if(id>8)return;
    CreateDirectoryA("geom6-evidence",NULL);
    snprintf(g6e.prefix,sizeof g6e.prefix,"geom6-evidence/pid-%lu-%llu-case-%ld",
             (unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64(),id);
    g6e.id=(unsigned)id;g6e.active=1;g6e.ok=1;
    LARGE_INTEGER q;QueryPerformanceCounter(&q);
    fprintf(stderr,"[geom6-evidence] begin case=%u qpc=%llu prefix=%s (capture IO perturbs timing)\n",
            g6e.id,(unsigned long long)q.QuadPart,g6e.prefix);
    g6e.ok&=g6e_write("pre-ls.bin",ctx->ls,SPU_LS_SIZE);
    g6e.ok&=g6e_write("pre-context.bin",ctx,sizeof *ctx);
    g6e.ok&=g6e_write("native.bin",native,(size_t)vb);
    g6e.ok&=g6e_write("indices.bin",indices,(size_t)ib);
    char meta[700];int n=snprintf(meta,sizeof meta,
      "{\"complete\":false,\"image\":10011,\"scheme\":0,\"pos_only\":true,\"format\":\"44/40\","
      "\"stride\":%u,\"vertices\":%u,\"indices\":%u,\"signature\":%u,\"hole\":%u,"
      "\"context_bytes\":%zu,\"ls_pointer_offset\":%zu,\"scope\":\"staged LS/context and native candidate; external GET memory not captured\"}\n",
      stride,nv,ni,sig,hole,sizeof *ctx,offsetof(spu_context,ls));
    g6e.ok&=n>0&&(size_t)n<sizeof meta&&g6e_write("input.json",meta,(size_t)n);
}
static void g6e_finish(const spu_context*ctx,const es_output_log*puts) {
    if(!g6e.active)return;g6e.active=0;
    g6e.ok&=g6e_write("post-ls.bin",ctx->ls,SPU_LS_SIZE);
    g6e.ok&=g6e_write("post-context.bin",ctx,sizeof *ctx);
    /* Serialize only initialized records/data, never unused recorder storage. */
    if(!puts||puts->n>ES_OUTPUT_RECORDS||puts->bytes>ES_OUTPUT_BYTES)g6e.ok=0;
    else {
        uint32_t header[3]={puts->n,puts->bytes,(uint32_t)puts->overflow};
        g6e.ok&=g6e_write("put-header.bin",header,sizeof header);
        g6e.ok&=g6e_write("put-records.bin",puts->rec,puts->n*sizeof puts->rec[0]);
        g6e.ok&=g6e_write("put-data.bin",puts->data,puts->bytes);
        if(puts->overflow)g6e.ok=0;
    }
    const char* result=g6e.ok?"{\"complete\":true,\"comparison\":\"not evaluated by capture\"}\n":"{\"complete\":false}\n";
    if(!g6e_write("result.json",result,strlen(result)))g6e.ok=0;
    LARGE_INTEGER q;QueryPerformanceCounter(&q);
    fprintf(stderr,"[geom6-evidence] case=%u saved=%d qpc=%llu prefix=%s (no trust change)\n",
            g6e.id,g6e.ok,(unsigned long long)q.QuadPart,g6e.prefix);
}
#endif
