// Reuse the established standalone guest-memory/ABI fixture, never the game.
#define main original_l10n_fixture_main
#include "test_l10n_bridge.cpp"
#undef main
int main(int argc, char** argv) {
    require(argc==4,"expected original, UTF8 reference and translated CSV");
    auto original = read_file(argv[1]);
    auto reference = read_file(argv[2]);
    auto korean = read_file(argv[3]);
    ppu_vm_size = 512 * 1024;
    vm_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, ppu_vm_size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(vm_base != nullptr, "guest memory allocation");
    macross_l10n_register();
    constexpr uint32_t src=0x1000, dst=0x20000, src_len=0x200, dst_len=0x208;
    std::memcpy(vm_base+src, original.data(), original.size());
    require((invoke(0x1906CE6B, {src,original.size()}) & 0x30) == 0x10,
        "original field CSV selects CP932 loader branch");
    vm_write32(src_len,uint32_t(original.size())); vm_write32(dst_len,200000);
    require(invoke(0xA298CAD2, {0x00180000,src,src_len,dst,dst_len})==0,
        "actual bridge converts whole original field CSV");
    require(vm_read32(dst_len)==reference.size() &&
        std::memcmp(vm_base+dst,reference.data(),reference.size())==0,
        "original conversion preserves every CSV byte after decoding");
    std::memcpy(vm_base+src,korean.data(),korean.size());
    const auto detected = invoke(0x1906CE6B, {src,korean.size()}) & 0x30;
    require(detected == 0x20, "Korean field CSV detected as UTF8");
    // Match func_004E82B4 with target encoding 2: equal encodings use sentinel
    // 1, not l10n_get_converter(0,0). The old fixture skipped this guest branch.
    const auto csv_handle = detected == 0x10 ? invoke(0xCC1633CC, {24,0}) : 1;
    vm_write32(src_len,uint32_t(korean.size())); vm_write32(dst_len,0x18247b40);
    require(invoke(0xA298CAD2, {csv_handle,src,src_len,0,dst_len})==0 &&
        vm_read32(dst_len)==korean.size(),
        "real guest CSV identity sentinel sizes output despite stale capacity");
    vm_write32(src_len,uint32_t(korean.size())); vm_write32(dst_len,200000);
    require(invoke(0xA298CAD2, {csv_handle,src,src_len,dst,dst_len})==0 &&
        vm_read32(dst_len)==korean.size() &&
        std::memcmp(vm_base+dst,korean.data(),korean.size())==0,
        "UTF8 identity conversion retains Korean data exactly");
    require(vm_read32(src_len)==korean.size(), "identity preserves source counter");
    vm_write32(dst_len,3); std::memset(vm_base+dst,0xa5,korean.size());
    require(invoke(0xA298CAD2,{1,src,src_len,dst,dst_len})==2 &&
        vm_read32(dst_len)==3 && vm_read32(src_len)==korean.size() &&
        std::all_of(vm_base+dst,vm_base+dst+korean.size(),[](uint8_t b){return b==0xa5;}),
        "identity exhaustion leaves output and counters intact");
    std::memcpy(vm_base+src,original.data(),original.size());
    vm_write32(src_len,uint32_t(original.size())); vm_write32(dst_len,200000);
    require(invoke(0xA298CAD2,{1,src,src_len,dst,dst_len})==0 &&
        vm_read32(dst_len)==original.size() &&
        std::memcmp(vm_base+dst,original.data(),original.size())==0,
        "matching SJIS target also preserves original bytes");
    vm_write32(dst_len,200000);
    require(invoke(0xA298CAD2,{1,src,src_len,src+7,dst_len})==0 &&
        std::memcmp(vm_base+src+7,original.data(),original.size())==0,
        "overlapping identity copy is exact");
    require(VirtualFree(vm_base,0,MEM_RELEASE)!=0,"release guest memory");
    vm_base=nullptr;
    std::printf("{\"passed\":true,\"game_started\":false,\"checks\":%u,"
                "\"cp932_gate\":16,\"utf8_gate\":32,\"csv_bytes\":%zu}\n",checks,korean.size());
}

