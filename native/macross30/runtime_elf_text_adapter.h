#pragma once
#include "runtime_elf_text.h"
#include "code_signature.h"
// The lifter and SDK declare ABI-identical ppu_context separately. The main
// harness already included ppu_recomp.h; suppress only the SDK duplicate there.
#ifdef MACROSS_TEXT_LIFTED_CONTEXT
#ifndef PPU_CONTEXT_H
#define PPU_CONTEXT_H
#endif
#endif
#include "../../vendor/ps3recomp-master/runtime/syscalls/sys_memory.h"
#include <fstream>
#include <cstdio>
#include <iterator>
namespace macross_text {
inline std::vector<uint8_t> read_file(const char* path,size_t limit){
    std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("ELF text file unavailable");
    auto n=f.tellg();if(n<0||uint64_t(n)>limit)throw std::runtime_error("ELF text file size limit");
    std::vector<uint8_t> b(static_cast<size_t>(n));f.seekg(0);if(n&&!f.read(reinterpret_cast<char*>(b.data()),n))throw std::runtime_error("ELF text file read failed");return b;
}
inline uint64_t be64(const uint8_t* p){return uint64_t(be32(p))<<32|be32(p+4);}
inline std::vector<Range> elf_ranges(const std::vector<uint8_t>& b){
    if(b.size()<64||std::memcmp(b.data(),"\x7f" "ELF",4)||b[4]!=2||b[5]!=2)throw std::runtime_error("expected big-endian ELF64");
    uint64_t off=be64(b.data()+32);uint32_t stride=uint32_t(b[54])<<8|b[55],n=uint32_t(b[56])<<8|b[57];
    if(stride<56||off>b.size()||uint64_t(n)*stride>b.size()-off)throw std::runtime_error("ELF program headers invalid");
    std::vector<Range> out;
    for(uint32_t i=0;i<n;i++){auto p=b.data()+off+i*stride;if(be32(p)!=1)continue;
        uint64_t start=be64(p+16),size=be64(p+32),file=be64(p+8);
        if(start>UINT32_MAX||size>0x100000000ull-start||file>b.size()||size>b.size()-file)throw std::runtime_error("ELF segment invalid");
        if(size)out.push_back({start,start+size,(be32(p+4)&2)!=0});
    }return out;
}
inline const uint8_t* guest_read(void*,uint32_t a,uint32_t){return vm_base+a;}
inline uint8_t* guest_write(void*,uint32_t a,uint32_t){return vm_base+a;}
inline uint32_t tracked_allocate(void*,uint32_t n){
    // Boot-only, before PPU/SPU/frame threads. sys_memory_allocate owns locking,
    // image/TLS exclusions, commit, zeroing and allocation accounting.
    int slot=-1;for(int i=0;i<SYS_MEMORY_ALLOC_MAX;i++)if(!g_sys_mem_allocs[i].active){slot=i;break;}
    if(slot<0)return 0;
    ppu_context ctx{};ctx.gpr[3]=n;ctx.gpr[4]=SYS_MEMORY_PAGE_SIZE_64K;ctx.gpr[5]=0;
    if(sys_memory_allocate(&ctx)!=CELL_OK)return 0;
    const auto& a=g_sys_mem_allocs[slot];
    return a.active&&a.size>=n?a.addr:0;
}
// Caller resolves manifest paths and validates pack SHA256. Empty/null pack path
// leaves original guest data untouched. Any selected-pack failure aborts boot.
inline bool install(const char* pack_path,const char* baseline_elf){
    if(!pack_path||!*pack_path)return true;
    try{
        auto elf=read_file(baseline_elf,128*1024*1024),pack=read_file(pack_path,64*1024*1024);
        uint8_t hash[32];if(!macross_code_sha256(elf.data(),elf.size(),hash))throw std::runtime_error("ELF SHA256 unavailable");
        auto result=apply(pack,hash,elf_ranges(elf),{nullptr,guest_read,guest_write,tracked_allocate});
        std::fprintf(stderr,"[elf-text] test-only pack=%s records=%u relocated=%u inline=%u pool=0x%08X bytes=%u\n",pack_path,result.records,result.relocated,result.in_place,result.pool_address,result.pool_bytes);
        return true;
    }catch(const std::exception& e){std::fprintf(stderr,"[elf-text] rejected: %s\n",e.what());return false;}
}
} // namespace macross_text
