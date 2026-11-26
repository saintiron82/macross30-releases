// Standalone: real bridge handlers, small committed guest mapping, no game runtime.
#define PPU_NO_INLINE_LOADS
#include "../native/macross30/recompiled/ppu_recomp.h"
#include "../native/macross30/l10n_bridge.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

extern "C" {
uint8_t* vm_base = nullptr;
int vm_inline_ok = 0;
uint32_t ppu_vm_size = 65536;
uint32_t ppu_hle_inject_base = 0;
uint32_t vm_read32(uint64_t ea) {
    const auto* p = vm_base + ea;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void vm_write32(uint64_t ea, uint32_t value) {
    auto* p = vm_base + ea;
    p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
}
}
static std::map<uint32_t, void (*)(ppu_context*)> handlers;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char*, void (*handler)(ppu_context*)) {
    handlers[nid] = handler;
}
static unsigned checks = 0;
static void require(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(2); }
}
static std::vector<unsigned char> read_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "fixture opens");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static uint32_t invoke(uint32_t nid, std::initializer_list<uint64_t> args) {
    ppu_context context{};
    unsigned reg = 3;
    for (auto arg : args) context.gpr[reg++] = arg;
    require(handlers.count(nid) == 1, "exact guest handler registered");
    handlers.at(nid)(&context);
    return uint32_t(context.gpr[3]);
}
static std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t n = 0; n <= value.size(); ++n) {
        if (n == value.size() || value[n] == delimiter) {
            fields.emplace_back(value.substr(start, n - start)); start = n + 1;
        }
    }
    return fields;
}
int main(int argc, char** argv) {
    require(argc == 3, "expected original CP932 and independent UTF8 fixture paths");
    const auto source = read_file(argv[1]), reference = read_file(argv[2]);
    require(source.size() == 1184, "complete original archive entry is 1184 bytes");
    require(reference.size() > source.size(), "Japanese headers expand in UTF8");
    vm_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, ppu_vm_size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(vm_base != nullptr, "guest mapping committed");
    constexpr uint32_t src = 0x1000, dst = 0x4000, src_length = 0x200, dst_length = 0x208;
    std::memcpy(vm_base + src, source.data(), source.size());
    macross_l10n_register();
    require(handlers.size() == 4, "all four exact ABI handlers registered");
    require((invoke(0x1906CE6B, {src, source.size()}) & 0x30) == 0x10,
        "original CSV detected as ShiftJIS by actual loader gate");
    const auto converter = invoke(0xCC1633CC, {24, 0});
    require(converter == 0x00180000, "PS3 code24-to-code0 converter handle");
    vm_write32(src_length, uint32_t(source.size())); vm_write32(dst_length, 0);
    require(vm_base[src_length + 2] == 4 && vm_base[src_length + 3] == 0xa0,
        "source length is guest big endian 000004A0");
    require(invoke(0xA298CAD2, {converter, src, src_length, 0, dst_length}) == 0,
        "five argument ABI supports null-output sizing");
    require(vm_read32(dst_length) == reference.size(), "sizing equals complete independent reference");
    require(vm_read32(src_length) == source.size(), "sizing preserves guest source counter");
    std::memset(vm_base + dst - 8, 0xa5, reference.size() + 16);
    require(invoke(0xA298CAD2, {converter, src, src_length, dst, dst_length}) == 0,
        "five argument ABI converts original CSV");
    require(vm_read32(src_length) == source.size() && vm_read32(dst_length) == reference.size(),
        "conversion counters obey guest ABI");
    require(std::memcmp(vm_base + dst, reference.data(), reference.size()) == 0,
        "entire converted file exactly matches independent Python CP932 reference");
    require(std::all_of(vm_base + dst - 8, vm_base + dst, [](uint8_t b) { return b == 0xa5; }) &&
        std::all_of(vm_base + dst + reference.size(), vm_base + dst + reference.size() + 8,
            [](uint8_t b) { return b == 0xa5; }), "conversion does not overwrite guard bytes");
    const std::string text(reinterpret_cast<char*>(vm_base + dst), reference.size());
    const std::string group_header = "\xe9\x9a\x8a\xe7\x95\xaa\xe5\x8f\xb7";
    auto lines = split(text, '\n');
    require(split(lines.front(), ',').at(12) == group_header, "exact UTF8 group header matches ELF");
    const std::map<std::string, int> expected{{"ENE101",1},{"ENE201",2},{"ENE301",3},
        {"ENEBOSS",4},{"DEMO_P0",1002},{"DEMO_P1",1002},{"DEMO_BOSS",1002}};
    unsigned records = 0;
    for (const auto& line : lines) {
        const auto columns = split(line, ',');
        if (columns.size() < 13 || columns[1] == "NAME") continue;
        require(expected.count(columns[1]) == 1, "record name belongs to original placement");
        require(std::stoi(columns[12]) == expected.at(columns[1]), "original record group survives conversion");
        ++records;
    }
    require(records == 7, "all seven original records parsed");
    // Exercise the separate four-argument fixed SJIS handler as well.
    vm_write32(dst_length, uint32_t(reference.size()));
    require(invoke(0x33435818, {src, src_length, dst, dst_length}) == 0 &&
        std::memcmp(vm_base + dst, reference.data(), reference.size()) == 0,
        "fixed SJIS ABI converts same complete file");
    vm_write32(dst_length, 3); std::memset(vm_base + dst, 0x5a, reference.size());
    require(invoke(0xA298CAD2, {converter, src, src_length, dst, dst_length}) == 2,
        "insufficient guest output returns destination exhausted");
    require(vm_read32(dst_length) == 3 && vm_read32(src_length) == source.size() &&
        std::all_of(vm_base + dst, vm_base + dst + reference.size(), [](uint8_t b) { return b == 0x5a; }),
        "exhaustion leaves counters and output unchanged");
    vm_base[src] = 0x82; vm_write32(src_length, 1); vm_write32(dst_length, 100);
    require(invoke(0xA298CAD2, {converter, src, src_length, dst, dst_length}) == 1,
        "truncated ShiftJIS lead byte reports source illegal");
    require(vm_read32(dst_length) == 100 && vm_read32(src_length) == 1 && vm_base[dst] == 0x5a,
        "invalid source preserves output and counters");
    require(invoke(0xA298CAD2, {converter, src, src_length, dst, 0}) == 1,
        "null guest output-counter pointer rejected");
    require(invoke(0xCC1633CC, {0xffff, 0}) == 0xffffffff,
        "unknown encoding has no fabricated converter");
    require(invoke(0xA298CAD2, {0xffffffff, src, src_length, dst, dst_length}) == 3,
        "unknown converter rejected by conversion handler");
    uint32_t count = 17;
    require(macross_l10n_convert_bytes(24, 0, nullptr, 1, nullptr, 0, &count) == 1,
        "null nonempty host source rejected");
    require(macross_l10n_convert_bytes(24, 0, source.data(), uint32_t(source.size()), nullptr, 0, nullptr) == 1,
        "null host result-length pointer rejected");
    require(VirtualFree(vm_base, 0, MEM_RELEASE) != 0, "guest test mapping released");
    vm_base = nullptr;
    std::printf("{\"passed\":true,\"game_started\":false,\"records_checked\":%u,"
        "\"checks\":%u,\"source_bytes\":%zu,\"converted_bytes\":%zu,"
        "\"complete_reference_equal\":true,\"guest_be_abi\":true}\n",
        records, checks, source.size(), reference.size());
    return 0;
}
