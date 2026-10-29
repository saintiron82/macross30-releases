/* Caller serializes with the GCM flip lock. No guest callbacks or GPU waits. */
#ifndef RSX_GUEST_COMPLETION_H
#define RSX_GUEST_COMPLETION_H
#include <stdint.h>
#include <string.h>
#define RSX_GC_CAP 256u
typedef struct { uint32_t buffer, sequence; } rsx_gc_flip;
typedef struct { rsx_gc_flip entries[RSX_GC_CAP];uint32_t head,count; } rsx_gc_fifo;
typedef struct { uint32_t ea;rsx_gc_flip flip;int valid; } rsx_gc_marker;
typedef struct {
    int enabled,fault;
    rsx_gc_marker markers[RSX_GC_CAP];
    rsx_gc_fifo present,inflight,gpu_done,callbacks;
    uint32_t display_buffer;
} rsx_guest_completion;
enum { RSX_GC_OVERFLOW=1,RSX_GC_DUPLICATE=2,RSX_GC_UNKNOWN_MARKER=3,
       RSX_GC_OUT_OF_ORDER=4,RSX_GC_UNSUPPORTED=5 };
static inline int rsx_gc_push(rsx_gc_fifo*q,rsx_gc_flip f){
    if(q->count==RSX_GC_CAP)return 0;
    q->entries[(q->head+q->count)%RSX_GC_CAP]=f;q->count++;return 1;
}
static inline int rsx_gc_pop(rsx_gc_fifo*q,rsx_gc_flip*f){
    if(!q->count)return 0;
    *f=q->entries[q->head];q->head=(q->head+1)%RSX_GC_CAP;q->count--;return 1;
}
static inline int rsx_gc_busy(const rsx_guest_completion*g){
    if(g->present.count||g->inflight.count||g->gpu_done.count||g->callbacks.count)return 1;
    for(unsigned i=0;i<RSX_GC_CAP;i++)if(g->markers[i].valid)return 1;
    return 0;
}
static inline int rsx_gc_mode(rsx_guest_completion*g,int enabled){
    if(!enabled&&(g->inflight.count||g->gpu_done.count||g->callbacks.count))return -1;
    if(!!enabled==g->enabled)return g->fault?-1:0;
    memset(g,0,sizeof(*g));g->enabled=!!enabled;return 0;
}
static inline int rsx_gc_marker_add(rsx_guest_completion*g,uint32_t ea,rsx_gc_flip f){
    if(g->fault)return -1;
    rsx_gc_marker*free_slot=0;
    for(unsigned i=0;i<RSX_GC_CAP;i++){
        if(g->markers[i].valid&&g->markers[i].ea==ea){g->fault=RSX_GC_DUPLICATE;return -1;}
        if(!g->markers[i].valid&&!free_slot)free_slot=&g->markers[i];
    }
    if(!free_slot){g->fault=RSX_GC_OVERFLOW;return -1;}
    free_slot->ea=ea;free_slot->flip=f;free_slot->valid=1;return 0;
}
static inline int rsx_gc_reached(rsx_guest_completion*g,uint32_t ea,uint32_t buffer){
    if(g->fault)return -1;
    for(unsigned i=0;i<RSX_GC_CAP;i++)if(g->markers[i].valid&&g->markers[i].ea==ea){
        if(g->markers[i].flip.buffer!=buffer){g->fault=RSX_GC_UNKNOWN_MARKER;return -1;}
        if(!rsx_gc_push(&g->present,g->markers[i].flip)){g->fault=RSX_GC_OVERFLOW;return -1;}
        g->markers[i].valid=0;return 0;
    }
    g->fault=RSX_GC_UNKNOWN_MARKER;return -1;
}
static inline int rsx_gc_direct(rsx_guest_completion*g,rsx_gc_flip f){
    if(g->fault)return -1;
    if(!rsx_gc_push(&g->present,f)){g->fault=RSX_GC_OVERFLOW;return -1;}return 0;
}
static inline int rsx_gc_take_present(rsx_guest_completion*g,rsx_gc_flip*f){
    if(g->fault)return -1;
    if(!g->present.count)return 0;
    if(g->inflight.count==RSX_GC_CAP){g->fault=RSX_GC_OVERFLOW;return -1;}
    rsx_gc_pop(&g->present,f);rsx_gc_push(&g->inflight,*f);return 1;
}
static inline int rsx_gc_gpu_complete(rsx_guest_completion*g,uint32_t sequence){
    if(g->fault)return -1;
    if(!g->inflight.count||g->inflight.entries[g->inflight.head].sequence!=sequence){g->fault=RSX_GC_OUT_OF_ORDER;return -1;}
    if(g->gpu_done.count==RSX_GC_CAP){g->fault=RSX_GC_OVERFLOW;return -1;}
    rsx_gc_flip f;rsx_gc_pop(&g->inflight,&f);rsx_gc_push(&g->gpu_done,f);g->display_buffer=f.buffer;return 0;
}
static inline int rsx_gc_tick(rsx_guest_completion*g){
    if(g->fault)return -1;
    if(g->gpu_done.count>RSX_GC_CAP-g->callbacks.count){g->fault=RSX_GC_OVERFLOW;return -1;}
    int n=(int)g->gpu_done.count;rsx_gc_flip f;
    while(rsx_gc_pop(&g->gpu_done,&f))rsx_gc_push(&g->callbacks,f);
    return n;
}
#endif
