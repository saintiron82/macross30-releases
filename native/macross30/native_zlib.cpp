/* Native Edge zlib inflate consumer (see native_zlib.h for the element).
 *
 * Replaces what each edgezlib_inflate_task SPU task does per popped element:
 * fetch the input, inflate (or copy stored data), write the middle
 * output_size bytes, then the completion protocol of 0x3220..0x3284:
 *   counter EA != 0: atomic decrement (GETLLAR/PUTLLC, 0xA9C0, returns the
 *     old value); old & 0x7FFFFFFF == 0 is the "already at zero" error, == 1
 *     means this was the last element -> set the event flag;
 *   counter EA == 0: set the event flag after every element;
 *   event flag EA == 0: nothing is set.
 * The flag is set with cellSpursEventFlagSet(flag, bits) (0x9070). On an SPU
 * error the task first ORs 0x80000000 into the counter (0xAA80); the host
 * never takes that path -- any element it cannot reproduce exactly is handed
 * to the SPU queue instead, so the SPU still reports it as before. */
#include "native_zlib.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
extern uint8_t* vm_base;
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t);
int spu_coh_is_reserved(uint32_t);
int32_t cellSpursEventFlagSet(void*, uint16_t);
}

namespace {

uint32_t bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
uint32_t guest_u32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
bool guest_range(uint32_t ea, uint64_t size)
{
    return ea && uint64_t(ea) + size <= 0x100000000ull;
}

std::atomic<int> g_mode{-1};
unsigned g_threads = 3;
std::once_flag g_mode_once;

void read_mode()
{
    /* Verify by default: the SPU still inflates, the native result is only
     * compared. The save-menu stall once blamed on the native consumer was the
     * coalesced GCM user command (fixed 2026-10-27); native output goes back
     * on once a play session shows verify ok with no mismatch.
     * MACROSS_NATIVE_ZLIB=1 / native opts in now, 0 / off turns it off. */
    int mode = MACROSS_ZLIB_VERIFY;
    const char* e = getenv("MACROSS_NATIVE_ZLIB");
    if (e && *e) {
        if (!strcmp(e, "1") || !_stricmp(e, "native")) mode = MACROSS_ZLIB_NATIVE;
        else if (!strcmp(e, "0") || !_stricmp(e, "off")) mode = MACROSS_ZLIB_OFF;
    }
    if (FILE* f = fopen("no_native_zlib", "rb")) { fclose(f); mode = MACROSS_ZLIB_OFF; }
    if (const char* t = getenv("MACROSS_NATIVE_ZLIB_THREADS")) {
        const int n = atoi(t);
        if (n >= 1 && n <= 8) g_threads = unsigned(n);
    }
    g_mode = mode;
    fprintf(stderr, "[native-zlib] mode=%s threads=%u\n",
            mode == MACROSS_ZLIB_NATIVE ? "native" : mode == MACROSS_ZLIB_VERIFY ? "verify" : "off",
            g_threads);
}

/* Same contract as an SPU PUT (spu_dma.h): a span touching a reserved line is
 * copied and announced under the lock-line lock so no PUTLLC commits over it. */
void guest_put(uint32_t ea, const uint8_t* src, uint32_t size)
{
    if (!size) return;
    const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
    bool reserved = false;
    for (uint32_t a = a0;; a += 128u) {
        if (spu_coh_is_reserved(a)) { reserved = true; break; }
        if (a == a1) break;
    }
    if (reserved) spu_lockline_lock();
    memmove(vm_base + ea, src, size);
    for (uint32_t a = a0;; a += 128u) {
        if (spu_coh_is_reserved(a)) {
            if (!reserved) spu_lockline_lock();
            spu_coh_notify_write(a);
            if (!reserved) spu_lockline_unlock();
        }
        if (a == a1) break;
    }
    if (reserved) spu_lockline_unlock();
}

/* 0xA9C0: GETLLAR/PUTLLC decrement of the BE word; returns the old value.
 * The CAS matches the PPU's lwarx/stwcx emulation; the lock-line lock keeps
 * it out of any SPU PUTLLC window on the same line. */
uint32_t guest_atomic_dec(uint32_t ea)
{
    spu_lockline_lock();
    std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(vm_base + ea));
    uint32_t raw = word.load(std::memory_order_acquire), old;
    do {
        old = bswap32(raw);
    } while (!word.compare_exchange_weak(raw, bswap32(old - 1u), std::memory_order_acq_rel,
                                         std::memory_order_acquire));
    spu_coh_notify_write(ea);
    spu_lockline_unlock();
    return old;
}

struct Job { uint32_t queue; uint8_t raw[32]; };
/* Leaked on purpose: detached workers still wait on it while static
 * destructors run at process exit. */
struct Pool {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool started = false;
};
Pool& pool()
{
    static Pool* p = new Pool;
    return *p;
}
std::atomic<unsigned long long> g_done, g_fallback, g_in_bytes, g_out_bytes;

void complete(const macross_zlib_element& e)
{
    if (e.ea_counter) {
        const uint32_t old = guest_atomic_dec(e.ea_counter) & 0x7FFFFFFFu;
        if (old == 0) {
            /* The SPU prints this (0x3364) and skips the flag. */
            fprintf(stderr, "[native-zlib] EDGE ZLIB ERROR: Attempted to decrement a work-to-do "
                            "counter already at zero (counter=%08X)\n", e.ea_counter);
            return;
        }
        if (old != 1) return;
    }
    if (!e.ea_event_flag) return;
    const int32_t rc = cellSpursEventFlagSet(reinterpret_cast<void*>(uintptr_t(e.ea_event_flag)),
                                             e.event_bits);
    if (rc) fprintf(stderr, "[native-zlib] cellSpursEventFlagSet(%08X, %04X) failed %08X\n",
                    e.ea_event_flag, e.event_bits, uint32_t(rc));
}

void fallback(const Job& j, const macross_zlib_element& e, int rc)
{
    const unsigned long long n = ++g_fallback;
    if (n <= 32 || n % 256 == 0)
        fprintf(stderr, "[native-zlib] fallback#%llu rc=%d in=%08X/%u out=%08X/%u type=%s "
                        "skip=%u/%u -> SPU queue\n", n, rc, e.ea_input, e.input_size, e.ea_output,
                e.output_size, e.deflate ? "deflate" : "stored", e.skip_begin, e.skip_end);
    macross_lf_publish(j.queue, j.raw);
}

/* Validates guest ranges and decodes. Returns 0 with out and len set, or the
 * reason the element must go to the SPU. */
int decode_guest(const macross_zlib_element& e, std::vector<uint8_t>& scratch,
                 const uint8_t** out, uint32_t* len)
{
    const uint64_t total = macross_zlib_element_total(&e);
    if (!guest_range(e.ea_input, e.input_size)) return -10;
    if (total > (64u << 20)) return -11;
    if (e.ea_counter & 3u) return -12;     /* the SPU halts on this (0xA9E8) */
    if (scratch.size() < total) scratch.resize(size_t(total));
    const int rc = macross_zlib_element_decode(&e, vm_base + e.ea_input, scratch.data(), out, len);
    if (rc) return rc;
    if (!guest_range(e.ea_output, *len)) return -13;
    return 0;
}

void run_job(const Job& j, std::vector<uint8_t>& scratch)
{
    macross_zlib_element e;
    macross_zlib_element_parse(j.raw, &e);
    const uint8_t* out = nullptr;
    uint32_t len = 0;
    const int rc = decode_guest(e, scratch, &out, &len);
    if (rc) { fallback(j, e, rc); return; }
    guest_put(e.ea_output, out, len);
    complete(e);
    g_in_bytes += e.input_size;
    g_out_bytes += len;
    const unsigned long long n = ++g_done;
    if (n <= 8 || n % 1024 == 0)
        fprintf(stderr, "[native-zlib] done#%llu in=%08X/%u out=%08X/%u type=%s counter=%08X "
                        "flag=%08X/%04X total in=%lluKB out=%lluKB fallback=%llu\n",
                n, e.ea_input, e.input_size, e.ea_output, len, e.deflate ? "deflate" : "stored",
                e.ea_counter, e.ea_event_flag, e.event_bits, g_in_bytes.load() >> 10,
                g_out_bytes.load() >> 10, g_fallback.load());
}

void worker()
{
    std::vector<uint8_t> scratch;
    for (;;) {
        Job j;
        {
            Pool& p = pool();
            std::unique_lock<std::mutex> guard(p.lock);
            p.cv.wait(guard, [&p] { return !p.jobs.empty(); });
            j = p.jobs.front();
            p.jobs.pop_front();
        }
        run_job(j, scratch);
    }
}

/* Verify mode: native decode at push time, compared once the SPU batch is
 * done (counter back to 0, or the flag itself for counter-less elements). */
struct Shadow { macross_zlib_element e; std::vector<uint8_t> data; };
std::mutex g_verify_lock;
std::vector<Shadow> g_shadows;
std::atomic<size_t> g_shadow_count;
size_t g_shadow_bytes;
unsigned long long g_ok, g_mismatch, g_native_fail, g_dropped;

void verify_report()
{
    fprintf(stderr, "[native-zlib] verify ok=%llu mismatch=%llu native_fail=%llu dropped=%llu "
                    "pending=%zu\n", g_ok, g_mismatch, g_native_fail, g_dropped, g_shadows.size());
}

void verify_record(const uint8_t raw[32])
{
    Shadow s;
    macross_zlib_element_parse(raw, &s.e);
    std::vector<uint8_t> scratch;
    const uint8_t* out = nullptr;
    uint32_t len = 0;
    const int rc = decode_guest(s.e, scratch, &out, &len);
    std::lock_guard<std::mutex> guard(g_verify_lock);
    if (rc) {
        if (++g_native_fail <= 32)
            fprintf(stderr, "[native-zlib] verify native-fail rc=%d in=%08X/%u out=%08X/%u type=%s\n",
                    rc, s.e.ea_input, s.e.input_size, s.e.ea_output, s.e.output_size,
                    s.e.deflate ? "deflate" : "stored");
        return;
    }
    s.data.assign(out, out + len);
    /* Bound the shadow memory; a batch the SPU never finishes must not grow it. */
    while (!g_shadows.empty() && g_shadow_bytes + len > (64u << 20)) {
        g_shadow_bytes -= g_shadows.front().data.size();
        g_shadows.erase(g_shadows.begin());
        ++g_dropped;
    }
    g_shadow_bytes += len;
    g_shadows.push_back(std::move(s));
    g_shadow_count = g_shadows.size();
}

} // namespace

extern "C" int macross_native_zlib_mode(void)
{
    std::call_once(g_mode_once, read_mode);
    return g_mode;
}

extern "C" int macross_native_zlib_submit(uint32_t queue, const uint8_t raw[32])
{
    const int mode = macross_native_zlib_mode();
    if (mode == MACROSS_ZLIB_VERIFY) { verify_record(raw); return 0; }
    if (mode != MACROSS_ZLIB_NATIVE) return 0;
    Job j;
    j.queue = queue;
    memcpy(j.raw, raw, 32);
    Pool& p = pool();
    {
        std::lock_guard<std::mutex> guard(p.lock);
        if (!p.started) {
            p.started = true;
            for (unsigned i = 0; i < g_threads; ++i) std::thread(worker).detach();
        }
        p.jobs.push_back(j);
    }
    p.cv.notify_one();
    return 1;
}

extern "C" void macross_native_zlib_after_wait(uint32_t event_flag)
{
    if (g_mode != MACROSS_ZLIB_VERIFY || !g_shadow_count) return;
    std::lock_guard<std::mutex> guard(g_verify_lock);
    bool report = false;
    for (size_t i = 0; i < g_shadows.size();) {
        const Shadow& s = g_shadows[i];
        const bool ready = s.e.ea_counter ? (guest_u32(s.e.ea_counter) & 0x7FFFFFFFu) == 0
                                          : s.e.ea_event_flag == event_flag;
        if (!ready) { ++i; continue; }
        const uint8_t* guest = vm_base + s.e.ea_output;
        if (!memcmp(guest, s.data.data(), s.data.size())) {
            if (++g_ok <= 4 || g_ok % 256 == 0) report = true;
        } else {
            size_t at = 0;
            while (at < s.data.size() && guest[at] == s.data[at]) ++at;
            ++g_mismatch;
            report = true;
            fprintf(stderr, "[native-zlib] MISMATCH in=%08X/%u out=%08X/%zu type=%s skip=%u/%u "
                            "first_diff=%zu spu=%02X native=%02X counter=%08X\n",
                    s.e.ea_input, s.e.input_size, s.e.ea_output, s.data.size(),
                    s.e.deflate ? "deflate" : "stored", s.e.skip_begin, s.e.skip_end, at,
                    guest[at], s.data[at], s.e.ea_counter ? guest_u32(s.e.ea_counter) : 0u);
        }
        g_shadow_bytes -= s.data.size();
        g_shadows.erase(g_shadows.begin() + ptrdiff_t(i));
    }
    g_shadow_count = g_shadows.size();
    if (report) verify_report();
}
