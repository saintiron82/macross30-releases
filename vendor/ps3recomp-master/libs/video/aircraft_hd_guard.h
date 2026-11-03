#ifndef AIRCRAFT_HD_GUARD_H
#define AIRCRAFT_HD_GUARD_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct {uint32_t width,levels,format,block;size_t offset[13],bytes[13];} aircraft_hd_dds;
static uint32_t ah_u32(const unsigned char* p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t ah_fnv(const unsigned char* p,size_t n){uint64_t h=UINT64_C(14695981039346656037);for(size_t i=0;i<n;i++)h=(h^p[i])*UINT64_C(1099511628211);return h;}
static int ah_dds(const unsigned char* p,size_t n,aircraft_hd_dds* d){
    if(!p||!d||n<128||memcmp(p,"DDS ",4)||ah_u32(p+4)!=124||ah_u32(p+76)!=32||ah_u32(p+112))return 0;
    uint32_t w=ah_u32(p+16),h=ah_u32(p+12),m=ah_u32(p+28),format=0,block=16;
    if(!memcmp(p+84,"DXT1",4)){format=0x86;block=8;}
    else if(!memcmp(p+84,"DXT3",4))format=0x87;
    else if(!memcmp(p+84,"DXT5",4))format=0x88;
    if(!format||w!=h||w<4||w>4096||(w&(w-1))||!m||m>13)return 0;
    uint32_t full=1;for(uint32_t a=w;a>1;a>>=1)full++;if(m!=full)return 0;
    memset(d,0,sizeof(*d));d->width=w;d->levels=m;d->format=format;d->block=block;
    size_t at=128;for(uint32_t i=0;i<m;i++){
        uint32_t a=w>>i;if(!a)a=1;size_t blocks=(a+3)/4,bytes=blocks*blocks*block;
        if(at>n||bytes>n-at)return 0;d->offset[i]=at;d->bytes[i]=bytes;at+=bytes;
    }return at==n;
}
static int ah_pair(const unsigned char* s,size_t sn,const unsigned char* h,size_t hn,aircraft_hd_dds* sd,aircraft_hd_dds* hd){
    return ah_dds(s,sn,sd)&&ah_dds(h,hn,hd)&&sd->format==hd->format&&hd->width>=sd->width&&hd->width<=sd->width*4;
}
static int ah_match(const unsigned char* source,const aircraft_hd_dds* d,const unsigned char* guest,size_t n){
    return guest&&n>=d->bytes[0]&&!memcmp(source+d->offset[0],guest,d->bytes[0]);
}
#endif
