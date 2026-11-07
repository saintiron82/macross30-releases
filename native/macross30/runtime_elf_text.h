#pragma once
// Locale-agnostic external ELF text pack. Boot-only: apply before guest threads.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <stdexcept>
#include <limits>
#include <algorithm>
namespace macross_text {
struct Range { uint64_t start, end; bool writable; };
struct Memory {
    void* context;
    const uint8_t* (*read)(void*, uint32_t, uint32_t);
    uint8_t* (*write)(void*, uint32_t, uint32_t);
    uint32_t (*allocate)(void*, uint32_t); // tracked LV2 user allocation; process lifetime
};
struct Result { uint32_t records=0, relocated=0, in_place=0, pool_address=0, pool_bytes=0; };
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
inline bool utf8(const uint8_t* p, size_t n) {
    for(size_t i=0;i<n;) {
        uint32_t c=p[i++]; if(!c) return false; if(c<128) continue;
        unsigned k; uint32_t minimum;
        if(c>=0xC2 && c<=0xDF){k=1;c&=31;minimum=0x80;}
        else if(c>=0xE0&&c<=0xEF){k=2;c&=15;minimum=0x800;}
        else if(c>=0xF0&&c<=0xF4){k=3;c&=7;minimum=0x10000;}else return false;
        if(k>n-i)return false;
        while(k--){uint32_t t=p[i++];if((t&0xC0)!=0x80)return false;c=(c<<6)|(t&63);}
        if(c<minimum||c>0x10FFFF||(c>=0xD800&&c<=0xDFFF))return false;
    } return true;
}
struct Cursor {
    const uint8_t* p; size_t left;
    const uint8_t* take(size_t n){if(n>left)throw std::runtime_error("truncated ELF text pack");auto q=p;p+=n;left-=n;return q;}
    uint32_t u32(){auto q=take(4);return uint32_t(q[0])|uint32_t(q[1])<<8|uint32_t(q[2])<<16|uint32_t(q[3])<<24;}
};
inline bool covered(const std::vector<Range>& ranges,uint32_t a,uint32_t n,bool writable=false){
    for(auto r:ranges)if((!writable||r.writable)&&a>=r.start&&uint64_t(a)+n<=r.end)return true;
    return false;
}
struct Record { uint32_t source,capacity,destination=0; const uint8_t* expected; std::string text; std::vector<uint32_t> pointers; };
inline Result apply(const std::vector<uint8_t>& pack,const uint8_t baseline_sha[32],
                    const std::vector<Range>& image,Memory mem) {
    Cursor c{pack.data(),pack.size()};
    if(std::memcmp(c.take(8),"M30TXP1\0",8))throw std::runtime_error("ELF text pack magic/version");
    if(std::memcmp(c.take(32),baseline_sha,32))throw std::runtime_error("ELF text baseline SHA256 mismatch");
    uint32_t count=c.u32(),flags=c.u32();
    if(count>100000||flags!=1)throw std::runtime_error("ELF text count/test-only flags");
    std::vector<Record> rows; std::vector<std::pair<uint64_t,uint64_t>> regions;
    std::set<uint32_t> pointers; uint64_t pool_size=0;
    for(uint32_t i=0;i<count;i++) {
        Record r; r.source=c.u32();r.capacity=c.u32();uint32_t n=c.u32(),refs=c.u32();
        if(!r.capacity||r.capacity>1048576||!n||n>1048576||refs>100000||!covered(image,r.source,r.capacity))
            throw std::runtime_error("invalid ELF text record bounds");
        // Writable image strings can be inline fields whose address also
        // denotes a record or array base. Pointer-value equality cannot prove
        // const-char semantics for those addresses; keep their names in place.
        if(refs)for(auto range:image)
            if(range.writable&&uint64_t(r.source)<range.end&&range.start<uint64_t(r.source)+r.capacity)
                throw std::runtime_error("cannot relocate text from writable ELF image data");
        r.expected=c.take(r.capacity);auto t=c.take(n);
        if(!utf8(t,n))throw std::runtime_error("invalid UTF8 replacement");
        auto zero=static_cast<const uint8_t*>(std::memchr(r.expected,0,r.capacity));
        if(!zero||!utf8(r.expected,size_t(zero-r.expected)))throw std::runtime_error("invalid source text guard");
        const uint8_t* current=mem.read(mem.context,r.source,r.capacity);
        if(!current||std::memcmp(current,r.expected,r.capacity))throw std::runtime_error("ELF text live source guard mismatch");
        r.text.assign(reinterpret_cast<const char*>(t),n);
        regions.push_back({r.source,uint64_t(r.source)+r.capacity});
        for(uint32_t j=0;j<refs;j++){
            uint32_t p=c.u32();
            if((p&3)||!covered(image,p,4,true)||!pointers.insert(p).second)throw std::runtime_error("invalid/duplicate typed pointer");
            auto v=mem.read(mem.context,p,4);
            if(!v||be32(v)!=r.source)throw std::runtime_error("ELF text typed pointer guard mismatch");
            r.pointers.push_back(p);
        }
        if(refs){pool_size+=n+1;if(pool_size>16*1024*1024)throw std::runtime_error("ELF text pool too large");}
        else if(uint64_t(n)+1>r.capacity)throw std::runtime_error("unreferenced text cannot fit; use explicit fallback");
        rows.push_back(std::move(r));
    }
    if(c.left)throw std::runtime_error("ELF text trailing bytes");
    std::sort(regions.begin(),regions.end());
    for(size_t i=1;i<regions.size();i++)if(regions[i].first<regions[i-1].second)throw std::runtime_error("overlapping text slots");
    for(auto p:pointers)for(auto r:regions)if(p<r.second&&uint64_t(p)+4>r.first)throw std::runtime_error("pointer overlaps text slot");
    Result out;out.records=count;out.pool_bytes=uint32_t(pool_size);
    if(pool_size){out.pool_address=mem.allocate(mem.context,uint32_t(pool_size));
        if(out.pool_address<0x10000000u||uint64_t(out.pool_address)+pool_size>0x20000000ull)throw std::runtime_error("ELF text allocator returned unsafe range");
        for(auto r:image)if(out.pool_address<r.end&&uint64_t(out.pool_address)+pool_size>r.start)throw std::runtime_error("ELF text allocation overlaps image");
    }
    // Resolve all writable views before any text/pointer mutation.
    uint32_t offset=0;std::vector<uint8_t*> destinations;std::vector<std::pair<uint8_t*,uint32_t>> patches;
    for(auto& r:rows){r.destination=r.pointers.empty()?r.source:out.pool_address+offset;
        uint32_t bytes=r.pointers.empty()?r.capacity:uint32_t(r.text.size()+1);
        auto d=mem.write(mem.context,r.destination,bytes);if(!d)throw std::runtime_error("ELF text destination inaccessible");destinations.push_back(d);
        if(!r.pointers.empty()){offset+=uint32_t(r.text.size()+1);out.relocated++;}else out.in_place++;
        for(auto p:r.pointers){auto q=mem.write(mem.context,p,4);if(!q)throw std::runtime_error("ELF pointer inaccessible");patches.push_back({q,r.destination});}
    }
    for(size_t i=0;i<rows.size();i++){auto& r=rows[i];auto d=destinations[i];if(r.pointers.empty())std::memset(d,0,r.capacity);std::memcpy(d,r.text.data(),r.text.size());d[r.text.size()]=0;}
    for(auto q:patches){q.first[0]=uint8_t(q.second>>24);q.first[1]=uint8_t(q.second>>16);q.first[2]=uint8_t(q.second>>8);q.first[3]=uint8_t(q.second);}
    return out;
}
} // namespace macross_text
