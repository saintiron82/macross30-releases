/* Bounded ownership ledger for asynchronous RSX submissions. C17/C++.
 *
 * This file does not submit D3D commands or publish guest labels. One renderer
 * owner serializes all calls. Every context owns its allocator/list, mapped
 * upload/VB/IB/CB/PSCB arenas, shader-visible descriptor slices and retired
 * references until its actual queue fence completes. No API waits implicitly.
 *
 * FRONTEND input copied/command consumed is a different namespace from GPU
 * completion. NV406E/NV4097 visibility must be classified by the caller.
 * GetCompletedValue == UINT64_MAX means device removal, never completion.
 * Initialization requires an idle/drained queue at initial_fence. This ledger
 * owns that fence's value sequence; external signals must use its tickets.
 */
#ifndef RSX_SUBMISSION_RING_H
#define RSX_SUBMISSION_RING_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define RSX_SR_CONTEXTS 3u
#define RSX_SR_BACKBUFFERS 2u
#define RSX_SR_NO_BACKBUFFER UINT32_MAX
#define RSX_SR_RETIRED_CAP 256u
enum rsx_sr_arena { RSX_SR_UPLOAD, RSX_SR_VB, RSX_SR_IB, RSX_SR_CB,
    RSX_SR_PSCB, RSX_SR_SRV, RSX_SR_SMP, RSX_SR_ARENAS };
enum rsx_sr_state { RSX_SR_FREE, RSX_SR_RECORDING, RSX_SR_PREPARED, RSX_SR_EXECUTED,
    RSX_SR_SUBMITTED, RSX_SR_POISONED };
enum rsx_sr_result { RSX_SR_OK=0, RSX_SR_WAIT=1, RSX_SR_FULL=2,
    RSX_SR_PENDING_PRESENT=3, RSX_SR_INVALID=-1, RSX_SR_DEVICE_LOST=-2,
    RSX_SR_FAULT=-3, RSX_SR_OVERFLOW=-4 };
typedef struct { uint32_t slot; uint64_t generation; } rsx_sr_lease;
typedef struct { uint64_t fence; } rsx_sr_gpu_ticket;
typedef struct { uint64_t fence; } rsx_sr_work_ticket;
typedef struct { uint64_t sequence; } rsx_sr_frontend_ticket;
typedef struct {
    void *allocator, *list;
    void *upload, *vb, *ib, *cb, *pscb, *srv_heap, *smp_heap;
} rsx_sr_resources;
typedef struct {
    enum rsx_sr_state state;
    uint64_t generation, fence, work_fence, guest_sequence;
    uint64_t used[RSX_SR_ARENAS];
    rsx_sr_resources resources;
    void *retired[RSX_SR_RETIRED_CAP];
    uint32_t retired_count, backbuffer;
    int is_present, present_decided, present_ok, work_signaled;
} rsx_sr_context;
typedef struct {
    rsx_sr_context contexts[RSX_SR_CONTEXTS];
    uint64_t capacity[RSX_SR_ARENAS], backbuffer_fence[RSX_SR_BACKBUFFERS];
    uint64_t last_reserved, last_submitted, completed, frontend_copied;
    uint32_t next_slot;
    int fault;
} rsx_submission_ring;
typedef void (*rsx_sr_release_fn)(void *user, void *owned_reference);
typedef void (*rsx_sr_present_fn)(void *user, uint64_t guest_sequence,
                                rsx_sr_gpu_ticket ticket);

static inline int rsx_sr_init(rsx_submission_ring *r,
    const uint64_t capacity[RSX_SR_ARENAS], uint64_t initial_fence)
{
    if (!r || !capacity || initial_fence==UINT64_MAX) return RSX_SR_INVALID;
    memset(r,0,sizeof(*r));
    memcpy(r->capacity,capacity,sizeof(r->capacity));
    r->last_reserved=r->last_submitted=r->completed=initial_fence;
    return RSX_SR_OK;
}
static inline rsx_sr_context *rsx_sr_get(rsx_submission_ring *r, rsx_sr_lease l)
{
    if (!r || l.slot>=RSX_SR_CONTEXTS) return NULL;
    rsx_sr_context *c=&r->contexts[l.slot];
    return c->generation==l.generation ? c : NULL;
}
static inline int rsx_sr_observe(rsx_submission_ring *r, uint64_t completed)
{
    if (!r) return RSX_SR_INVALID;
    if (completed==UINT64_MAX) { r->fault=RSX_SR_DEVICE_LOST; return r->fault; }
    if (r->fault) return r->fault;
    if (completed<r->completed || completed>r->last_reserved) return RSX_SR_INVALID;
    r->completed=completed;
    return RSX_SR_OK;
}
/* Release exactly one caller-transferred reference for each retire entry.
 * Retire entries may refer to resources used by earlier contexts: the single
 * ordered queue fence of this submission protects that prefix as well. */
static inline int rsx_sr_collect(rsx_submission_ring *r,
    rsx_sr_release_fn release, rsx_sr_present_fn present, void *user)
{
    if (!r) return RSX_SR_INVALID;
    if (r->fault) return r->fault;
    for (;;) {
        rsx_sr_context *c=NULL;
        for (uint32_t i=0;i<RSX_SR_CONTEXTS;i++) {
            rsx_sr_context *q=&r->contexts[i];
            if (q->state==RSX_SR_SUBMITTED && (!c || q->fence<c->fence)) c=q;
        }
        if (!c || c->fence>r->completed) return RSX_SR_OK;
        if (!c->present_decided) return RSX_SR_PENDING_PRESENT;
        if (c->retired_count && !release) return RSX_SR_INVALID;
        if (c->is_present && c->present_ok && !present) return RSX_SR_INVALID;
        for (uint32_t i=0;i<c->retired_count;i++) release(user,c->retired[i]);
        c->retired_count=0;
        if (c->is_present && c->present_ok && present) {
            rsx_sr_gpu_ticket t={c->fence}; present(user,c->guest_sequence,t);
        }
        c->state=RSX_SR_FREE;
    }
}
/* Caller polls/collects first, then resets this context's actual allocator and
 * list only AFTER begin succeeds. Returned WAIT is the exact reuse boundary.
 * Backbuffer availability is tracked separately from the three contexts. */
static inline int rsx_sr_begin(rsx_submission_ring *r, uint32_t backbuffer,
    rsx_sr_lease *lease, rsx_sr_gpu_ticket *wait_for)
{
    if (!r || !lease || !wait_for ||
        (backbuffer>=RSX_SR_BACKBUFFERS && backbuffer!=RSX_SR_NO_BACKBUFFER)) return RSX_SR_INVALID;
    wait_for->fence=0;
    if (r->fault) return r->fault;
    for (uint32_t i=0;i<RSX_SR_CONTEXTS;i++)
        if (r->contexts[i].state==RSX_SR_RECORDING || r->contexts[i].state==RSX_SR_PREPARED ||
            r->contexts[i].state==RSX_SR_EXECUTED)
            return RSX_SR_INVALID;
    rsx_sr_context *c=&r->contexts[r->next_slot];
    if (c->state==RSX_SR_SUBMITTED) {
        if (!c->present_decided) return RSX_SR_PENDING_PRESENT;
        if (c->fence<=r->completed) return RSX_SR_INVALID; /* collect required */
        wait_for->fence=c->fence;
    } else if (c->state!=RSX_SR_FREE) return RSX_SR_INVALID;
    if (backbuffer<RSX_SR_BACKBUFFERS && r->backbuffer_fence[backbuffer]>r->completed &&
        r->backbuffer_fence[backbuffer]>wait_for->fence)
        wait_for->fence=r->backbuffer_fence[backbuffer];
    if (wait_for->fence) return RSX_SR_WAIT;
    if (c->generation==UINT64_MAX) return RSX_SR_OVERFLOW;
    c->generation++; c->state=RSX_SR_RECORDING; c->fence=0;
    c->backbuffer=backbuffer; c->is_present=0; c->present_decided=1;
    c->present_ok=0; c->work_signaled=0; c->work_fence=0; c->guest_sequence=0;
    memset(c->used,0,sizeof(c->used));
    lease->slot=r->next_slot; lease->generation=c->generation;
    return RSX_SR_OK;
}
/* Offscreen/label batches need no DXGI buffer. Claim a buffer before recording
 * its first actual write; do not block offscreen work merely because some
 * unrelated backbuffer is still owned by presentation. */
static inline int rsx_sr_bind_backbuffer(rsx_submission_ring *r, rsx_sr_lease l,
    uint32_t backbuffer, rsx_sr_gpu_ticket *wait_for)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || c->state!=RSX_SR_RECORDING ||
        backbuffer>=RSX_SR_BACKBUFFERS || !wait_for ||
        (c->backbuffer!=RSX_SR_NO_BACKBUFFER && c->backbuffer!=backbuffer)) return RSX_SR_INVALID;
    wait_for->fence=r->backbuffer_fence[backbuffer]>r->completed ? r->backbuffer_fence[backbuffer] : 0;
    if (wait_for->fence) return RSX_SR_WAIT;
    c->backbuffer=backbuffer; return RSX_SR_OK;
}
static inline int rsx_sr_allocate(rsx_submission_ring *r, rsx_sr_lease l,
    enum rsx_sr_arena arena, uint64_t size, uint64_t alignment, uint64_t *offset)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || c->state!=RSX_SR_RECORDING ||
        (unsigned)arena>=RSX_SR_ARENAS || !offset || !alignment ||
        (alignment&(alignment-1))) return RSX_SR_INVALID;
    uint64_t at=c->used[arena];
    if (at>UINT64_MAX-(alignment-1)) return RSX_SR_OVERFLOW;
    at=(at+alignment-1)&~(alignment-1);
    if (at>r->capacity[arena] || size>r->capacity[arena]-at) return RSX_SR_FULL;
    c->used[arena]=at+size; *offset=at;
    return RSX_SR_OK;
}
static inline int rsx_sr_retire(rsx_submission_ring *r, rsx_sr_lease l, void *reference)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || c->state!=RSX_SR_RECORDING || !reference) return RSX_SR_INVALID;
    if (c->retired_count==RSX_SR_RETIRED_CAP) return RSX_SR_FULL;
    c->retired[c->retired_count++]=reference;
    return RSX_SR_OK;
}
/* Reserve before Close/Execute. No allocation or retirement after preparation.
 * If Close fails before Execute, cancel_before_execute is safe. Once Execute
 * has been called, a failed Signal MUST poison the ring instead of cancel. */
static inline int rsx_sr_prepare(rsx_submission_ring *r, rsx_sr_lease l,
    int is_present, uint64_t guest_sequence, rsx_sr_gpu_ticket *ticket)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || c->state!=RSX_SR_RECORDING || !ticket) return RSX_SR_INVALID;
    if (is_present && c->backbuffer>=RSX_SR_BACKBUFFERS) return RSX_SR_INVALID;
    const uint64_t count=is_present ? 2u : 1u;
    if (r->last_reserved>UINT64_MAX-1-count) return RSX_SR_OVERFLOW;
    c->work_fence=r->last_reserved+1;
    r->last_reserved+=count; c->fence=r->last_reserved; c->is_present=!!is_present;
    c->guest_sequence=guest_sequence; c->present_decided=!is_present;
    c->state=RSX_SR_PREPARED; ticket->fence=c->fence;
    return RSX_SR_OK;
}
/* ExecuteCommandLists has no HRESULT. Mark it executed immediately after the
 * real call. A context can no longer be cancelled once its GPU work exists. */
static inline int rsx_sr_mark_executed(rsx_submission_ring *r, rsx_sr_lease l,
    rsx_sr_gpu_ticket ticket)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || c->state!=RSX_SR_PREPARED || c->fence!=ticket.fence)
        return RSX_SR_INVALID;
    c->state=RSX_SR_EXECUTED; return RSX_SR_OK;
}
/* Optional pre-Present Signal for render-output observers/timestamp queries.
 * This ticket NEVER authorizes allocator, backbuffer or arena reuse. */
static inline int rsx_sr_note_work_signal(rsx_submission_ring *r, rsx_sr_lease l,
    rsx_sr_work_ticket ticket)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || c->state!=RSX_SR_EXECUTED || !c->is_present ||
        c->present_decided || c->work_signaled || c->work_fence!=ticket.fence ||
        ticket.fence<=r->last_submitted) return RSX_SR_INVALID;
    c->work_signaled=1; r->last_submitted=ticket.fence; return RSX_SR_OK;
}
/* Queue Signal must occur AFTER Present has been enqueued for a frame, like
 * D3D12HelloFrameBuffering::MoveToNextFrame. A render-work fence emitted before
 * Present cannot prove DXGI backbuffer ownership has been returned. */
static inline int rsx_sr_commit_signal(rsx_submission_ring *r, rsx_sr_lease l,
    rsx_sr_gpu_ticket ticket)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || c->state!=RSX_SR_EXECUTED || !c->present_decided || c->fence!=ticket.fence ||
        ticket.fence<=r->last_submitted) return RSX_SR_INVALID;
    r->last_submitted=ticket.fence;
    if (c->backbuffer<RSX_SR_BACKBUFFERS) r->backbuffer_fence[c->backbuffer]=ticket.fence;
    c->state=RSX_SR_SUBMITTED; r->next_slot=(l.slot+1)%RSX_SR_CONTEXTS;
    return RSX_SR_OK;
}
static inline int rsx_sr_present_result(rsx_submission_ring *r, rsx_sr_lease l,
    rsx_sr_gpu_ticket ticket, int succeeded)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || (succeeded!=0 && succeeded!=1) || c->state!=RSX_SR_EXECUTED || !c->is_present ||
        c->fence!=ticket.fence || c->present_decided) return RSX_SR_INVALID;
    c->present_ok=!!succeeded; c->present_decided=1;
    return RSX_SR_OK;
}
static inline int rsx_sr_cancel_before_execute(rsx_submission_ring *r, rsx_sr_lease l)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (!c || r->fault || (c->state!=RSX_SR_RECORDING && c->state!=RSX_SR_PREPARED))
        return RSX_SR_INVALID;
    /* A retired reference may belong to earlier queue work. Protect that
     * prefix even though this recording itself was never submitted. */
    c->fence=r->last_submitted; c->is_present=0; c->present_decided=1;
    c->state=RSX_SR_SUBMITTED;
    return RSX_SR_OK;
}
static inline int rsx_sr_signal_failed_after_execute(rsx_submission_ring *r, rsx_sr_lease l)
{
    rsx_sr_context *c=rsx_sr_get(r,l);
    if (r && r->fault) return r->fault;
    if (!c || (c->state!=RSX_SR_PREPARED && c->state!=RSX_SR_EXECUTED)) return RSX_SR_INVALID;
    c->state=RSX_SR_POISONED; r->fault=RSX_SR_FAULT;
    return RSX_SR_FAULT;
}
/* Resize/shutdown first waits for this ticket, observes and collects. No
 * reset may reuse recording/prepared/poisoned resources. Device-loss teardown
 * is an external device-stop contract, not a forged completed fence. */
static inline int rsx_sr_drain_ticket(const rsx_submission_ring *r, rsx_sr_gpu_ticket *ticket)
{
    if (!r || !ticket) return RSX_SR_INVALID;
    if (r->fault) return r->fault;
    for (uint32_t i=0;i<RSX_SR_CONTEXTS;i++)
        if (r->contexts[i].state==RSX_SR_RECORDING || r->contexts[i].state==RSX_SR_PREPARED ||
            r->contexts[i].state==RSX_SR_EXECUTED)
            return RSX_SR_INVALID;
    ticket->fence=r->last_submitted;
    return ticket->fence>r->completed ? RSX_SR_WAIT : RSX_SR_OK;
}
static inline int rsx_sr_reconfigure(rsx_submission_ring *r,
    const uint64_t capacity[RSX_SR_ARENAS])
{
    if (!r || !capacity) return RSX_SR_INVALID;
    if (r->fault) return r->fault;
    for (uint32_t i=0;i<RSX_SR_CONTEXTS;i++)
        if (r->contexts[i].state!=RSX_SR_FREE || r->contexts[i].retired_count)
            return RSX_SR_INVALID;
    memcpy(r->capacity,capacity,sizeof(r->capacity));
    for (uint32_t i=0;i<RSX_SR_CONTEXTS;i++) memset(r->contexts[i].used,0,sizeof(r->contexts[i].used));
    return RSX_SR_OK;
}
static inline int rsx_sr_frontend_copied(rsx_submission_ring *r,
    rsx_sr_frontend_ticket ticket)
{
    if (!r || r->fault || ticket.sequence<r->frontend_copied) return RSX_SR_INVALID;
    r->frontend_copied=ticket.sequence; return RSX_SR_OK;
}
#endif
