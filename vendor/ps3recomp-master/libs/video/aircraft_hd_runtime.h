/* Host-only original GB texture reuse. Included after GPU upload helpers. */
#include "aircraft_hd_guard.h"
#include "aircraft_hd_pack_pin.h"
#include "rsx_null_backend.h"
typedef struct {const unsigned char *source,*hd;aircraft_hd_dds sd,hd_layout;uint64_t prefix;int logged;} aircraft_hd_entry;
static aircraft_hd_entry ah_entries[1024];
static uint32_t ah_count;
static INIT_ONCE ah_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK ah_init(PINIT_ONCE once,PVOID param,PVOID* context){
    (void)once;(void)param;(void)context;
    if(!rsx_null_backend_hd_textures_enabled())return TRUE;
    wchar_t path[32768];HANDLE file=INVALID_HANDLE_VALUE;
    const wchar_t* configured=_wgetenv(L"MACROSS_HD_TEXTURE_PACK");
    if(configured&&*configured)file=CreateFileW(configured,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    else {
        DWORD len=GetModuleFileNameW(NULL,path,32768);if(!len||len>=32768)return TRUE;
        wchar_t* tail=wcsrchr(path,L'\\');if(!tail||tail-path>32000)return TRUE;tail[1]=0;
        wcscat(path,L"assets\\aircraft-hd.pack");file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        if(file==INVALID_HANDLE_VALUE){tail[1]=0;wcscat(path,L"..\\assets\\aircraft-hd.pack");file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);}
    }
    if(file==INVALID_HANDLE_VALUE){fprintf(stderr,"[aircraft-hd] pack missing; original textures retained\n");return TRUE;}
    LARGE_INTEGER size;GetFileSizeEx(file,&size);
    if(size.QuadPart!=AIRCRAFT_HD_PACK_BYTES){CloseHandle(file);fprintf(stderr,"[aircraft-hd] rejected pack size\n");return TRUE;}
    HANDLE map=CreateFileMappingW(file,NULL,PAGE_READONLY,0,0,NULL);CloseHandle(file);if(!map)return TRUE;
    const unsigned char* data=(const unsigned char*)MapViewOfFile(map,FILE_MAP_READ,0,0,0);CloseHandle(map);if(!data)return TRUE;
    const size_t n=AIRCRAFT_HD_PACK_BYTES;uint32_t count=0;size_t at=12;
    if(n<12||memcmp(data,"M30HD001",8)||ah_fnv(data,n)!=AIRCRAFT_HD_PACK_FNV)goto rejected;
    count=ah_u32(data+8);if(count!=AIRCRAFT_HD_PACK_COUNT||count>1024)goto rejected;
    for(uint32_t i=0;i<count;i++){
        if(at>n||n-at<8)goto rejected;
        uint32_t sn=ah_u32(data+at),hn=ah_u32(data+at+4);at+=8;
        if(sn>n-at)goto rejected;const unsigned char* s=data+at;at+=sn;
        if(hn>n-at)goto rejected;const unsigned char* h=data+at;at+=hn;
        aircraft_hd_entry* e=&ah_entries[i];
        if(!ah_pair(s,sn,h,hn,&e->sd,&e->hd_layout))goto rejected;
        e->source=s;e->hd=h;e->prefix=ah_fnv(s+e->sd.offset[0],64);
    }
    if(at!=n)goto rejected;
    ah_count=count;fprintf(stderr,"[aircraft-hd] enabled maps=%u; preview textures in battle; guest unchanged\n",count);
    return TRUE; /* Keep the read-only mapping alive while entries point into it. */
rejected:
    ah_count=0;memset(ah_entries,0,sizeof ah_entries);UnmapViewOfFile(data);
    fprintf(stderr,"[aircraft-hd] rejected pack; original textures retained\n");return TRUE;
}
static ID3D12Resource* aircraft_hd_texture(const rsx_dsp_texture* t){
    InitOnceExecuteOnce(&ah_once,ah_init,NULL,NULL);
    if(!ah_count||t->cubemap||t->dimension!=2||t->width!=t->height||(t->format&TEX_FMT_UNNORM))return NULL;
    const u32 format=t->format&TEX_FMT_BASE_MASK;
    if(format!=TEX_FMT_DXT1&&format!=TEX_FMT_DXT23&&format!=TEX_FMT_DXT45)return NULL;
    const u32 span=((t->width+3)/4)*((t->height+3)/4)*(format==TEX_FMT_DXT1?8:16);
    const u8* guest=guest_ptr(t->location,t->offset,span);if(!guest||span<64)return NULL;
    const uint64_t prefix=ah_fnv(guest,64);
    for(u32 i=0;i<ah_count;i++){
        aircraft_hd_entry* e=&ah_entries[i];
        if(e->sd.width!=t->width||e->sd.format!=format||e->prefix!=prefix||!ah_match(e->source,&e->sd,guest,span))continue;
        tex_level_t levels[13];
        for(u32 j=0;j<e->hd_layout.levels;j++){
            u32 a=e->hd_layout.width>>j;if(!a)a=1;
            levels[j].w=(a+3)&~3u;levels[j].h=(a+3)&~3u;levels[j].data=e->hd+e->hd_layout.offset[j];
            levels[j].row_bytes=((a+3)/4)*e->hd_layout.block;levels[j].rows=(a+3)/4;
        }
        DXGI_FORMAT dxgi=format==TEX_FMT_DXT1?DXGI_FORMAT_BC1_UNORM:format==TEX_FMT_DXT23?DXGI_FORMAT_BC2_UNORM:DXGI_FORMAT_BC3_UNORM;
        ID3D12Resource* result=create_texture_mipped(dxgi,levels,e->hd_layout.levels);
        if(result&&!e->logged){e->logged=1;fprintf(stderr,"[aircraft-hd] matched map=%u fmt=%02X %u -> %u\n",i,format,e->sd.width,e->hd_layout.width);}
        return result;
    }return NULL;
}
