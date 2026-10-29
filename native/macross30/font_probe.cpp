#include "font_probe.h"
#include "win32_compat.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

extern "C" {
extern uint8_t* vm_base;
void ppu_register_function(uint64_t address, void (*function)(ppu_context*));
}

namespace {
constexpr uint32_t kParser = 0x0019B7B4;
constexpr uint64_t kMaxDump = 8u * 1024u * 1024u;
constexpr unsigned kMaxCalls = 16;
std::atomic<unsigned> calls{0};
void (*original_parser)(ppu_context*) = nullptr;
thread_local bool wrapper_active = false;

bool readable(uint32_t address, uint64_t size)
{
    if (!vm_base || !address || !size || uint64_t(address) + size > 0x100000000ull)
        return false;
#ifdef _WIN32
    uintptr_t cursor = reinterpret_cast<uintptr_t>(vm_base) + address;
    const uintptr_t end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
#else
    // This diagnostic targets the current Windows port. Do not risk touching
    // an uncommitted guest page on hosts without VirtualQuery.
    return false;
#endif
}

void observe(ppu_context* ctx)
{
    if (!std::getenv("MACROSS_FONT_PROBE")) return;
    const unsigned index = calls.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMaxCalls) return;
    const uint32_t begin = uint32_t(ctx->gpr[4]);
    const uint32_t end = uint32_t(ctx->gpr[5]);
    const uint64_t size = end >= begin ? uint64_t(end) - begin : 0;
    uint32_t magic = 0;
    if (readable(begin, 4))
        magic = (uint32_t(vm_base[begin]) << 24) | (uint32_t(vm_base[begin + 1]) << 16) |
                (uint32_t(vm_base[begin + 2]) << 8) | vm_base[begin + 3];
    std::fprintf(stderr,
        "[font-probe] call=%u tid=%llu lr=%08X toc=%08X object=%08X begin=%08X end=%08X bytes=%llu magic=%08X\n",
        index, static_cast<unsigned long long>(ctx->thread_id), uint32_t(ctx->lr),
        uint32_t(ctx->gpr[2]), uint32_t(ctx->gpr[3]), begin, end,
        static_cast<unsigned long long>(size), magic);
    if (!size || size > kMaxDump || !readable(begin, size)) {
        std::fprintf(stderr, "[font-probe] call=%u dump skipped: invalid, uncommitted or over 8 MiB\n", index);
        return;
    }
    char path[96];
    // The bounded runner sets its working directory to this run's evidence
    // directory. No source ISO, VFS asset or translation is written here.
    std::snprintf(path, sizeof(path), "font_probe_%02u_%08X_%08X.bin", index, begin, magic);
    FILE* file = std::fopen(path, "wb");
    if (!file) {
        std::fprintf(stderr, "[font-probe] call=%u dump open failed\n", index);
        return;
    }
    const size_t written = std::fwrite(vm_base + begin, 1, size_t(size), file);
    const int close_result = std::fclose(file);
    std::fprintf(stderr, "[font-probe] call=%u dump=%s written=%llu complete=%d\n",
        index, path, static_cast<unsigned long long>(written), written == size && !close_result);
}

void parser_wrapper(ppu_context* ctx)
{
    observe(ctx);
    // If the generated parser also has the entry probe, avoid double counting
    // this single wrapper entry. Recursive direct parser calls remain visible.
    wrapper_active = true;
    original_parser(ctx);
    wrapper_active = false;
}
}

extern "C" void macross_font_probe_entry(ppu_context* ctx)
{
    if (wrapper_active) { wrapper_active = false; return; }
    observe(ctx);
}

extern "C" void macross_font_probe_register(void)
{
    if (!std::getenv("MACROSS_FONT_PROBE") || original_parser) return;
    for (uint64_t i = 0; i < function_table_count; ++i) {
        if (function_table[i].addr != kParser) continue;
        original_parser = function_table[i].func;
        ppu_register_function(kParser, parser_wrapper);
        std::fprintf(stderr, "[font-probe] registry wrapper installed; direct calls require entry probe\n");
        return;
    }
    std::fprintf(stderr, "[font-probe] parser missing from lifted function table\n");
}
