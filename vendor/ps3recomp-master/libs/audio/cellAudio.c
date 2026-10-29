/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ps3emu/log_first.h"

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;
static int                  s_wasapi_failed = 0;
static int                  s_wasapi_started = 0;
static void audio_backend_shutdown(void);

/* Latch a failed endpoint: do not keep submitting to a lost device or report
 * its failed packets as successful output. The mixer may still clock guest
 * notifications; actual output resumes only after a fresh Init. */
static int wasapi_failure(const char* operation, HRESULT hr)
{
    if (!s_wasapi_failed)
        fprintf(stderr, "[cellAudio] WASAPI %s failed: 0x%08lX; device output disabled until reinit\n",
                operation, (unsigned long)hr);
    s_wasapi_failed = 1;
    return -1;
}

static int audio_backend_init(void)
{
    HRESULT hr;
    s_wasapi_failed = 0;
    s_wasapi_started = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Request event-driven mode with a ~20ms buffer */
    REFERENCE_TIME buf_duration = 200000; /* 20ms in 100ns units */
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_wasapi_event) {
        wasapi_failure("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
        audio_backend_shutdown();
        return -1;
    }

    /* Shared mode only takes the device's own mix format unless the stream
     * asks the audio engine to convert: on a device not running 48 kHz stereo
     * float (a 44.1 kHz or 5.1 output) Initialize failed with
     * AUDCLNT_E_UNSUPPORTED_FORMAT and every title ran silent. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            convert, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        hr = s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
        if (FAILED(hr)) {
            wasapi_failure("SetEventHandle", hr);
            audio_backend_shutdown();
            return -1;
        }
    }

    hr = s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);
    if (FAILED(hr) || !s_wasapi_buf_frames) {
        wasapi_failure("GetBufferSize", FAILED(hr) ? hr : E_FAIL);
        audio_backend_shutdown();
        return -1;
    }

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        audio_backend_shutdown();
        return -1;
    }

    hr = s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    if (FAILED(hr)) {
        wasapi_failure("Start", hr);
        audio_backend_shutdown();
        return -1;
    }
    s_wasapi_started = 1;
    fprintf(stderr, "[cellAudio] WASAPI started: stereo float32 48000 Hz, buffer=%u frames, PS3_MUTE=%s\n",
            (unsigned)s_wasapi_buf_frames, getenv("PS3_MUTE") ? "enabled" : "unset");
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client && s_wasapi_started) {
        HRESULT hr = s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
        if (FAILED(hr)) wasapi_failure("Stop", hr);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
    s_wasapi_buf_frames = 0;
    s_wasapi_started = 0;
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render || s_wasapi_failed) {
        g_audio_dropped += num_samples;
        return;
    }

    UINT32 padding = 0;
    HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr) || padding > s_wasapi_buf_frames) {
        wasapi_failure("GetCurrentPadding(submit)", FAILED(hr) ? hr : E_FAIL);
        g_audio_dropped += num_samples;
        return;
    }

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        /* PS3_MUTE=1: silence at the device, nothing else changes -- the
         * title still gets a working cellAudio and the device still paces
         * it, unlike PS3_NO_AUDIO, which sends the title down its no-audio
         * path. For unattended runs. */
        static int mute = -1;
        if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        hr = s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                               mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        if (FAILED(hr)) {
            wasapi_failure("ReleaseBuffer", hr);
            g_audio_dropped += num_samples;
        }
    } else {
        wasapi_failure("GetBuffer", FAILED(hr) ? hr : E_POINTER);
        g_audio_dropped += num_samples;
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client || s_wasapi_failed) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr) || padding > s_wasapi_buf_frames) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        wasapi_failure("GetCurrentPadding(room)", FAILED(hr) ? hr : E_FAIL);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void audio_store_be_f32(float* p,float value)
{
    u32 v;memcpy(&v,&value,4);
    v=(v>>24)|((v>>8)&0xff00u)|((v<<8)&0xff0000u)|(v<<24);
    memcpy(p,&v,4);
}
/* g_vm_page_bitmap is a committed-page CACHE, not an allocation registry.
 * Host vm_commit does not seed it. On a miss query actual mapping; never commit
 * memory on behalf of an invalid Add pointer. Existing cached hits retain the
 * runtime's stable-page assumption; this is not a permission-change tracker. */
static int audio_pcm_guest_range(u64 ea,u64 bytes)
{
    extern uint8_t g_vm_page_bitmap[65536/8];
    if(!vm_base||!ea||!bytes||ea>=0x100000000ULL||bytes>0x100000000ULL-ea)return 0;
    u32 first=(u32)(ea>>16),last=(u32)((ea+bytes-1)>>16);
    for(u32 page=first;page<=last;page++) {
        if(__atomic_load_n(&g_vm_page_bitmap[page>>3],__ATOMIC_RELAXED)&(1u<<(page&7)))continue;
#ifdef _WIN32
        const uintptr_t base=(uintptr_t)vm_base+((uintptr_t)page<<16),page_end=base+0x10000u;
        uintptr_t at=(uintptr_t)vm_base+(uintptr_t)(ea>((u64)page<<16)?ea:((u64)page<<16));
        uintptr_t end=(uintptr_t)vm_base+(uintptr_t)(ea+bytes);
        if(end>page_end)end=page_end;
        int whole_page=0;
        while(at<end) {
            MEMORY_BASIC_INFORMATION mbi;
            if(!VirtualQuery((const void*)at,&mbi,sizeof mbi)||mbi.State!=MEM_COMMIT||
               (mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))||
               !(mbi.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return 0;
            uintptr_t region_end=(uintptr_t)mbi.BaseAddress+mbi.RegionSize;
            if(region_end<=at)return 0;
            if((uintptr_t)mbi.BaseAddress<=base&&region_end>=page_end)whole_page=1;
            at=region_end<end?region_end:end;
        }
        /* A partially committed64KiB page can validate the requested bytes,
         * but must not mark unrelated bytes as committed. */
        if(whole_page)__atomic_fetch_or(&g_vm_page_bitmap[page>>3],(uint8_t)(1u<<(page&7)),__ATOMIC_RELAXED);
#else
        return 0;
#endif
    }
    return 1;
}
/* AUDIO_ADD_TRACE=1: bounded provenance from actual HLE args to actual Add
 * bytes and mixer preclear bytes. No scans/clocks/RMW when disabled. Ring
 * bookkeeping is protected by the existing audio mutex; HLE row cap is atomic. */
static int s_add_trace_on = -1;
static u32 s_add_trace_hle_rows, s_add_trace_write_rows, s_add_trace_mix_rows;
static struct { u32 seq,hash; u64 ring_ea,ridx; } s_add_trace_ring[CELL_AUDIO_PORT_MAX][32];
static int audio_add_trace_enabled(void)
{
    int on=__atomic_load_n(&s_add_trace_on,__ATOMIC_RELAXED);
    if(on<0){const char* e=getenv("AUDIO_ADD_TRACE");on=e&&strcmp(e,"1")==0;
        __atomic_store_n(&s_add_trace_on,on,__ATOMIC_RELAXED);}
    return on;
}
static u32 audio_add_trace_hash(const void* bytes,u32 n)
{
    const unsigned char* p=(const unsigned char*)bytes;u32 h=2166136261u;
    for(u32 i=0;i<n;i++){h^=p[i];h*=16777619u;}return h;
}
/* Called after real HLE dispatch, including its invalid-width rejection. */
void audio_add_trace_hle(u32 nid,u64 port,u64 src,u64 samples,double fpr1,u32 lr,s32 rc)
{
    if(!audio_add_trace_enabled()||__atomic_load_n(&s_add_trace_hle_rows,__ATOMIC_RELAXED)>=128)return;
    u32 row=__atomic_fetch_add(&s_add_trace_hle_rows,1,__ATOMIC_RELAXED);if(row>=128)return;
    u64 raw;float gain=(float)fpr1;u32 gb;memcpy(&raw,&fpr1,8);memcpy(&gb,&gain,4);
    fprintf(stderr,"[audio-add-hle] row=%u nid=%08X lr=%08X port=%llu src=%016llX samples=%llu fpr1_bits=%016llX gain_bits=%08X rc=%08X\n",
        row+1,nid,lr,(unsigned long long)port,(unsigned long long)src,(unsigned long long)samples,
        (unsigned long long)raw,gb,(u32)rc);
}
/* Read-only diagnostic, independent of the committed-page cache. Query exact
 * Windows regions before bounded bytes; SEH covers a concurrent unmap/protect.
 * This never commits pages, invokes a guest helper, or walks arbitrary frames. */
static const char* audio_stop_read_u64(u64 ea,u64* value)
{
    *value=0;
    if(!vm_base||ea>=0x100000000ULL||ea>0x100000000ULL-8)return "range";
    if(ea&3)return "alignment";
#ifdef _WIN32
    uintptr_t at=(uintptr_t)vm_base+(uintptr_t)ea,end=at+8;
    if(end<at)return "host_overflow";
    while(at<end){MEMORY_BASIC_INFORMATION mbi;
        if(!VirtualQuery((const void*)at,&mbi,sizeof mbi)||mbi.State!=MEM_COMMIT)return "unmapped";
        if((mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))||
           !(mbi.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return "protection";
        uintptr_t next=(uintptr_t)mbi.BaseAddress+mbi.RegionSize;
        if(next<=at)return "mapping_overflow";at=next<end?next:end;
    }
    __try {
        u64 n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|vm_base[(u32)ea+i];*value=n;
    } __except(EXCEPTION_EXECUTE_HANDLER){return "read_fault";}
    return "ok";
#else
    return "platform_unavailable";
#endif
}
/* Only a statically identified prologue may authorize an additional frame. */
static const char* audio_stop_known_frame(u64 sp,u32 offset,u32 pc,u64 prologue,u64* saved)
{
    u64 code=0;const char* status=audio_stop_read_u64(pc,&code);
    if(strcmp(status,"ok"))return "prologue_unreadable";
    if(code!=prologue)return "prologue_mismatch";
    return audio_stop_read_u64(sp+offset,saved);
}
/* Caller provenance only: three statically verified frames, no generic walk.
 * 1CD784(-80)->1CD864(-70)->1CB740(-70), saved LR at oldSP+10.
 * Thus HLE SP+90/+100/+170. Gate on exact HLE LR and parent return addresses. */
void audio_add_trace_sail_stop(u64 self,u32 lr,u64 sp)
{
    static u32 rows;
    if(!audio_add_trace_enabled()||__atomic_load_n(&rows,__ATOMIC_RELAXED)>=8)return;
    u32 row=__atomic_fetch_add(&rows,1,__ATOMIC_RELAXED);if(row>=8)return;
    u64 saved[7]={0},code[3]={0};const char* status[3]={"unknown_prologue","unknown_prologue","unknown_prologue"};
    const u32 pc[3]={0x1CD78C,0x1CD864,0x1CB744};
    const u64 expected[3]={0xF821FF817C0802A6ULL,0xF821FF917C0802A6ULL,0x7C0802A6F821FF91ULL};
    const char* fingerprint[3]={"not_checked","not_checked","not_checked"};
    if(lr==0x1CD7E8u){
        for(unsigned i=0;i<3;i++){fingerprint[i]=audio_stop_read_u64(pc[i],&code[i]);
            if(!strcmp(fingerprint[i],"ok"))fingerprint[i]=code[i]==expected[i]?"match":"mismatch";}
        if(sp&15)status[0]=status[1]=status[2]="sp_alignment";
        else if(sp>0x100000000ULL-0x178)status[0]=status[1]=status[2]="sp_range";
        else {
            status[0]=audio_stop_read_u64(sp+0x90,&saved[0]);
            if(!strcmp(status[0],"ok")&&saved[0]==0x1CD874){
                status[1]=audio_stop_read_u64(sp+0x100,&saved[1]);
                if(!strcmp(status[1],"ok")&&saved[1]==0x1CB758)
                    status[2]=audio_stop_read_u64(sp+0x170,&saved[2]);
                else status[2]="parent_not_expected";
            }else status[1]=status[2]="parent_not_expected";
        }
    }
    const char* upper[4]={"not_branch_a","not_reached","not_reached","not_reached"};
    u32 seventh_offset=0;u64 request_r31=0,request_root=0,request_obj=0,request_flags=0,request_byte=0;
    const char* request_status="not_request_branch";
    if(!strcmp(status[2],"ok")&&saved[2]==0x1CB78C){
        upper[0]=audio_stop_known_frame(sp,0x200,0x1CB770,0xF821FF717C0802A6ULL,&saved[3]);
        if(!strcmp(upper[0],"ok")&&saved[3]==0xA0B60){
            upper[1]=audio_stop_known_frame(sp,0x290,0xA0B1C,0xF821FF717C0802A6ULL,&saved[4]);
            u32 caller= saved[4]==0xA3984?0xA392C:saved[4]==0x20DD80?0x20DC5C:
                (saved[4]==0x20EB68||saved[4]==0x20EB78)?0x20E784:0;
            if(!strcmp(upper[1],"ok")&&caller){
                upper[2]=audio_stop_known_frame(sp,0x320,caller,0xF821FF717C0802A6ULL,&saved[5]);
                u32 next_pc=0;u64 next_prologue=0;
                if(caller==0xA392C&&saved[5]==0x107654){next_pc=0x10762C;seventh_offset=0x390;next_prologue=0xF821FF917C0802A6ULL;}
                else if(caller==0xA392C&&saved[5]==0x1FAF48){next_pc=0x1FA890;seventh_offset=0x490;next_prologue=0xF821FE917C0802A6ULL;}
                else if(caller==0x20DC5C&&saved[5]==0x20E1BC){next_pc=0x20E140;seventh_offset=0x3A0;next_prologue=0xF821FF817C0802A6ULL;}
                else if(caller==0x20E784&&saved[5]==0x20EF48){next_pc=0x20EECC;seventh_offset=0x3A0;next_prologue=0xF821FF817C0802A6ULL;}
                if(!strcmp(upper[2],"ok")&&next_pc)
                    upper[3]=audio_stop_known_frame(sp,seventh_offset,next_pc,next_prologue,&saved[6]);
                else upper[3]="unknown_static_caller";
            }else upper[2]="unknown_static_caller";
            /* This exact original branch uses saved r31 -> [0] -> [80], then
             * reads request flags98 and enabled byte1D3. These are observations
             * of the request structure, not an assertion that they are pad bits. */
            if(!strcmp(upper[1],"ok")&&saved[4]==0x20DD80){
                request_status=audio_stop_read_u64(sp+0x1E8,&request_r31);
                u64 pair=0;
                if(!strcmp(request_status,"ok"))request_status=audio_stop_read_u64(request_r31,&pair);
                request_root=pair>>32;pair=0;
                if(!strcmp(request_status,"ok"))request_status=audio_stop_read_u64(request_root+0x80,&pair);
                request_obj=pair>>32;pair=0;
                if(!strcmp(request_status,"ok"))request_status=audio_stop_read_u64(request_obj+0x98,&pair);
                request_flags=pair>>32;pair=0;
                if(!strcmp(request_status,"ok"))request_status=audio_stop_read_u64(request_obj+0x1D0,&pair);
                request_byte=(pair>>32)&255;
            }
        }else upper[1]="parent_not_expected";
    }
    fprintf(stderr,"[sail-stop-hle] row=%u self=%016llX guest_lr=%08X guest_sp=%016llX saved90=%016llX status90=%s saved100=%016llX status100=%s saved170=%016llX status170=%s prologue=[%s,%s,%s] scope=known_static_chain_max7 saved200=%016llX status200=%s saved290=%016llX status290=%s saved320=%016llX status320=%s seventh_offset=%03X saved7=%016llX status7=%s request_r31=%016llX request_root=%08llX request_obj=%08llX request_flags98=%08llX request_byte1D3=%02llX request_status=%s\n",
        row+1,(unsigned long long)self,lr,(unsigned long long)sp,
        (unsigned long long)saved[0],status[0],(unsigned long long)saved[1],status[1],
        (unsigned long long)saved[2],status[2],fingerprint[0],fingerprint[1],fingerprint[2],
        (unsigned long long)saved[3],upper[0],(unsigned long long)saved[4],upper[1],
        (unsigned long long)saved[5],upper[2],seventh_offset,(unsigned long long)saved[6],upper[3],
        (unsigned long long)request_r31,(unsigned long long)request_root,(unsigned long long)request_obj,
        (unsigned long long)request_flags,(unsigned long long)request_byte,request_status);
}
static void audio_add_trace_written(u32 port,const AudioPortSlot* p,u32 slot,u64 ridx,
    const void* src,u32 src_bytes,const void* dst,u32 dst_bytes,float gain)
{
    if(!audio_add_trace_enabled()||s_add_trace_write_rows>=128)return;
    u32 seq=++s_add_trace_write_rows,gb;memcpy(&gb,&gain,4);
    u32 sh=audio_add_trace_hash(src,src_bytes),dh=audio_add_trace_hash(dst,dst_bytes);
    u64 ring=p->port_addr+(u64)slot*dst_bytes;
    if(slot<32){s_add_trace_ring[port][slot].seq=seq;s_add_trace_ring[port][slot].hash=dh;
        s_add_trace_ring[port][slot].ring_ea=ring;s_add_trace_ring[port][slot].ridx=ridx;}
    fprintf(stderr,"[audio-add-write] seq=%u port=%u slot=%u ridx=%llu running=%d ring_EA=%08llX source_bytes=%u source_hash=%08X ring_bytes=%u ring_hash=%08X gain_bits=%08X snapshot=under_audio_mutex\n",
        seq,port,slot,(unsigned long long)ridx,p->running,(unsigned long long)ring,src_bytes,sh,dst_bytes,dh,gb);
}
static void audio_add_trace_preclear(u32 port,const AudioPortSlot* p,const void* src,u32 slot)
{
    if(!audio_add_trace_enabled()||slot>=32||!s_add_trace_ring[port][slot].seq)return;
    u32 seq=s_add_trace_ring[port][slot].seq,prior=s_add_trace_ring[port][slot].hash;
    s_add_trace_ring[port][slot].seq=0;
    if(s_add_trace_mix_rows>=128)return;s_add_trace_mix_rows++;
    u32 bytes=CELL_AUDIO_BLOCK_SAMPLES*(u32)p->param.nChannel*4;
    u32 hash=audio_add_trace_hash(src,bytes);
    fprintf(stderr,"[audio-add-preclear] seq=%u port=%u slot=%u mix_ridx=%llu add_ridx=%llu ring_EA=%08llX ring_bytes=%u written_hash=%08X preclear_hash=%08X hash_match=%d snapshot=under_audio_mutex_not_final_output\n",
        seq,port,slot,(unsigned long long)p->read_index,(unsigned long long)s_add_trace_ring[port][slot].ridx,
        (unsigned long long)s_add_trace_ring[port][slot].ring_ea,bytes,prior,hash,prior==hash);
}

static void audio_add_trace_reject(u32 port,const char* reason,u64 ea,u64 bytes)
{
    static u32 rows;
    if(!audio_add_trace_enabled()||__atomic_load_n(&rows,__ATOMIC_RELAXED)>=64)return;
    u32 row=__atomic_fetch_add(&rows,1,__ATOMIC_RELAXED);if(row>=64)return;
    fprintf(stderr,"[audio-add-reject] row=%u port=%u reason=%s EA=%016llX bytes=%llu\n",
        row+1,port,reason,(unsigned long long)ea,(unsigned long long)bytes);
}
/* RPCS3 cellAudio.cpp Add* contract: accumulate, never replace; 256 frames
 * into the guest-visible read-index block. Keep the same mixer mutex across
 * index selection and all writes so the mixer cannot clear a partial block. */
/* Blocks the mixer has consumed so far. A producer that adds one block per
 * consumed block (SAIL movie audio) paces on this instead of a wall clock:
 * cellAudioAdd*Data sums into the current block, so two adds in one block
 * period layer two different frames and the next block plays silence. */
static u64 s_audio_blocks_mixed;
u64 cellAudio_blocks_mixed(void) { return __atomic_load_n(&s_audio_blocks_mixed, __ATOMIC_ACQUIRE); }
static s32 audio_add_pcm(u32 portNum,const float* src,u32 samples,float volume,u32 source_channels)
{
    if(!s_audio_initialized)return CELL_AUDIO_ERROR_NOT_INIT;
    u64 ea=(u64)(uintptr_t)src;
    if(portNum>=CELL_AUDIO_PORT_MAX||!ea||(ea&3)||samples!=CELL_AUDIO_BLOCK_SAMPLES)return CELL_AUDIO_ERROR_PARAM;
    mutex_lock(&s_audio_mutex);
    AudioPortSlot* p=&s_ports[portNum];
    if(!p->in_use){mutex_unlock(&s_audio_mutex);return CELL_AUDIO_ERROR_PORT_NOT_OPEN;}
    u32 nch=(u32)p->param.nChannel,blocks=(u32)p->param.nBlock;
    u32 sc=source_channels?source_channels:nch;
    if((nch!=2&&nch!=8)||!blocks||!p->buffer||(source_channels==6&&nch!=8)){
        audio_add_trace_reject(portNum,"port_format",ea,(u64)samples*sc*4);
        mutex_unlock(&s_audio_mutex);return CELL_AUDIO_ERROR_PARAM;
    }
    if(!audio_pcm_guest_range(ea,(u64)samples*sc*4)){
        audio_add_trace_reject(portNum,"source_range",ea,(u64)samples*sc*4);
        mutex_unlock(&s_audio_mutex);return CELL_AUDIO_ERROR_PARAM;
    }
    if(!audio_pcm_guest_range(p->read_idx_addr,8)){
        audio_add_trace_reject(portNum,"read_index_range",p->read_idx_addr,8);
        mutex_unlock(&s_audio_mutex);return CELL_AUDIO_ERROR_PARAM;
    }
    u64 read_index=vm_read64(p->read_idx_addr);
    u32 block=(u32)(read_index%blocks);
    u64 offset=(u64)block*samples*nch*4;
    if(offset+(u64)samples*nch*4>p->buf_size||!audio_pcm_guest_range(p->port_addr+offset,(u64)samples*nch*4)){
        audio_add_trace_reject(portNum,"ring_range_or_offset",p->port_addr+offset,(u64)samples*nch*4);
        mutex_unlock(&s_audio_mutex);return CELL_AUDIO_ERROR_PARAM;
    }
    float* dst=p->buffer+(size_t)block*samples*nch;
    const float* input=(const float*)(vm_base+(u32)ea);
    if(!isfinite(volume))volume=0;
    if(volume>16)volume=16;if(volume< -16)volume=-16;
    static const u32 map6[6]={0,2,1,4,5,3};
    for(u32 i=0;i<samples;i++)for(u32 c=0;c<sc;c++){
        u32 dc=source_channels==6?map6[c]:c;
        float* out=dst+i*nch+dc;
        audio_store_be_f32(out,ld_be_f32(out)+ld_be_f32(input+i*sc+c)*volume);
    }
    audio_add_trace_written(portNum,p,block,read_index,input,samples*sc*4,dst,samples*nch*4,volume);
    mutex_unlock(&s_audio_mutex);return CELL_OK;
}
s32 cellAudioAddData(u32 portNum,const float* src,u32 samples,float volume)
{
    return audio_add_pcm(portNum,src,samples,volume,0);
}
s32 cellAudioAdd2chData(u32 portNum,const float* src,u32 samples,float volume)
{
    return audio_add_pcm(portNum,src,samples,volume,2);
}
s32 cellAudioAdd6chData(u32 portNum,const float* src,float volume)
{
    return audio_add_pcm(portNum,src,CELL_AUDIO_BLOCK_SAMPLES,volume,6);
}

/* Bounded cadence diagnostic, mixer-thread owned. Gate-file existence is
 * checked at most four times/second; getenv is read once, never per block.
 * Pre-mix snapshots are not atomic against guest PCM writes. */
typedef struct {
    u32 port,slot,channels,blocks,nonzero[8],nonfinite;u64 read_index;
} audio_ring_timing_row;
static struct {
    int initialized,on,armed,pending;u32 limit,written,count;
    u64 sequence,begin_us,last_begin_us,last_notify_us,next_gate_us;
    char gate[512];audio_ring_timing_row rows[CELL_AUDIO_PORT_MAX];
} s_ring_timing;
static u64 audio_ring_timing_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (u64)((double)now.QuadPart*1000000.0/(double)frequency.QuadPart);
#else
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (u64)now.tv_sec*1000000u+(u64)now.tv_nsec/1000u;
#endif
}
static void audio_ring_timing_begin(void)
{
    if(!s_ring_timing.initialized) {
        s_ring_timing.initialized=1;
        const char* e=getenv("AUDIO_RING_TIMING");s_ring_timing.on=e && atoi(e)!=0;
        s_ring_timing.limit=512;
        e=getenv("AUDIO_RING_TIMING_LIMIT");
        if(e){char* end=NULL;unsigned long n=strtoul(e,&end,10);
            if(end!=e && !*end && n>=1 && n<=512)s_ring_timing.limit=(u32)n;}
        e=getenv("AUDIO_RING_TIMING_GATE");
        if(e) {strncpy(s_ring_timing.gate,e,sizeof s_ring_timing.gate-1);}
        s_ring_timing.armed=!s_ring_timing.gate[0];
    }
    s_ring_timing.pending=0;s_ring_timing.count=0;
    if(!s_ring_timing.on || s_ring_timing.written>=s_ring_timing.limit)return;
    const u64 now=audio_ring_timing_now();
    if(!s_ring_timing.armed) {
        if(now<s_ring_timing.next_gate_us)return;
        s_ring_timing.next_gate_us=now+250000u;
        FILE* gate=fopen(s_ring_timing.gate,"rb");
        if(!gate)return;fclose(gate);s_ring_timing.armed=1;
    }
    s_ring_timing.pending=1;s_ring_timing.begin_us=now;s_ring_timing.sequence++;
}
static void audio_ring_timing_port(u32 p,const AudioPortSlot* port,const float* src,u32 slot)
{
    if(!s_ring_timing.pending || s_ring_timing.count>=CELL_AUDIO_PORT_MAX ||
       s_ring_timing.written+s_ring_timing.count>=s_ring_timing.limit)return;
    audio_ring_timing_row* row=&s_ring_timing.rows[s_ring_timing.count++];
    memset(row,0,sizeof *row);row->port=p;row->slot=slot;row->read_index=port->read_index;
    row->channels=(u32)port->param.nChannel;row->blocks=(u32)port->param.nBlock;
    for(u32 sample=0;sample<CELL_AUDIO_BLOCK_SAMPLES;sample++)for(u32 ch=0;ch<row->channels && ch<8;ch++) {
        const float f=ld_be_f32(src+sample*row->channels+ch);
        if(f!=0.0f)row->nonzero[ch]++;
        if(!isfinite(f))row->nonfinite++;
    }
}
static void audio_ring_timing_finish(u64 notify_begin,u64 notify_end,u32 attempts,u32 accepted,
                                     u32 missing,int first_rc)
{
    if(!s_ring_timing.pending)return;
    for(u32 i=0;i<s_ring_timing.count;i++) {
        const audio_ring_timing_row* r=&s_ring_timing.rows[i];
        fprintf(stderr,"[audio-ring-timing] seq=%llu port=%u monotonic_us=%llu mix_delta_us=%llu previous_gap_valid=%d block_nominal_us=5333.333 ridx=%llu slot=%u blocks=%u channels=%u nz=[%u,%u,%u,%u,%u,%u,%u,%u] nonfinite=%u notify_us=%llu notify_delta_us=%llu mix_to_notify_us=%llu notify_work_us=%llu queue_attempts=%u queue_accepted=%u queue_missing=%u first_push_rc=%d push_rc_valid=%d snapshot=pre_mix_nonatomic\n",
            (unsigned long long)s_ring_timing.sequence,r->port,(unsigned long long)s_ring_timing.begin_us,
            (unsigned long long)(s_ring_timing.last_begin_us?s_ring_timing.begin_us-s_ring_timing.last_begin_us:0),
            s_ring_timing.last_begin_us!=0,
            (unsigned long long)r->read_index,r->slot,r->blocks,r->channels,
            r->nonzero[0],r->nonzero[1],r->nonzero[2],r->nonzero[3],r->nonzero[4],r->nonzero[5],r->nonzero[6],r->nonzero[7],r->nonfinite,
            (unsigned long long)notify_begin,
            (unsigned long long)(s_ring_timing.last_notify_us?notify_begin-s_ring_timing.last_notify_us:0),
            (unsigned long long)(notify_begin-s_ring_timing.begin_us),(unsigned long long)(notify_end-notify_begin),
            attempts,accepted,missing,first_rc,attempts!=0);
        s_ring_timing.written++;
    }
    if(s_ring_timing.count){s_ring_timing.last_begin_us=s_ring_timing.begin_us;s_ring_timing.last_notify_us=notify_begin;}
    s_ring_timing.pending=0;
}

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    audio_ring_timing_begin();
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;
        audio_ring_timing_port((u32)p,port,src,block_idx);
        audio_add_trace_preclear((u32)p,port,src,block_idx);

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&src[s * nch + 0]) * level;
                right = ld_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&src[s]) * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
    }

    mutex_unlock(&s_audio_mutex);
    __atomic_add_fetch(&s_audio_blocks_mixed, 1, __ATOMIC_RELEASE);

    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

/* Pure integer deadline admission for optional block-clock pacing. Small
 * lateness keeps the absolute schedule (no accumulating Sleep(1) drift).
 * A delay over half a block reanchors the next deadline instead of catching
 * up with back-to-back consumption. A >50ms backlog keeps the old reset rule. */
typedef struct { long long origin,frequency;unsigned long long blocks;u32 last_reset; } AudioBlockPacer;
static long long audio_block_pacer_ticks(const AudioBlockPacer* p)
{
    return (long long)(p->blocks*CELL_AUDIO_BLOCK_SAMPLES*(unsigned long long)p->frequency/CELL_AUDIO_SAMPLE_RATE);
}
static int audio_block_pacer_admit(AudioBlockPacer* p,long long now)
{
    const long long elapsed=audio_block_pacer_ticks(p);
    const long long due=p->origin+elapsed;p->last_reset=0;
    if(now<due)return 0;
    const long long late=now-due;
    if(late>p->frequency/20) {p->origin=now-elapsed;p->last_reset=2;}
    else if(late>p->frequency*CELL_AUDIO_BLOCK_SAMPLES/(CELL_AUDIO_SAMPLE_RATE*2)) {
        p->origin=now-elapsed;p->last_reset=1;
    }
    p->blocks++;return 1;
}

static void audio_notify_event_queues(void)
{
    const int timing=s_ring_timing.pending;
    const u64 notify_begin=timing?audio_ring_timing_now():0;
    u32 attempts=0,accepted=0,missing=0;int first_rc=0;
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid) {
            const int rc=sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
            if(timing){if(!attempts)first_rc=rc;attempts++;if(rc==0)accepted++;}
        } else if(timing)missing++;
    }
    if(timing)audio_ring_timing_finish(notify_begin,audio_ring_timing_now(),attempts,accepted,missing,first_rc);
}

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    const char* pacing_env=getenv("AUDIO_BLOCK_PACING");
    const int block_pacing=pacing_env && atoi(pacing_env)!=0;
    AudioBlockPacer pacer={q0.QuadPart,qf.QuadPart,0,0};
    if(block_pacing)fprintf(stderr,"[cellAudio] AUDIO_BLOCK_PACING=1 rate=48000/256 half_block_reanchor=1\n");
    while (s_mix_thread_running) {
        const int room = audio_backend_room();
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
        if (block_pacing) {
            LARGE_INTEGER now;QueryPerformanceCounter(&now);
            if(!audio_block_pacer_admit(&pacer,now.QuadPart)){Sleep(1);continue;}
        } else if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
        blocks++;
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        /* AUDIO_WAV=<file>: append the final mix as raw f32le stereo 48 kHz
         * (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). */
        { static FILE* _wf = (FILE*)-1;
          if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
          if (_wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); }
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, device buf %u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, 10);
#else
                        0u, 10);
#endif
                n = z = 0; d0 = g_audio_dropped; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    if(audio_add_trace_enabled())memset(s_add_trace_ring,0,sizeof(s_add_trace_ring));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        fprintf(stderr, "[cellAudio] Init failed: no working host audio backend\n");
        audio_backend_shutdown();
        mutex_destroy(&s_audio_mutex);
        return (s32)CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    if (audio_start_mix_thread() < 0) {
        fprintf(stderr, "[cellAudio] Init failed: could not start mixing thread\n");
        audio_backend_shutdown();
        mutex_destroy(&s_audio_mutex);
        return (s32)CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    u64 attr = vm_read64(param_ea + 16);
    float initial_level = 1.0f;
    /* CELL_AUDIO_PORTATTR_INITLEVEL: the level field is optional. A missing
     * flag means unity, while an explicitly supplied zero remains muted.
     * RPCS3 cellAudio.cpp port initialization follows this same contract. */
    if (attr & 0x1000ULL) {
        u32 bits = vm_read32(param_ea + 24);
        memcpy(&initial_level, &bits, sizeof(initial_level));
        if (!isfinite(initial_level) || initial_level < 0.0f || initial_level > 1.0f)
            return CELL_AUDIO_ERROR_PARAM;
    }
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->param.attr     = attr;
    port->param.level    = initial_level;
    fprintf(stderr, "[cellAudio] PortOpen port=%d attr=0x%llX initial_level=%.6f explicit=%d\n",
            found, (unsigned long long)attr, initial_level, (attr & 0x1000ULL) != 0);
    port->read_index  = 0;
    port->write_index = 0;

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Each port reuses its own two pages in the runtime-owned audio window. */
    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    u32 guest_buf = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, VM_AUDIO_PORT_SIZE) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    /* Seed the committed-page cache from the actual allocation, including the
     * read-index page; source buffers use the same miss validation lazily. */
#ifdef _WIN32
    if(!audio_pcm_guest_range(guest_buf,VM_AUDIO_PORT_SIZE)) {
        port->in_use=0;mutex_unlock(&s_audio_mutex);return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }
#endif
    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;
    if(audio_add_trace_enabled())memset(s_add_trace_ring[portNum],0,sizeof(s_add_trace_ring[portNum]));

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 24))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
