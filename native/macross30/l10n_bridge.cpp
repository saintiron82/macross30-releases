/* Exact cellL10n guest ABI, based on RPCS3 cellL10n.cpp/.h:
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellL10n.cpp
 * The generic SDK shim uses incompatible code IDs and six arguments.
 * This bridge supports only the title's CP932/UTF8 path, with strict input.
 */
#include "l10n_bridge.h"
#include "recompiled/ppu_recomp.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));

namespace {
constexpr uint32_t utf8 = 0, sjis = 24, max_bytes = 64 * 1024 * 1024;
constexpr int32_t ok = 0, illegal = 1, exhausted = 2, unknown = 3;
bool supported(uint32_t code) { return code == utf8 || code == sjis; }
UINT host_code(uint32_t code) { return code == sjis ? 932 : CP_UTF8; }
bool accessible(uint32_t ea, uint32_t bytes, bool write = false) {
    if (!ea || uint64_t(ea) + bytes > 0x100000000ULL || bytes > max_bytes) return false;
    uintptr_t cursor = reinterpret_cast<uintptr_t>(vm_base + ea), end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD p = info.Protect & 255;
        const bool writable = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
        if (!writable && (write || (p != PAGE_READONLY && p != PAGE_EXECUTE_READ))) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
bool decode(uint32_t code, const unsigned char* source, uint32_t bytes, std::vector<wchar_t>& wide) {
    if (!bytes) { wide.clear(); return true; }
    const int count = MultiByteToWideChar(host_code(code), MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(source), int(bytes), nullptr, 0);
    if (!count) return false;
    wide.resize(count);
    return MultiByteToWideChar(host_code(code), MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(source), int(bytes), wide.data(), count) == count;
}
void converter(ppu_context* ctx) {
    const uint32_t src = uint32_t(ctx->gpr[3]), dst = uint32_t(ctx->gpr[4]);
    ctx->gpr[3] = supported(src) && supported(dst) ? (src << 16) | dst : 0xffffffffU;
}
void convert_guest(ppu_context* ctx, bool fixed_sjis) {
    const uint32_t handle = fixed_sjis ? sjis << 16 : uint32_t(ctx->gpr[3]);
    const unsigned shift = fixed_sjis ? 0 : 1;
    const uint32_t src = uint32_t(ctx->gpr[3 + shift]);
    const uint32_t src_len = uint32_t(ctx->gpr[4 + shift]);
    const uint32_t dst = uint32_t(ctx->gpr[5 + shift]);
    const uint32_t dst_len = uint32_t(ctx->gpr[6 + shift]);
    if (!accessible(src_len, 4) || !accessible(dst_len, 4, true)) { ctx->gpr[3] = illegal; return; }
    const uint32_t bytes = vm_read32(src_len), capacity = vm_read32(dst_len);
    if (bytes > max_bytes || !accessible(src, bytes) || (dst && !accessible(dst, capacity, true))) {
        ctx->gpr[3] = illegal; return;
    }
    uint32_t produced = capacity;
    const int32_t result = macross_l10n_convert_bytes(handle >> 16, handle & 65535,
        vm_base + src, bytes, dst ? vm_base + dst : nullptr, capacity, &produced);
    if (result == ok) vm_write32(dst_len, produced);
    ctx->gpr[3] = result;
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 8)
        std::fprintf(stderr, "[macross-l10n] cd=%08X src=%08X bytes=%u dst=%08X capacity=%u output=%u result=%d\n",
            handle, src, bytes, dst, capacity, produced, result);
}
void convert(ppu_context* ctx) { convert_guest(ctx, false); }
void sjis_to_utf8(ppu_context* ctx) { convert_guest(ctx, true); }
void detect(ppu_context* ctx) {
    const uint32_t src = uint32_t(ctx->gpr[3]);
    const int32_t length = int32_t(ctx->gpr[4]);
    if (length < 0 || !accessible(src, uint32_t(length))) { ctx->gpr[3] = 1u << 17; return; }
    bool ascii = true;
    for (int32_t n = 0; n < length; ++n) if (vm_base[src + n] >= 128) { ascii = false; break; }
    if (ascii) { ctx->gpr[3] = 2; return; }
    std::vector<wchar_t> wide;
    // Valid UTF8 is already the title's target encoding. CP932 is selected only
    // when UTF8 validation fails; never report successful decoding of garbage.
    if (decode(utf8, vm_base + src, uint32_t(length), wide)) ctx->gpr[3] = 1u << 5;
    else if (decode(sjis, vm_base + src, uint32_t(length), wide)) ctx->gpr[3] = 1u << 4;
    else ctx->gpr[3] = 1u << 16;
}
}

extern "C" int32_t macross_l10n_convert_bytes(uint32_t source_code, uint32_t target_code,
    const unsigned char* source, uint32_t source_bytes, unsigned char* target,
    uint32_t target_capacity, uint32_t* target_bytes) {
    if (!supported(source_code) || !supported(target_code)) return unknown;
    if (!target_bytes || (!source && source_bytes) || source_bytes > max_bytes) return illegal;
    if (!source_bytes) { *target_bytes = 0; return ok; }
    std::vector<wchar_t> wide;
    if (!decode(source_code, source, source_bytes, wide)) return illegal;
    BOOL substituted = FALSE;
    const DWORD flags = target_code == utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL* used_default = target_code == utf8 ? nullptr : &substituted;
    const int count = WideCharToMultiByte(host_code(target_code), flags, wide.data(), int(wide.size()), nullptr, 0, nullptr, used_default);
    if (!count || substituted) return illegal;
    if (target && uint32_t(count) > target_capacity) return exhausted;
    if (target) {
        // Stage output to support overlapping guest source/destination exactly.
        std::vector<char> bytes(count);
        if (WideCharToMultiByte(host_code(target_code), flags, wide.data(), int(wide.size()), bytes.data(), count, nullptr, used_default) != count || substituted) return illegal;
        std::memcpy(target, bytes.data(), count);
    }
    *target_bytes = uint32_t(count);
    return ok;
}
extern "C" void macross_l10n_register(void) {
    ps3_hle_register_ctx(0x1906CE6B, "jstrnchk exact Macross encoding", detect);
    ps3_hle_register_ctx(0xCC1633CC, "l10n_get_converter exact Macross ABI", converter);
    ps3_hle_register_ctx(0xA298CAD2, "l10n_convert_str exact five-argument ABI", convert);
    ps3_hle_register_ctx(0x33435818, "SJISstoUTF8s exact Macross ABI", sjis_to_utf8);
}
extern "C" void macross_l10n_install(void) { macross_l10n_register(); }
