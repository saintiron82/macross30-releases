/* Host scheduling only, at the reviewed queue callback/no-result retry edge.
 * No guest completion, reservation, register, LS or PC is modified. */
#include "spu/spu_context.h"
#include "spu_queue_backoff.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef MACROSS_QUEUE_BACKOFF_TEST
#include <windows.h>
#define queue_now() GetTickCount64()
#define queue_sleep() Sleep(1)
#define queue_host() GetCurrentThreadId()
#else
extern uint64_t queue_test_now(void);
extern void queue_test_sleep(void);
#define queue_now() queue_test_now()
#define queue_sleep() queue_test_sleep()
#define queue_host() 0u
#endif

typedef struct queue_backoff_state {
    int initialized, enabled;
    unsigned interval, streak;
    struct spu_context* owner;
    const void* local_store;
    uint32_t queue_key[4];
    uint64_t calls, sleeps, resets, guard_rejects, last_report;
} queue_backoff_state;
static _Thread_local queue_backoff_state qstate;

static void queue_config(void)
{
    if (qstate.initialized) return;
    qstate.initialized = 1;
    const char* e = getenv("MACROSS_JOB_QUEUE_BACKOFF");
    qstate.enabled = e && !strcmp(e, "1");
    qstate.interval = 64;
    e = getenv("MACROSS_JOB_QUEUE_BACKOFF_EVERY");
    if (e && *e) {
        char* end = NULL;
        unsigned long n = strtoul(e, &end, 10);
        if (*end == 0 && n >= 1 && n <= 4096) qstate.interval = (unsigned)n;
    }
    qstate.last_report = queue_now();
}

static int queue_guard(const struct spu_context* ctx)
{
    return ctx && ctx->image_id == 433707155 && ctx->ls &&
        ctx->ls[0xA134] == 0x32 && ctx->ls[0xA135] == 0x7F &&
        ctx->ls[0xA136] == 0x81 && ctx->ls[0xA137] == 0x00;
}

void macross_spu_queue_retry(struct spu_context* ctx)
{
    queue_config();
    if (!qstate.enabled) return;
    if (!queue_guard(ctx)) { qstate.streak = 0; qstate.guard_rejects++; return; }
    if (qstate.owner != ctx || qstate.local_store != ctx->ls ||
        memcmp(qstate.queue_key, &ctx->gpr[82], sizeof qstate.queue_key)) {
        qstate.owner = ctx;
        qstate.local_store = ctx->ls;
        memcpy(qstate.queue_key, &ctx->gpr[82], sizeof qstate.queue_key);
        qstate.streak = 0;
        qstate.resets++;
    }
    qstate.calls++;
    if (++qstate.streak >= qstate.interval) {
        qstate.streak = 0;
        qstate.sleeps++;
        queue_sleep();
    }
    const uint64_t now = queue_now();
    if (now - qstate.last_report >= 5000) {
        fprintf(stderr, "[job-queue-backoff] host=%lu image=433707155 pc=0A134 "
            "every=%u calls=%llu sleeps=%llu resets=%llu guard_rejects=%llu\n",
            (unsigned long)queue_host(), qstate.interval,
            (unsigned long long)qstate.calls, (unsigned long long)qstate.sleeps,
            (unsigned long long)qstate.resets, (unsigned long long)qstate.guard_rejects);
        qstate.last_report = now;
    }
}

void macross_spu_queue_activity(struct spu_context* ctx)
{
    /* All paths leaving the original queue operation converge at A140.
     * A new call starts a new logical retry streak, even on a reused context. */
    if (qstate.owner == ctx) { qstate.streak = 0; qstate.resets++; }
}
