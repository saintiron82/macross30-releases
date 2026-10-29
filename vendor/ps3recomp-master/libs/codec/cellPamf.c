/*
 * ps3recomp - cellPamf HLE implementation
 *
 * Parses PAMF container headers to extract stream information.
 * The actual PAMF header format is parsed to provide correct stream
 * counts and codec parameters to cellDmux/cellVdec/cellAdec.
 */

#include "cellPamf.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* Real PAMF0040/0041 byte layout, per RPCS3 cellPamf.h. Header and data
 * sizes are2048-byte units. Sequence at50,48-bit PTS high56/low58 and high5C/low5E.
 * One grouping period/group is supported here; unsupported layouts fail. */
#define PAMF_OFF_MAGIC 0
#define PAMF_OFF_VERSION 4
#define PAMF_OFF_DATA_OFFSET 8
#define PAMF_OFF_DATA_SIZE 12
#define PAMF_OFF_STREAM_TABLE 0x88
#define PAMF_OFF_NUM_STREAMS 0x87
#define PAMF_OFF_PRESENT_START 0x56
#define PAMF_OFF_PRESENT_END 0x5C
#define PAMF_OFF_MUX_RATE 0x62
#define PAMF_OFF_EP_OFFSET 0x90
#define PAMF_OFF_EP_COUNT 0x94
#define PAMF_MIN_HEADER_SIZE 0x88
#define PAMF_STREAM_DESC_SIZE 48
#define PAMF_RAW_TYPE_AVC 0x1B
#define PAMF_RAW_TYPE_M2V 0x02
#define PAMF_RAW_TYPE_ATRAC3PLUS 0xDC
#define PAMF_RAW_TYPE_AC3 0x81
#define PAMF_RAW_TYPE_LPCM 0x80
#define PAMF_RAW_TYPE_USERDATA 0xDD

/* ---------------------------------------------------------------------------
 * Endian helpers (PAMF is big-endian)
 * -----------------------------------------------------------------------*/
static u32 be32(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static u64 be64(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u64)be32(b) << 32) | be32(b + 4);
}

static u16 be16(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u16)b[0] << 8) | b[1];
}

/* ---------------------------------------------------------------------------
 * Raw-type to SDK-type conversion
 * -----------------------------------------------------------------------*/
static s32 raw_type_to_sdk(u8 rawType)
{
    switch (rawType) {
    case PAMF_RAW_TYPE_AVC:        return CELL_PAMF_STREAM_TYPE_AVC;
    case PAMF_RAW_TYPE_M2V:        return CELL_PAMF_STREAM_TYPE_M2V;
    case PAMF_RAW_TYPE_ATRAC3PLUS: return CELL_PAMF_STREAM_TYPE_ATRAC3PLUS;
    case PAMF_RAW_TYPE_AC3:        return CELL_PAMF_STREAM_TYPE_AC3;
    case PAMF_RAW_TYPE_LPCM:       return CELL_PAMF_STREAM_TYPE_PAMF_LPCM;
    case PAMF_RAW_TYPE_USERDATA:   return CELL_PAMF_STREAM_TYPE_USER_DATA;
    default:                       return -1;
    }
}

/* Host metadata is keyed by opaque128-byte guest reader EA; never store
 * host pointers into the guest reader. Queries do not create uninitialized state. */
#define PAMF_MAX_READERS 16
static struct { u32 ea; int in_use; CellPamfReader r; } s_readers[PAMF_MAX_READERS];
static int pamf_guest_span_access(u64 ea,u64 size,int writable)
{
    if(!vm_base||!ea||!size||ea>=0x100000000ULL||size>0x100000000ULL-ea)return 0;
#ifdef _WIN32
    uintptr_t at=(uintptr_t)vm_base+(uintptr_t)ea,end=at+(uintptr_t)size;
    while(at<end){MEMORY_BASIC_INFORMATION m;
        if(!VirtualQuery((const void*)at,&m,sizeof m)||m.State!=MEM_COMMIT||
           (m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return 0;
        if(writable&&!(m.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return 0;
        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=at)return 0;at=next<end?next:end;}
#endif
    return 1;
}
static int pamf_guest_span(u64 ea,u64 size){return pamf_guest_span_access(ea,size,0);}
int pamf_guest_buffer_writable(u32 ea,u32 size){return pamf_guest_span_access(ea,size,1);}
static CellPamfReader* pamf_host(CellPamfReader* guest_ea)
{
    if((uintptr_t)guest_ea>UINT32_MAX)return NULL;
    u32 ea=(u32)(uintptr_t)guest_ea;
    if(!ea)return NULL;
    for(int i=0;i<PAMF_MAX_READERS;i++)if(s_readers[i].in_use&&s_readers[i].ea==ea)return &s_readers[i].r;
    return NULL;
}
void pamf_reader_forget(u32 ea)
{
    for(int i=0;i<PAMF_MAX_READERS;i++)if(s_readers[i].in_use&&s_readers[i].ea==ea){
        memset(&s_readers[i],0,sizeof s_readers[i]);return;}
}
static CellPamfReader* pamf_allocate(u32 ea)
{
    CellPamfReader* old=pamf_host((CellPamfReader*)(uintptr_t)ea);if(old)return old;
    for(int i=0;i<PAMF_MAX_READERS;i++)if(!s_readers[i].in_use){s_readers[i].in_use=1;s_readers[i].ea=ea;return &s_readers[i].r;}
    return NULL;
}

/* --------------------------------------------------------------------------- 
 * Reader lifecycle
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderInitialize(CellPamfReader* reader,void* pamfAddr,u64 pamfSize,u32 attribute)
{
    u32 out=(u32)(uintptr_t)reader,header=(u32)(uintptr_t)pamfAddr;
    if((uintptr_t)reader>UINT32_MAX||(uintptr_t)pamfAddr>UINT32_MAX||
       !pamf_guest_buffer_writable(out,CELL_PAMF_READER_GUEST_SIZE)||!pamf_guest_span(header,PAMF_MIN_HEADER_SIZE))
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    pamf_reader_forget(out);
    const u8* base=vm_base+header;
    u32 version=be32(base+4);u64 hs=(u64)be32(base+8)*2048,ds=(u64)be32(base+12)*2048;
    if(be32(base)!=CELL_PAMF_MAGIC||(version!=0x30303430&&version!=0x30303431)||
       hs<PAMF_MIN_HEADER_SIZE||hs>UINT32_MAX||ds>UINT32_MAX||
       (pamfSize&&hs+ds>pamfSize)||!pamf_guest_span(header,hs))return (s32)CELL_PAMF_ERROR_INVALID_HEADER;
    u64 start=((u64)be16(base+0x56)<<32)|be32(base+0x58),end=((u64)be16(base+0x5C)<<32)|be32(base+0x5E);
    u32 streams=base[0x87];
    if(end<start||base[0x6F]!=1||base[0x81]!=1||!streams||
       0x88ull+(u64)streams*48>hs)return (s32)CELL_PAMF_ERROR_INVALID_HEADER;
    CellPamfReader* r=pamf_allocate(out);if(!r)return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
    memset(r,0,sizeof *r);r->pamfAddr=(void*)base;r->pamfSize=pamfSize;r->dataOffset=(u32)hs;r->dataSize=(u32)ds;
    r->streamTableOffset=0x88;r->numStreams=(u8)streams;
    /* Exact128-byte reader prefix: headerSize,dataSize,attribute,isPsmf,version,
     * grouping/group/stream indexes, then five guest pointers and reserved. */
    memset(vm_base+out,0,CELL_PAMF_READER_GUEST_SIZE);
    vm_write64(out,hs);vm_write64(out+8,ds);vm_write32(out+16,attribute);
    vm_write32(out+20,version==0x30303431?41:40);
    vm_write32(out+36,header);vm_write32(out+40,header+0x50);
    vm_write32(out+44,header+0x70);vm_write32(out+48,header+0x82);vm_write32(out+52,header+0x88);
    return CELL_OK;
}

s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, u64* startTime)
{
    reader = pamf_host(reader);
    if (!reader || !pamf_guest_buffer_writable((u32)(uintptr_t)startTime,8) || !reader->pamfAddr)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    vm_write64((u32)(uintptr_t)startTime, ((u64)be16(base + PAMF_OFF_PRESENT_START)<<32)|be32(base+PAMF_OFF_PRESENT_START+2));
    return CELL_OK;
}

s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, u64* endTime)
{
    reader = pamf_host(reader);
    if (!reader || !pamf_guest_buffer_writable((u32)(uintptr_t)endTime,8) || !reader->pamfAddr)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    vm_write64((u32)(uintptr_t)endTime, ((u64)be16(base + PAMF_OFF_PRESENT_END)<<32)|be32(base+PAMF_OFF_PRESENT_END+2));
    return CELL_OK;
}

u32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader)
{
    reader = pamf_host(reader);
    if (!reader)
        return 0;

    const u8* base = (const u8*)reader->pamfAddr;
    return be32(base + PAMF_OFF_MUX_RATE);
}

/* ---------------------------------------------------------------------------
 * Stream queries
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetNumberOfStreams(CellPamfReader* reader)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    return reader->numStreams;
}

/* Get pointer to raw stream descriptor bytes by absolute index */
static const u8* get_stream_desc_raw(CellPamfReader* reader, u32 index)
{
    if (!reader || index >= reader->numStreams)
        return NULL;

    const u8* base = (const u8*)reader->pamfAddr;
    return base + reader->streamTableOffset + index * PAMF_STREAM_DESC_SIZE;
}

/* Get SDK stream type for a given absolute stream index */
static s32 get_stream_sdk_type(CellPamfReader* reader, u32 index)
{
    const u8* desc = get_stream_desc_raw(reader, index);
    if (!desc) return -1;
    return raw_type_to_sdk(desc[0]);
}

s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    s32 count = 0;
    for (u32 i = 0; i < reader->numStreams; i++) {
        if (get_stream_sdk_type(reader, i) == (s32)streamType)
            count++;
    }
    return count;
}

s32 cellPamfReaderSetStreamWithType(CellPamfReader* reader, u8 streamType, u32 streamIndex)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    u32 found = 0;
    for (u32 i = 0; i < reader->numStreams; i++) {
        if (get_stream_sdk_type(reader, i) == (s32)streamType) {
            if (found == streamIndex) {
                reader->currentStream = (u8)i;
                return CELL_OK;
            }
            found++;
        }
    }
    return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
}

s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader* reader, u8 streamType, u32 channel)
{
    /* Forward the GUEST pointer untouched: cellPamfReaderSetStreamWithType
     * resolves it itself, and pamf_host keys on the guest EA -- handing it an
     * already-resolved host pointer would key a second, empty slot. */
    return cellPamfReaderSetStreamWithType(reader, streamType, channel);
}

s32 cellPamfReaderSetStreamWithIndex(CellPamfReader* reader, u32 streamIndex)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    if (streamIndex >= reader->numStreams)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    reader->currentStream = (u8)streamIndex;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Sample rate / channel lookup helpers for audio descriptors
 * -----------------------------------------------------------------------*/
static u32 audio_sample_rate(u8 code)
{
    switch (code) {
    case 1: return 44100;
    case 2: return 48000;
    default: return 48000;
    }
}

static u8 audio_channels(u8 code)
{
    switch (code) {
    case 1: return 1;
    case 2: return 2;
    case 6: return 6;
    case 8: return 8;
    default: return 2;
    }
}

static u8 lpcm_bits(u8 code)
{
    switch (code) {
    case 1: return 16;
    case 2: return 24;
    default: return 16;
    }
}

/* ---------------------------------------------------------------------------
 * Stream info extraction — reads real codec parameters from descriptors
 *
 * Stream descriptor layout (24 bytes, big-endian):
 *   Byte  0:    stream type (raw: 1=AVC, 2=M2V, 3=ATRAC3+, 4=AC3, 5=LPCM)
 *   Byte  1:    channel index
 *   For AVC (type 1):
 *     Byte  2:  profileIdc (66=baseline, 77=main, 100=high)
 *     Byte  3:  levelIdc
 *     Byte  4:  frameMbsOnlyFlag (bit 7), videoSignalInfoFlag (bit 6)
 *     Byte  5:  frameRateCode (upper nibble), aspectRatioIdc (lower nibble)
 *     Bytes 6-7:   sarWidth  (be16)
 *     Bytes 8-9:   sarHeight (be16)
 *     Bytes 14-15:  horizontalSize (be16)
 *     Bytes 16-17:  verticalSize   (be16)
 *     Bytes 18-19:  frameCropLeft/Right
 *     Bytes 20-21:  frameCropTop/Bottom
 *   For ATRAC3+ (type 3) / AC3 (type 4):
 *     Byte 10:  channel configuration
 *     Byte 12:  sample rate code
 *   For LPCM (type 5):
 *     Byte 10:  channel configuration
 *     Byte 11:  bits-per-sample code
 *     Byte 12:  sample rate code
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize)
{
    reader = pamf_host(reader);
    if (!reader || !info)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    info = GUEST_PTR(info, void*);
    /* ponytail: the CellPamf*Info structs below are filled host-endian.
     * Their u8 fields are already right; the u16/u32 ones (horizontalSize,
     * samplingFreq, ...) still need a swap if a title ever reads them. */
    const u8* desc = get_stream_desc_raw(reader, reader->currentStream);
    if (!desc)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    s32 sdkType = raw_type_to_sdk(desc[0]);

    switch (sdkType) {
    case CELL_PAMF_STREAM_TYPE_AVC:
        if (infoSize >= sizeof(CellPamfAvcInfo)) {
            CellPamfAvcInfo* avc = (CellPamfAvcInfo*)info;
            memset(avc, 0, sizeof(*avc));
            avc->profileIdc            = desc[2];
            avc->levelIdc              = desc[3];
            avc->frameMbsOnlyFlag      = (desc[4] >> 7) & 1;
            avc->videoSignalInfoFlag   = (desc[4] >> 6) & 1;
            avc->frameRateCode         = (desc[5] >> 4) & 0x0F;
            avc->aspectRatioIdc        = desc[5] & 0x0F;
            avc->sarWidth              = be16(desc + 6);
            avc->sarHeight             = be16(desc + 8);
            avc->horizontalSize        = be16(desc + 14);
            avc->verticalSize          = be16(desc + 16);
            avc->frameCropLeftOffset   = desc[18];
            avc->frameCropRightOffset  = desc[19];
            avc->frameCropTopOffset    = desc[20];
            avc->frameCropBottomOffset = desc[21];

            printf("[cellPamf] AVC info: profile=%u level=%u %ux%u "
                   "frameRate=%u aspect=%u\n",
                   avc->profileIdc, avc->levelIdc,
                   avc->horizontalSize, avc->verticalSize,
                   avc->frameRateCode, avc->aspectRatioIdc);
        }
        break;

    case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS:
        if (infoSize >= sizeof(CellPamfAtrac3plusInfo)) {
            CellPamfAtrac3plusInfo* at3 = (CellPamfAtrac3plusInfo*)info;
            memset(at3, 0, sizeof(*at3));
            at3->numberOfChannels  = audio_channels(desc[10]);
            at3->samplingFrequency = audio_sample_rate(desc[12]);

            printf("[cellPamf] ATRAC3+ info: %u ch, %u Hz\n",
                   at3->numberOfChannels, at3->samplingFrequency);
        }
        break;

    case CELL_PAMF_STREAM_TYPE_PAMF_LPCM:
        if (infoSize >= sizeof(CellPamfLpcmInfo)) {
            CellPamfLpcmInfo* lpcm = (CellPamfLpcmInfo*)info;
            memset(lpcm, 0, sizeof(*lpcm));
            lpcm->numberOfChannels  = audio_channels(desc[10]);
            lpcm->bitsPerSample     = lpcm_bits(desc[11]);
            lpcm->samplingFrequency = audio_sample_rate(desc[12]);

            printf("[cellPamf] LPCM info: %u ch, %u Hz, %u bits\n",
                   lpcm->numberOfChannels, lpcm->samplingFrequency,
                   lpcm->bitsPerSample);
        }
        break;

    case CELL_PAMF_STREAM_TYPE_AC3:
        if (infoSize >= sizeof(CellPamfAc3Info)) {
            CellPamfAc3Info* ac3 = (CellPamfAc3Info*)info;
            memset(ac3, 0, sizeof(*ac3));
            ac3->numberOfChannels  = audio_channels(desc[10]);
            ac3->samplingFrequency = audio_sample_rate(desc[12]);

            printf("[cellPamf] AC3 info: %u ch, %u Hz\n",
                   ac3->numberOfChannels, ac3->samplingFrequency);
        }
        break;

    default:
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
    }

    return CELL_OK;
}

s32 cellPamfReaderGetStreamIndex(CellPamfReader* reader)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    return reader->currentStream;
}

s32 cellPamfStreamTypeToEsFilterId(u8 streamType, u8 streamIndex,
                                     void* esFilterId)
{
    (void)streamType; (void)streamIndex;
    if (!esFilterId)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    /* Set filter ID bytes (type + index) */
    u8* id = (u8*)esFilterId;
    id[0] = streamType;
    id[1] = streamIndex;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Entry point navigation
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetNumberOfEp(CellPamfReader* reader, s32* numEp)
{
    reader = pamf_host(reader);
    if (!reader || !numEp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    vm_write32((u32)(uintptr_t)numEp, be32(base + PAMF_OFF_EP_COUNT));
    return CELL_OK;
}

s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep)
{
    reader = pamf_host(reader);
    if (!reader || !ep)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    u32 epCount = be32(base + PAMF_OFF_EP_COUNT);
    if (epIndex >= epCount)
        return (s32)CELL_PAMF_ERROR_EP_NOT_FOUND;

    /* EP table entries are at epOffset, each 8 bytes (pts + offset) */
    u32 epTableOff = be32(base + PAMF_OFF_EP_OFFSET);
    const u8* epData = base + epTableOff + epIndex * 8;
    vm_write32((u32)(uintptr_t)ep + 0, be32(epData));
    vm_write32((u32)(uintptr_t)ep + 4, be32(epData + 4));
    return CELL_OK;
}
