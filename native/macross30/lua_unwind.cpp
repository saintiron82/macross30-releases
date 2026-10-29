#include "lua_unwind.h"
#include "recompiled/ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <exception>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
namespace {
thread_local MacrossLuaBoundary* current_boundary;
bool probing() {
    const char* e = std::getenv("MACROSS_LUA_PROBE");
    return e && *e && std::strcmp(e, "0");
}
void terminate_probe() noexcept {
    std::fprintf(stderr, "[lua-unwind] std::terminate entered; active=%p\n", (void*)current_boundary);
    if (auto e = std::current_exception()) {
        try { std::rethrow_exception(e); }
        catch (const MacrossLuaJump& j) {
            std::fprintf(stderr, "[lua-unwind] terminating with MacrossLuaJump target=%p\n", (void*)j.target);
        }
        catch (const std::exception& e) { std::fprintf(stderr, "[lua-unwind] terminating exception: %s\n", e.what()); }
        catch (...) { std::fprintf(stderr, "[lua-unwind] terminating unknown exception\n"); }
    }
#ifdef _WIN32
    void* frames[32]; unsigned count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    std::fprintf(stderr, "[lua-unwind] terminate host RVAs:");
    for (unsigned i = 0; i < count; ++i)
        std::fprintf(stderr, " %llX", (unsigned long long)((uintptr_t)frames[i] - base));
    std::fputc('\n', stderr);
#endif
    std::fflush(stderr);
    std::abort();
}
}

MacrossLuaBoundary::MacrossLuaBoundary(void* context, uint32_t buffer)
    : context_(context), buffer_((buffer + 15u) & ~15u),
      previous_(current_boundary), trampoline_(g_trampoline_fn) {
    if (probing()) {
        static const bool installed = [] { std::set_terminate(terminate_probe); return true; }();
        (void)installed;
    }
    current_boundary = this;
}
MacrossLuaBoundary::~MacrossLuaBoundary() {
    if (current_boundary != this) {
        std::fprintf(stderr, "[lua-unwind] FATAL boundary retirement mismatch this=%p current=%p env=%08X\n",
                     (void*)this, (void*)current_boundary, buffer_);
        std::fflush(stderr);
        std::abort();
    }
    current_boundary = previous_;
}
bool MacrossLuaBoundary::accepts(const MacrossLuaJump& jump) const {
    if (probing()) {
        static std::atomic<unsigned> count;
        if (count.fetch_add(1) < 12)
            std::fprintf(stderr, "[lua-unwind] catch boundary=%p target=%p env=%08X match=%d\n",
                         (void*)this, (void*)jump.target, buffer_, jump.target == this);
        std::fflush(stderr);
    }
    if (jump.target != this) return false;
    /* Discard work scheduled by an abandoned frame, preserving any work
     * already pending when this protected region was entered. */
    g_trampoline_fn = trampoline_;
    return true;
}
[[noreturn]] void macross_lua_guest_longjmp(void* context, uint32_t aligned_buffer) {
    for (auto* b = current_boundary; b; b = b->previous_) {
        if (b->context_ == context && b->buffer_ == aligned_buffer) {
            if (probing()) {
                static std::atomic<unsigned> count;
                if (count.fetch_add(1) < 12)
                    std::fprintf(stderr, "[lua-unwind] throwing env=%08X context=%p target=%p current=%p\n",
                                 aligned_buffer, context, (void*)b, (void*)current_boundary);
                std::fflush(stderr);
            }
            throw MacrossLuaJump{b};
        }
    }
    std::fprintf(stderr, "[lua-unwind] unmatched guest jump buffer=%08X context=%p\n",
                 aligned_buffer, context);
    std::fflush(stderr);
    std::abort();
}
