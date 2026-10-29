/* Optional exact-content job fingerprint memoization. Include in one dispatch
 * translation unit. Images obey the existing stable-descriptor contract; this
 * is not synchronization against concurrent guest mutation. */
#ifndef SPU_JOB_FINGERPRINT_CACHE_H
#define SPU_JOB_FINGERPRINT_CACHE_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/win32_compat.h"

#define SPU_JOB_FP_CACHE_SLOTS 16u
#define SPU_JOB_FP_CACHE_MAX_BYTES (256u * 1024u)
typedef struct spu_job_fp_cache_entry {
    const void* image;
    size_t size;
    uint8_t* snapshot;
    uint64_t fingerprint;
} spu_job_fp_cache_entry;
static spu_job_fp_cache_entry s_job_fp_cache[SPU_JOB_FP_CACHE_SLOTS];
static unsigned s_job_fp_cache_next;
static SRWLOCK s_job_fp_cache_lock = SRWLOCK_INIT;
static INIT_ONCE s_job_fp_cache_once = INIT_ONCE_STATIC_INIT;
static int s_job_fp_cache_ready, s_job_fp_cache_enabled;
uint64_t spu_workload_fingerprint(const void* data, size_t n);

static BOOL CALLBACK spu_job_fp_cache_init(PINIT_ONCE once, PVOID arg, PVOID* context)
{
    (void)once; (void)arg; (void)context;
    const char* value = getenv("SPU_JOB_FP_CACHE");
    s_job_fp_cache_enabled = value && value[0] == '1' && value[1] == 0;
    __atomic_store_n(&s_job_fp_cache_ready, 1, __ATOMIC_RELEASE);
    return TRUE;
}

static inline int spu_job_fp_cache_on(void)
{
    if (!__atomic_load_n(&s_job_fp_cache_ready, __ATOMIC_ACQUIRE))
        InitOnceExecuteOnce(&s_job_fp_cache_once, spu_job_fp_cache_init, NULL, NULL);
    return s_job_fp_cache_enabled;
}

static inline uint64_t spu_job_fingerprint_cached(const void* image, size_t size)
{
    if (!spu_job_fp_cache_on() || !image || !size || size > SPU_JOB_FP_CACHE_MAX_BYTES)
        return spu_workload_fingerprint(image, size);
    AcquireSRWLockShared(&s_job_fp_cache_lock);
    for (unsigned i = 0; i < SPU_JOB_FP_CACHE_SLOTS; i++) {
        const spu_job_fp_cache_entry* entry = &s_job_fp_cache[i];
        if (entry->snapshot && entry->image == image && entry->size == size &&
            memcmp(image, entry->snapshot, size) == 0) {
            const uint64_t fingerprint = entry->fingerprint;
            ReleaseSRWLockShared(&s_job_fp_cache_lock);
            return fingerprint;
        }
    }
    ReleaseSRWLockShared(&s_job_fp_cache_lock);
    uint8_t* snapshot = (uint8_t*)calloc(1, size);
    if (!snapshot) return spu_workload_fingerprint(image, size);
    memcpy(snapshot, image, size);
    const uint64_t fingerprint = spu_workload_fingerprint(snapshot, size);
    AcquireSRWLockExclusive(&s_job_fp_cache_lock);
    /* Never publish a hash paired with bytes different from the caller's
     * current buffer. Concurrent guest writes remain outside this contract. */
    if (memcmp(image, snapshot, size) != 0) {
        ReleaseSRWLockExclusive(&s_job_fp_cache_lock);
        free(snapshot);
        return spu_workload_fingerprint(image, size);
    }
    unsigned target = SPU_JOB_FP_CACHE_SLOTS;
    for (unsigned i = 0; i < SPU_JOB_FP_CACHE_SLOTS; i++) {
        spu_job_fp_cache_entry* entry = &s_job_fp_cache[i];
        if (entry->snapshot && entry->image == image && entry->size == size) {
            if (memcmp(image, entry->snapshot, size) == 0) {
                const uint64_t existing = entry->fingerprint;
                ReleaseSRWLockExclusive(&s_job_fp_cache_lock);
                free(snapshot);
                return existing;
            }
            target = i; /* Same-address mutation replaces the stale incarnation. */
            break;
        }
        if (!entry->snapshot && target == SPU_JOB_FP_CACHE_SLOTS) target = i;
    }
    if (target == SPU_JOB_FP_CACHE_SLOTS)
        target = s_job_fp_cache_next++ % SPU_JOB_FP_CACHE_SLOTS;
    spu_job_fp_cache_entry* entry = &s_job_fp_cache[target];
    free(entry->snapshot); /* Readers cannot retain a pointer after unlocking. */
    entry->image = image;
    entry->size = size;
    entry->snapshot = snapshot;
    entry->fingerprint = fingerprint;
    ReleaseSRWLockExclusive(&s_job_fp_cache_lock);
    return fingerprint;
}
#endif
