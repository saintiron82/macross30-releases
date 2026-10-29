/* Backend-only staged integration. Include after g and ld_gt helpers. */
#ifndef RSX_LIVE_ASYNC_FRAMES_H
#define RSX_LIVE_ASYNC_FRAMES_H
#define LD_AF_CONTEXTS 3
#define LD_AF_COMPLETIONS 64
#define LD_AF_LABELS 1024
extern void vm_write32(uint64_t address,uint32_t value);
typedef struct ld_af_context {
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *buffer[5];
    void *mapped[5];
    ID3D12DescriptorHeap *srv, *smp;
    ID3D12Resource *retired[MAX_RETIRED_TEXTURES];
    u32 n_retired;
    u64 frame_fence;
    u32 sequence;
    int completion_pending;
} ld_af_context;
static struct {
    int on, pending, poisoned, request_valid, label_batch;
    HANDLE gpu_completion_event;
    u64 gpu_armed_fence;
    u32 active, bb, request_seq, complete_frames;
    u32 complete_seq[LD_AF_COMPLETIONS], complete_read, complete_write;
    u64 bb_fence[LD_SWAP_BUFFERS];
    struct {u64 fence;u32 ea,value;} labels[LD_AF_LABELS];
    u64 label_read,label_write,label_assigned;
    ld_af_context ctx[LD_AF_CONTEXTS];
} g_af;
/* Sparse async-only counters: no QPC/per-draw clocks. A pending wait means
   completed<required at the initial fence read, not measured sleep duration. */
#define LD_AF_FLUSH_REASONS 11
#define LD_AF_WAIT_BB 0
#define LD_AF_WAIT_CONTEXT 1
#define LD_AF_WAIT_DRAIN_BASE 2
#define LD_AF_WAIT_EXTERNAL 13
#define LD_AF_WAIT_CAUSES 14
typedef struct ld_af_counters {
    u64 prefix_submits,present_submits,frame_signals,labels_published,clones,blank,gpu_arms;
    u64 batch_cause[3]; /* Actual prefix Execute: drain/acquire/threshold. */
    u64 wait_calls[LD_AF_WAIT_CAUSES],wait_pending[LD_AF_WAIT_CAUSES];
    u64 drains[LD_AF_FLUSH_REASONS],legacy_submits[LD_AF_FLUSH_REASONS],legacy_waits[LD_AF_FLUSH_REASONS];
} ld_af_counters;
static ld_af_counters g_af_counts,g_af_counts_last;
static u64 g_af_label_high,g_af_label_high_total;
static void ld_af_counts_emit(const char *why) {
    if(!g_af.on)return;
    const ld_af_counters *c=&g_af_counts,*p=&g_af_counts_last;
    char line[4096]; /* Maximum fixed field count with 20-digit u64s fits. */
    int n=snprintf(line,sizeof(line),"[async-frame-counts] reason=%s completed=%u window=delta prefix=%llu present=%llu frame_signal=%llu label_publish=%llu label_high=%llu clone=%llu blank=%llu wait_bb=%llu/%llu wait_context=%llu/%llu wait_external=%llu/%llu nonpresent={",
        why,g_af.complete_frames,(unsigned long long)(c->prefix_submits-p->prefix_submits),(unsigned long long)(c->present_submits-p->present_submits),(unsigned long long)(c->frame_signals-p->frame_signals),(unsigned long long)(c->labels_published-p->labels_published),(unsigned long long)g_af_label_high,(unsigned long long)(c->clones-p->clones),(unsigned long long)(c->blank-p->blank),
        (unsigned long long)(c->wait_calls[0]-p->wait_calls[0]),(unsigned long long)(c->wait_pending[0]-p->wait_pending[0]),(unsigned long long)(c->wait_calls[1]-p->wait_calls[1]),(unsigned long long)(c->wait_pending[1]-p->wait_pending[1]),(unsigned long long)(c->wait_calls[13]-p->wait_calls[13]),(unsigned long long)(c->wait_pending[13]-p->wait_pending[13]));
    for(u32 i=0;i<LD_AF_FLUSH_REASONS;i++){
        n+=snprintf(line+n,sizeof(line)-(size_t)n,"%s%u:%llu/%llu/%llu/%llu/%llu",i?",":"",i,
            (unsigned long long)(c->drains[i]-p->drains[i]),
            (unsigned long long)(c->wait_calls[2+i]-p->wait_calls[2+i]),
            (unsigned long long)(c->wait_pending[2+i]-p->wait_pending[2+i]),
            (unsigned long long)(c->legacy_submits[i]-p->legacy_submits[i]),
            (unsigned long long)(c->legacy_waits[i]-p->legacy_waits[i]));
    }
    snprintf(line+n,sizeof(line)-(size_t)n,"} batch_cause_delta=%llu/%llu/%llu totals={prefix=%llu present=%llu frame_signal=%llu label_publish=%llu label_high=%llu gpu_arm=%llu batch_cause=%llu/%llu/%llu} batch_cause_order=drain/acquire/threshold wait_pairs=call/pending nonpresent_fields=drain/waitcall/pending/legacy_submit/legacy_pending reasons=0present,1reference,2VB,3CB,4retire,5movie,6moviePresent,7readback,8PSCB,9descriptor,10shutdown\n",
        (unsigned long long)(c->batch_cause[0]-p->batch_cause[0]),(unsigned long long)(c->batch_cause[1]-p->batch_cause[1]),(unsigned long long)(c->batch_cause[2]-p->batch_cause[2]),
        (unsigned long long)c->prefix_submits,(unsigned long long)c->present_submits,(unsigned long long)c->frame_signals,(unsigned long long)c->labels_published,(unsigned long long)g_af_label_high_total,(unsigned long long)c->gpu_arms,
        (unsigned long long)c->batch_cause[0],(unsigned long long)c->batch_cause[1],(unsigned long long)c->batch_cause[2]);
    fputs(line,stderr);
    g_af_counts_last=*c;g_af_label_high=0;
}
static void ld_af_poison(const char *where, HRESULT hr) {
    fprintf(stderr,"[async-frame-fatal] op=%s hr=%08lX active=%u fence=%llu\n",where,(unsigned long)hr,g_af.active,(unsigned long long)g.fence_value);
    g_af.poisoned=1;g.ready=0;
}
static int ld_af_wait_cause(u64 value,u32 cause) {
    if(g_af.on&&cause<LD_AF_WAIT_CAUSES)g_af_counts.wait_calls[cause]++;
    u64 done=g.fence->lpVtbl->GetCompletedValue(g.fence);
    if(done==UINT64_MAX){ld_af_poison("device-removed",E_FAIL);return 0;}
    if(done>=value)return 1;
    if(g_af.on&&cause<LD_AF_WAIT_CAUSES)g_af_counts.wait_pending[cause]++;
    HRESULT hr=g.fence->lpVtbl->SetEventOnCompletion(g.fence,value,g.fence_event);
    if(FAILED(hr)){ld_af_poison("wait-event",hr);return 0;}
    if(WaitForSingleObject(g.fence_event,INFINITE)!=WAIT_OBJECT_0){ld_af_poison("wait",E_FAIL);return 0;}
    done=g.fence->lpVtbl->GetCompletedValue(g.fence);
    if(done==UINT64_MAX||done<value){ld_af_poison("wait-completion",E_FAIL);return 0;}
    return 1;
}
static void ld_af_save(ld_af_context *c) {
    c->alloc=g.alloc;c->list=g.list;
    c->buffer[0]=g.upload;c->mapped[0]=g.upload_mapped;
    c->buffer[1]=g.vb;c->mapped[1]=g.vb_mapped;
    c->buffer[2]=g.ib;c->mapped[2]=g.ib_mapped;
    c->buffer[3]=g.cb;c->mapped[3]=g.cb_mapped;
    c->buffer[4]=g.ps_cb;c->mapped[4]=g.ps_cb_mapped;
    c->srv=g.srv_heap;c->smp=g.smp_heap;
    c->n_retired=g.n_retired_textures;
    memcpy(c->retired,g.retired_textures,sizeof(c->retired));
}
static void ld_af_bind(ld_af_context *c) {
    g.alloc=c->alloc;g.list=c->list;
    g.upload=c->buffer[0];g.upload_mapped=c->mapped[0];
    g.vb=c->buffer[1];g.vb_mapped=c->mapped[1];
    g.ib=c->buffer[2];g.ib_mapped=c->mapped[2];
    g.cb=c->buffer[3];g.cb_mapped=c->mapped[3];
    g.ps_cb=c->buffer[4];g.ps_cb_mapped=c->mapped[4];
    g.srv_heap=c->srv;g.smp_heap=c->smp;
    g.n_retired_textures=c->n_retired;
    memcpy(g.retired_textures,c->retired,sizeof(c->retired));
    /* Every SRV table copies live CPU descriptors. Invalidate sampler table
       cache on activation so live shared CPU descriptors are copied too. */
    memset(g.smp_ring_slots,0xff,sizeof(g.smp_ring_slots));
}
static void ld_af_release_retired(ld_af_context *c) {
    for(u32 i=0;i<c->n_retired;i++)c->retired[i]->lpVtbl->Release(c->retired[i]);
    c->n_retired=0;
}
/* One outstanding event registration, for the oldest observable completion.
   Never use the synchronous wait event: the present thread may block on either
   event independently, and auto-reset consumption must not steal a sync wait. */
static void ld_af_arm_completion(u64 done) {
    if(!g_af.gpu_completion_event||g_af.poisoned)return;
    if(g_af.gpu_armed_fence&&done>=g_af.gpu_armed_fence){
        g_af.gpu_armed_fence=0;
        if(!ResetEvent(g_af.gpu_completion_event)){ld_af_poison("gpu-wake-reset",HRESULT_FROM_WIN32(GetLastError()));return;}
    }
    u64 earliest=0;
    if(g_af.label_read!=g_af.label_assigned)earliest=g_af.labels[g_af.label_read%LD_AF_LABELS].fence;
    for(u32 i=0;i<LD_AF_CONTEXTS;i++){
        ld_af_context*c=&g_af.ctx[i];
        if(c->completion_pending&&(!earliest||c->frame_fence<earliest))earliest=c->frame_fence;
    }
    if(g_af.gpu_armed_fence){
        /* Queue tickets are monotonic: new work cannot precede the oldest
           outstanding registration. Never replace it with per-label arms. */
        if(earliest&&earliest<g_af.gpu_armed_fence)ld_af_poison("gpu-wake-ticket-order",E_FAIL);
        return;
    }
    if(!earliest)return;
    if(earliest<=done){ld_af_poison("gpu-wake-unpublished-done",E_FAIL);return;}
    HRESULT hr=g.fence->lpVtbl->SetEventOnCompletion(g.fence,earliest,g_af.gpu_completion_event);
    if(FAILED(hr)){ld_af_poison("gpu-wake-arm",hr);return;}
    g_af.gpu_armed_fence=earliest;g_af_counts.gpu_arms++;
}
static void ld_af_poll(void) {
    if(!g_af.on||g_af.poisoned)return;
    u64 done=g.fence->lpVtbl->GetCompletedValue(g.fence);
    if(done==UINT64_MAX){ld_af_poison("poll-device-removed",E_FAIL);return;}
    /* Unassigned batch labels have fence0, never a completion ticket. */
    while(g_af.label_read!=g_af.label_assigned){
        u32 slot=(u32)(g_af.label_read%LD_AF_LABELS);
        if(g_af.labels[slot].fence>done)break;
        /* Out-of-line VM store preserves guest endian conversion, VM_WRITE_COH
           / SPU reservation notifications and VM validation. Never raw memcpy.
           Release barrier orders prior host readback/coherent stores before
           the completion label observed by guest PPU/SPU pollers. */
        MemoryBarrier();
        vm_write32((uint64_t)g_af.labels[slot].ea,g_af.labels[slot].value);
        g_af.label_read++;g_af_counts.labels_published++;
    }
    for(;;){
        ld_af_context *first=NULL;
        for(u32 i=0;i<LD_AF_CONTEXTS;i++){
            ld_af_context *c=&g_af.ctx[i];
            if(c->completion_pending&&(!first||c->frame_fence<first->frame_fence))first=c;
        }
        if(!first||first->frame_fence>done)break;
        if(g_af.complete_write-g_af.complete_read>=LD_AF_COMPLETIONS){ld_af_poison("completion-overflow",E_FAIL);return;}
        g_af.complete_seq[g_af.complete_write++%LD_AF_COMPLETIONS]=first->sequence;
        first->completion_pending=0;g_af.complete_frames++;
        if((g_af.complete_frames&255u)==0)ld_af_counts_emit("256frames");
    }
    ld_af_arm_completion(done);
}
static void ld_af_assign_labels(u64 fence) {
    if(!g_af.on||!g_af.label_batch)return;
    if(!fence||fence>g.fence_value){ld_af_poison("label-assignment-fence",E_FAIL);return;}
    while(g_af.label_assigned!=g_af.label_write){
        g_af.labels[g_af.label_assigned%LD_AF_LABELS].fence=fence;
        g_af.label_assigned++;
    }
}
static int ld_af_batch_submit(u32 cause);
static int ld_af_completion_shutdown(void){
    if(!g_af.gpu_completion_event)return 1;
    if(g_af.poisoned)return 0; /* Keep registered handles/resources for exit. */
    u64 done=g.fence->lpVtbl->GetCompletedValue(g.fence);
    if(done==UINT64_MAX||done<g.fence_value){ld_af_poison("gpu-wake-shutdown-undrained",E_FAIL);return 0;}
    /* Called only after ld_flush(SHUTDOWN) has drained every queue ticket. */
    if(!CloseHandle(g_af.gpu_completion_event)){ld_af_poison("gpu-wake-close",HRESULT_FROM_WIN32(GetLastError()));return 0;}
    g_af.gpu_completion_event=NULL;g_af.gpu_armed_fence=0;return 1;
}
static int ld_af_drain_reason(u32 reason) {
    if(!g_af.on)return 1;
    if(g_af.poisoned)return 0;
    if(g_af.pending){ld_af_poison("unpaired-present",E_UNEXPECTED);return 0;}
    /* Includes active-context prefix submissions, which need not have a
       Present/frame fence yet. All values originate from actual queue Signal. */
    if(reason<LD_AF_FLUSH_REASONS)g_af_counts.drains[reason]++;
    if(!ld_af_wait_cause(g.fence_value,reason<LD_AF_FLUSH_REASONS?LD_AF_WAIT_DRAIN_BASE+reason:LD_AF_WAIT_EXTERNAL))return 0;
    ld_af_poll();if(g_af.poisoned)return 0;
    for(u32 i=0;i<LD_AF_CONTEXTS;i++)if(i!=g_af.active)ld_af_release_retired(&g_af.ctx[i]);
    return !g_af.poisoned;
}
static int ld_af_drain(void){
    if(g_af.on&&g_af.label_batch&&g_af.label_assigned!=g_af.label_write&&ld_af_batch_submit(0)<0)return 0;
    return ld_af_drain_reason(LD_AF_FLUSH_REASONS);
}
static int ld_af_wait(u64 value){return ld_af_wait_cause(value,LD_AF_WAIT_EXTERNAL);}
static int ld_af_clone(ld_af_context *c) {
    if(g_af.on)g_af_counts.clones++;
    HRESULT hr=g.dev->lpVtbl->CreateCommandAllocator(g.dev,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&c->alloc);
    if(FAILED(hr))return 0;
    hr=g.dev->lpVtbl->CreateCommandList(g.dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,c->alloc,NULL,&IID_ID3D12GraphicsCommandList,(void**)&c->list);
    if(FAILED(hr))return 0;
    if(FAILED(c->list->lpVtbl->Close(c->list)))return 0;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RANGE rr={0,0};
    for(u32 i=0;i<5;i++){
        D3D12_RESOURCE_DESC desc;
        g_af.ctx[0].buffer[i]->lpVtbl->GetDesc(g_af.ctx[0].buffer[i],&desc);
        hr=g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)&c->buffer[i]);
        if(FAILED(hr)||FAILED(c->buffer[i]->lpVtbl->Map(c->buffer[i],0,&rr,&c->mapped[i])))return 0;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    g_af.ctx[0].srv->lpVtbl->GetDesc(g_af.ctx[0].srv,&hd);
    if(FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&c->srv)))return 0;
    g_af.ctx[0].smp->lpVtbl->GetDesc(g_af.ctx[0].smp,&hd);
    return SUCCEEDED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&c->smp));
}
/* No allocation after submission until worst-case memory admission succeeds.
   Lazy allocation failure after admission remains fatal, never legacy reuse. */
static void ld_af_init(void) {
    const char *v=getenv("LD_ASYNC_FRAME_RESOURCES");
    if(!v||strcmp(v,"1"))return;
    ld_af_save(&g_af.ctx[0]);
    u64 bytes=0;
    for(u32 i=0;i<5;i++){D3D12_RESOURCE_DESC d;g_af.ctx[0].buffer[i]->lpVtbl->GetDesc(g_af.ctx[0].buffer[i],&d);bytes+=d.Width;}
    u64 descriptors=(u64)SRV_RING_TABLES*SRV_TABLE_SIZE*g.srv_step+(u64)SMP_RING_TABLES*SMP_TABLE_SIZE*g.smp_step;
    u64 extra=2*(bytes+descriptors);
    MEMORYSTATUSEX ms={0};ms.dwLength=sizeof(ms);
    if(!GlobalMemoryStatusEx(&ms)||extra>ms.ullAvailPhys/4){fprintf(stderr,"[async-frame-reject] extra_bytes=%llu reason=memory-budget\n",(unsigned long long)extra);return;}
    /* Host decoder and debug callbacks may touch D3D state off renderer
       thread; first version declines those contracts before any submit. */
    const char *movie=getenv("MACROSS_SAIL_PLAYBACK"), *composite=getenv("RSX_MOVIE_COMPOSITE_UI"), *debug=getenv("D3D12_DEBUG");
    /* The host movie path takes g_ld_access_lock and its flush drains every
       queued frame first. LD_ASYNC_ALLOW_MOVIE=1 admits async with it. */
    const char *allow_movie=getenv("LD_ASYNC_ALLOW_MOVIE");
    const int movie_ok=allow_movie&&!strcmp(allow_movie,"1");
    if(((movie&&!strcmp(movie,"1"))&&!movie_ok)||composite||(debug&&!strcmp(debug,"1"))){fprintf(stderr,"[async-frame-reject] reason=host-movie-or-debug\n");return;}
    const char *wake=getenv("LD_ASYNC_GPU_WAKE");
    if(wake&&!strcmp(wake,"1")){
        g_af.gpu_completion_event=CreateEventA(NULL,FALSE,FALSE,NULL);
        if(!g_af.gpu_completion_event){fprintf(stderr,"[async-frame-reject] reason=gpu-wake-create error=%lu\n",(unsigned long)GetLastError());return;}
        fprintf(stderr,"[async-gpu-wake] on=1 auto_reset=1 separate_sync_event=1 earliest_only=1\n");
    }
    g_af.on=1;
    const char *batch=getenv("LD_ASYNC_LABEL_BATCH");
    g_af.label_batch=batch&&!strcmp(batch,"1");
    if(g_af.label_batch)fprintf(stderr,"[async-label-batch] on=1 threshold=256 capacity=1024 assigned_only_publication=1\n");
    fprintf(stderr,"[async-frame-admit] contexts=3 backbuffers=%u cpu_upload_extra_bytes=%llu shader_heap_estimate_bytes=%llu available_phys=%llu allocation=lazy-identical-desc\n",LD_SWAP_BUFFERS,(unsigned long long)(bytes*2),(unsigned long long)(descriptors*2),(unsigned long long)ms.ullAvailPhys);
}
static int ld_af_claim(u32 bb) {
    if(!g_af.on)return 1;
    if(g_af.poisoned)return 0;
    if(bb>=LD_SWAP_BUFFERS){ld_af_poison("backbuffer-index",E_INVALIDARG);return 0;}
    if(!ld_af_wait_cause(g_af.bb_fence[bb],LD_AF_WAIT_BB))return 0;
    g_af.bb=bb;return 1;
}
/* Boundary batching retains every label value and its order. Same-context
   allocator/arenas append until actual wait/retirement, just as immediate
   prefix mode. The ticket is assigned ONLY after a successful real Signal. */
static int ld_af_batch_submit(u32 cause) {
    if(!g_af.on||!g_af.label_batch)return 0;
    if(g_af.poisoned||g_af.pending||!g.ready)return -1;
    if(g_af.label_assigned==g_af.label_write){ld_af_poll();return g_af.poisoned?-1:1;}
    if(g.fence_value==UINT64_MAX){ld_af_poison("batch-fence-overflow",E_FAIL);return -1;}
    int slot=ld_gt_end(LD_FLUSH_GUEST_REFERENCE);
    HRESULT hr=g.list->lpVtbl->Close(g.list);
    if(FAILED(hr)){ld_af_poison("batch-close",hr);return -1;}
    ID3D12CommandList *lists[]={(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);g_af_counts.prefix_submits++;
    if(cause<3u)g_af_counts.batch_cause[cause]++;
    u64 fence=++g.fence_value;
    hr=g.queue->lpVtbl->Signal(g.queue,g.fence,fence);
    if(FAILED(hr)){ld_af_poison("batch-signal",hr);return -1;}
    ld_af_assign_labels(fence);
    if(g_af.poisoned)return -1;
    if(slot>=0){g_gt.slots[slot].fence=fence;g_gt.slots[slot].pending=1;g_gt.slots[slot].seq=++g_gt.count;g_gt.active=-1;}
    hr=g.list->lpVtbl->Reset(g.list,g.alloc,NULL);
    if(FAILED(hr)){ld_af_poison("batch-list-reset",hr);return -1;}
    ld_gt_begin();ld_af_poll();return g_af.poisoned?-1:1;
}
/* Prefix submission without deferred labels: hand the GPU what this frame
 * has recorded so far, without waiting, and keep recording into the same
 * allocator (arena cursors retained; the frame fence still guards reuse).
 * The GPU then works while the CPU encodes the rest of the frame instead of
 * starting only at present -- that idle GPU time was the guest's flip wait. */
static int ld_af_prefix_submit(void) {
    if(!g_af.on||g_af.poisoned||g_af.pending||!g.ready)return 0;
    if(g.fence_value==UINT64_MAX)return 0;
    int slot=ld_gt_end(LD_FLUSH_GUEST_REFERENCE);
    HRESULT hr=g.list->lpVtbl->Close(g.list);
    if(FAILED(hr)){ld_af_poison("prefix-close",hr);return -1;}
    ID3D12CommandList *lists[]={(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);g_af_counts.prefix_submits++;
    u64 fence=++g.fence_value;
    hr=g.queue->lpVtbl->Signal(g.queue,g.fence,fence);
    if(FAILED(hr)){ld_af_poison("prefix-signal",hr);return -1;}
    if(slot>=0){g_gt.slots[slot].fence=fence;g_gt.slots[slot].pending=1;g_gt.slots[slot].seq=++g_gt.count;g_gt.active=-1;}
    hr=g.list->lpVtbl->Reset(g.list,g.alloc,NULL);
    if(FAILED(hr)){ld_af_poison("prefix-list-reset",hr);return -1;}
    ld_gt_begin();ld_af_poll();return g_af.poisoned?-1:1;
}
static int ld_af_batch_boundary(void *user,u32 boundary){
    (void)user;return ld_af_batch_submit(boundary);
}
/* Microsoft ID3D12GraphicsCommandList::Reset permits reset while executing.
   ID3D12CommandAllocator::Reset does NOT: retain allocator and every arena
   cursor until actual completion. Prefix fence is not a frame/reuse fence. */
static int ld_af_defer_label(u32 ea,u32 value) {
    if(!g_af.on)return 0;
    if(g_af.poisoned||g_af.pending||!g.ready)return -1;
    ld_af_poll();
    if(g_af.poisoned)return -1;
    if((ea&3u)||g_af.label_write-g_af.label_read>=LD_AF_LABELS||g.fence_value==UINT64_MAX){ld_af_poison("label-queue-or-address",E_FAIL);return -1;}
    if(g_af.label_batch){
        u32 index=(u32)(g_af.label_write%LD_AF_LABELS);
        g_af.labels[index].fence=0;g_af.labels[index].ea=ea;g_af.labels[index].value=value;g_af.label_write++;
        u64 depth=g_af.label_write-g_af.label_read;
        if(depth>g_af_label_high)g_af_label_high=depth;
        if(depth>g_af_label_high_total)g_af_label_high_total=depth;
        if(g_af.label_write-g_af.label_assigned>=256u)return ld_af_batch_submit(2);
        return 1;
    }
    int slot=ld_gt_end(LD_FLUSH_GUEST_REFERENCE);
    HRESULT hr=g.list->lpVtbl->Close(g.list);
    if(FAILED(hr)){ld_af_poison("label-close",hr);return -1;}
    ID3D12CommandList *lists[]={(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
    g_af_counts.prefix_submits++;
    u64 work=++g.fence_value;
    hr=g.queue->lpVtbl->Signal(g.queue,g.fence,work);
    if(FAILED(hr)){ld_af_poison("label-signal",hr);return -1;}
    u32 index=(u32)(g_af.label_write%LD_AF_LABELS);
    g_af.labels[index].fence=work;g_af.labels[index].ea=ea;g_af.labels[index].value=value;g_af.label_write++;g_af.label_assigned=g_af.label_write;
    u64 depth=g_af.label_write-g_af.label_read;
    if(depth>g_af_label_high)g_af_label_high=depth;
    if(depth>g_af_label_high_total)g_af_label_high_total=depth;
    if(slot>=0){g_gt.slots[slot].fence=work;g_gt.slots[slot].pending=1;g_gt.slots[slot].seq=++g_gt.count;g_gt.active=-1;}
    /* Keep resource pointers, retired owners and all seven arena cursors.
       Only a closed submitted list is reset; allocator memory is appended. */
    hr=g.list->lpVtbl->Reset(g.list,g.alloc,NULL);
    if(FAILED(hr)){ld_af_poison("label-list-reset",hr);return -1;}
    ld_gt_begin();return 1;
}
static void ld_af_record_blank(ID3D12Resource *bb,D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    if(g_af.on)g_af_counts.blank++;
    D3D12_RESOURCE_BARRIER bar={0};bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource=bb;bar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;bar.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
    const FLOAT black[4]={0,0,0,1};g.list->lpVtbl->ClearRenderTargetView(g.list,rtv,black,0,NULL);
    bar.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;bar.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
}
static int ld_af_submit_present(void) {
    if(!g_af.on)return 0;
    if(g_af.pending||g_af.poisoned){ld_af_poison("double-submit",E_UNEXPECTED);return 1;}
    if(g.fence_value>UINT64_MAX-2){ld_af_poison("fence-overflow",E_FAIL);return 1;}
    int slot=ld_gt_end(LD_FLUSH_PRESENT);
    HRESULT hr=g.list->lpVtbl->Close(g.list);
    if(FAILED(hr)){ld_af_poison("close",hr);return 1;}
    ID3D12CommandList *lists[]={(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
    g_af_counts.present_submits++;
    u64 work=++g.fence_value;
    hr=g.queue->lpVtbl->Signal(g.queue,g.fence,work);
    if(FAILED(hr)){ld_af_poison("work-signal",hr);return 1;}
    ld_af_assign_labels(work);
    if(g_af.poisoned)return 1;
    if(slot>=0){g_gt.slots[slot].fence=work;g_gt.slots[slot].pending=1;g_gt.slots[slot].seq=++g_gt.count;g_gt.active=-1;}
    g_af.pending=1;return 1;
}
static void ld_af_post_present(HRESULT present_hr) {
    if(!g_af.on||!g_af.pending)return;
    ld_af_context *old=&g_af.ctx[g_af.active];ld_af_save(old);
    u64 frame=++g.fence_value;
    HRESULT hr=g.queue->lpVtbl->Signal(g.queue,g.fence,frame);
    if(FAILED(hr)){ld_af_poison("post-present-signal",hr);return;}
    g_af_counts.frame_signals++;
    old->frame_fence=frame;g_af.bb_fence[g_af.bb]=frame;g_af.pending=0;
    if(FAILED(present_hr)){ld_af_poison("present",present_hr);return;}
    if(!g_af.request_valid){ld_af_poison("missing-guest-request",E_UNEXPECTED);return;}
    if(g_af.label_assigned!=g_af.label_write){ld_af_poison("unassigned-at-context-reuse",E_UNEXPECTED);return;}
    old->sequence=g_af.request_seq;old->completion_pending=1;g_af.request_valid=0;
    u32 next=(g_af.active+1)%LD_AF_CONTEXTS;ld_af_context *c=&g_af.ctx[next];
    if(!ld_af_wait_cause(c->frame_fence,LD_AF_WAIT_CONTEXT))return;
    ld_af_poll();if(g_af.poisoned)return;
    ld_af_release_retired(c);
    if(!c->alloc&&!ld_af_clone(c)){ld_af_poison("lazy-context-allocation",E_OUTOFMEMORY);return;}
    g_af.active=next;ld_af_bind(c);
    hr=g.alloc->lpVtbl->Reset(g.alloc);
    if(SUCCEEDED(hr))hr=g.list->lpVtbl->Reset(g.list,g.alloc,NULL);
    if(FAILED(hr)){ld_af_poison("context-reset",hr);return;}
    g.upload_used=g.vb_used=g.ib_used=g.cb_used=g.ps_cb_used=0;
    g.srv_ring_used=g.smp_ring_used=0;ld_gt_collect();ld_gt_begin();
}
#endif
