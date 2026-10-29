/* Original Macross PAMF -> MPEG-PS -> MPEG-2 -> I420, through supplied FFmpeg.
 * This is a decoder only: no window, playback clock, callbacks, guest memory,
 * sound, or fake end-of-stream. SAIL integration owns those responsibilities.
 *
 * Public prefix layouts reviewed against the EXACT runtime's source tag:
 * https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.1/libavcodec/packet.h
 * https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.1/libavutil/frame.h
 * https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.1/libavutil/version.h
 * The supplied DLL reports av_version_info=9.0.1, avcodec/avformat=63.1.101,
 * avutil=61.1.101. Other builds are rejected before dereferencing a prefix.
 * Library objects are always allocated/freed by their library APIs. No guessed
 * AVFormatContext/AVStream/AVCodec/AVCodecContext layout is accessed.
 */
#include "pam_decoder.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr int kAgain = -11;
constexpr int kEof = -541478725;  // FFERRTAG('E','O','F',' ').
constexpr uint32_t kCodecVersion = (63u << 16) | (1u << 8) | 101u;
constexpr uint32_t kUtilVersion = (61u << 16) | (1u << 8) | 101u;

struct PacketPrefix {
    void* buf;
    int64_t pts;
    int64_t dts;
    uint8_t* data;
    int size;
    int stream_index;
};
struct Rational { int num; int den; };
struct FramePrefix {
    uint8_t* data[8];
    int linesize[8];
    uint8_t** extended_data;
    int width;
    int height;
    int nb_samples;
    int format;
    int pict_type;
    Rational sample_aspect_ratio;
    int64_t pts;
};
static_assert(sizeof(void*) == 8, "Reviewed FFmpeg prefix ABI requires 64-bit host");
static_assert(offsetof(PacketPrefix, stream_index) == 36, "AVPacket prefix alignment");
static_assert(offsetof(FramePrefix, width) == 104, "AVFrame prefix alignment");
static_assert(offsetof(FramePrefix, pts) == 136, "AVFrame prefix alignment");

struct Api {
    unsigned (*avutil_version)() = nullptr;
    unsigned (*avcodec_version)() = nullptr;
    unsigned (*avformat_version)() = nullptr;
    const char* (*av_version_info)() = nullptr;
    int (*av_strerror)(int, char*, size_t) = nullptr;
    int (*av_dict_set)(void**, const char*, const char*, int) = nullptr;
    void (*av_dict_free)(void**) = nullptr;
    int (*av_opt_set_int)(void*, const char*, int64_t, int) = nullptr;
    const void* (*av_find_input_format)(const char*) = nullptr;
    int (*avformat_open_input)(void**, const char*, const void*, void**) = nullptr;
    int (*avformat_find_stream_info)(void*, void*) = nullptr;
    int (*av_find_best_stream)(void*, int, int, int, const void**, int) = nullptr;
    void (*avformat_close_input)(void**) = nullptr;
    int (*av_read_frame)(void*, void*) = nullptr;
    const void* (*avcodec_find_decoder)(int) = nullptr;
    void* (*avcodec_alloc_context3)(const void*) = nullptr;
    int (*avcodec_open2)(void*, const void*, void**) = nullptr;
    void (*avcodec_free_context)(void**) = nullptr;
    int (*avcodec_send_packet)(void*, const void*) = nullptr;
    int (*avcodec_receive_frame)(void*, void*) = nullptr;
    void* (*av_packet_alloc)() = nullptr;
    void (*av_packet_free)(void**) = nullptr;
    void (*av_packet_unref)(void*) = nullptr;
    void* (*av_frame_alloc)() = nullptr;
    void (*av_frame_free)(void**) = nullptr;
    void (*av_frame_unref)(void*) = nullptr;
    const char* (*av_get_pix_fmt_name)(int) = nullptr;
    int (*av_image_get_buffer_size)(int, int, int, int) = nullptr;
    int (*av_image_copy_to_buffer)(uint8_t*, int, const uint8_t* const*, const int*, int, int, int, int) = nullptr;
};

void copy_error(char* output, size_t capacity, const char* text)
{
    if (output && capacity) std::snprintf(output, capacity, "%s", text ? text : "Unknown decoder error");
}

#ifdef _WIN32
std::wstring wide_path(const char* input)
{
    if (!input || !*input) throw std::runtime_error("Missing FFmpeg DLL directory");
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, nullptr, 0);
    if (!count) throw std::runtime_error("FFmpeg DLL directory is not valid UTF-8");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, &result[0], count);
    result.resize(static_cast<size_t>(count - 1));
    DWORD length = GetFullPathNameW(result.c_str(), 0, nullptr, nullptr);
    if (!length) throw std::runtime_error("Cannot resolve FFmpeg DLL directory");
    std::wstring absolute(length, L'\0');
    DWORD copied = GetFullPathNameW(result.c_str(), length, &absolute[0], nullptr);
    if (!copied || copied >= length) throw std::runtime_error("Cannot resolve FFmpeg DLL directory");
    absolute.resize(copied);
    return absolute;
}
template<class T> void resolve(HMODULE module, const char* name, T& output)
{
    output = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!output) throw std::runtime_error(std::string("Missing FFmpeg symbol: ") + name);
}
#endif
} // namespace

struct MacrossPamDecoder {
    Api api;
#ifdef _WIN32
    HMODULE util = nullptr, codec = nullptr, format = nullptr;
#endif
    void* demux = nullptr;
    void* decode = nullptr;
    void* packet = nullptr;
    void* frame = nullptr;
    int video_stream = -1;
    bool flushed = false;
    bool eof = false;
    bool failed = false;
    uint32_t frame_index = 0;
    std::vector<uint8_t> pixels;
    std::string error;

    ~MacrossPamDecoder()
    {
        if (frame && api.av_frame_free) api.av_frame_free(&frame);
        if (packet && api.av_packet_free) api.av_packet_free(&packet);
        if (decode && api.avcodec_free_context) api.avcodec_free_context(&decode);
        if (demux && api.avformat_close_input) api.avformat_close_input(&demux);
#ifdef _WIN32
        if (format) FreeLibrary(format);
        if (codec) FreeLibrary(codec);
        if (util) FreeLibrary(util);
#endif
    }

    void check(int result, const char* action)
    {
        if (result >= 0) return;
        char text[256] = {};
        if (api.av_strerror) api.av_strerror(result, text, sizeof(text));
        throw std::runtime_error(std::string(action) + ": " + text + " (" + std::to_string(result) + ")");
    }

    void load(const char* directory)
    {
#ifdef _WIN32
        std::wstring base = wide_path(directory);
        if (base.back() != L'\\' && base.back() != L'/') base += L'\\';
        auto library = [&base](const wchar_t* name) {
            HMODULE module = LoadLibraryExW((base + name).c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) throw std::runtime_error("Cannot load supplied FFmpeg DLL; Windows error " + std::to_string(GetLastError()));
            return module;
        };
        util = library(L"avutil-61.dll");
        codec = library(L"avcodec-63.dll");
        format = library(L"avformat-63.dll");
#define RESOLVE(module, name) resolve(module, #name, api.name)
        RESOLVE(util, avutil_version); RESOLVE(codec, avcodec_version);
        RESOLVE(format, avformat_version); RESOLVE(util, av_version_info);
        if (api.avutil_version() != kUtilVersion || api.avcodec_version() != kCodecVersion ||
            api.avformat_version() != kCodecVersion || std::strcmp(api.av_version_info(), "9.0.1"))
            throw std::runtime_error("Unsupported FFmpeg version: reviewed DLLs must report exactly 9.0.1 / 63.1.101 / 61.1.101");
        RESOLVE(util, av_strerror); RESOLVE(util, av_dict_set); RESOLVE(util, av_dict_free);
        RESOLVE(util, av_opt_set_int); RESOLVE(format, av_find_input_format);
        RESOLVE(format, avformat_open_input); RESOLVE(format, avformat_find_stream_info);
        RESOLVE(format, av_find_best_stream); RESOLVE(format, avformat_close_input);
        RESOLVE(format, av_read_frame); RESOLVE(codec, avcodec_find_decoder);
        RESOLVE(codec, avcodec_alloc_context3); RESOLVE(codec, avcodec_open2);
        RESOLVE(codec, avcodec_free_context); RESOLVE(codec, avcodec_send_packet);
        RESOLVE(codec, avcodec_receive_frame); RESOLVE(codec, av_packet_alloc);
        RESOLVE(codec, av_packet_free); RESOLVE(codec, av_packet_unref);
        RESOLVE(util, av_frame_alloc); RESOLVE(util, av_frame_free); RESOLVE(util, av_frame_unref);
        RESOLVE(util, av_get_pix_fmt_name); RESOLVE(util, av_image_get_buffer_size);
        RESOLVE(util, av_image_copy_to_buffer);
#undef RESOLVE
#else
        (void)directory;
        throw std::runtime_error("This supplied-DLL decoder prototype requires Windows");
#endif
    }

    void open(const char* path)
    {
        if (!path || !*path) throw std::runtime_error("Missing PAMF media path");
        const void* input = api.av_find_input_format("mpeg");
        if (!input) throw std::runtime_error("FFmpeg has no MPEG-PS demuxer");
        void* options = nullptr;
        check(api.av_dict_set(&options, "probesize", "8388608", 0), "Set probe limit");
        check(api.av_dict_set(&options, "analyzeduration", "3000000", 0), "Set analysis limit");
        int result = api.avformat_open_input(&demux, path, input, &options);
        api.av_dict_free(&options);
        check(result, "Open PAMF/MPEG-PS");
        check(api.avformat_find_stream_info(demux, nullptr), "Inspect PAMF streams");
        const void* selected_decoder = nullptr;
        video_stream = api.av_find_best_stream(demux, 0 /* AVMEDIA_TYPE_VIDEO */, -1, -1, &selected_decoder, 0);
        check(video_stream, "Find video stream");
        if (!selected_decoder || selected_decoder != api.avcodec_find_decoder(2 /* AV_CODEC_ID_MPEG2VIDEO */))
            throw std::runtime_error("This decoder prototype currently accepts MPEG-2 video only");
        decode = api.avcodec_alloc_context3(selected_decoder);
        if (!decode) throw std::bad_alloc();
        check(api.av_opt_set_int(decode, "threads", 1, 0), "Set decoder thread count");
        check(api.avcodec_open2(decode, selected_decoder, nullptr), "Open MPEG-2 decoder");
        packet = api.av_packet_alloc();
        frame = api.av_frame_alloc();
        if (!packet || !frame) throw std::bad_alloc();
    }

    int next(MacrossPamFrame& output)
    {
        if (eof) return 0;
        // A corrupt/non-video-heavy input cannot spin forever in one call.
        // The normal path consumes one MPEG video packet per decoded frame.
        for (unsigned reads = 0; reads < 4096; ++reads) {
            int result = api.avcodec_receive_frame(decode, frame);
            if (result == kEof) { eof = true; return 0; }
            if (result >= 0) {
                const FramePrefix* f = static_cast<const FramePrefix*>(frame);
                const char* name = api.av_get_pix_fmt_name(f->format);
                if (!name || std::strcmp(name, "yuv420p")) {
                    api.av_frame_unref(frame);
                    throw std::runtime_error("Decoded video is not supported I420/yuv420p");
                }
                if (f->width < 1 || f->height < 1 || f->width > 8192 || f->height > 8192) {
                    api.av_frame_unref(frame);
                    throw std::runtime_error("Decoded dimensions outside bounds");
                }
                int size = api.av_image_get_buffer_size(f->format, f->width, f->height, 1);
                if (size <= 0 || size > 128 * 1024 * 1024) {
                    api.av_frame_unref(frame);
                    throw std::runtime_error("Decoded frame size outside bounds");
                }
                pixels.resize(static_cast<size_t>(size));
                const uint8_t* planes[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
                int copied = api.av_image_copy_to_buffer(pixels.data(), size, planes, f->linesize,
                                                        f->format, f->width, f->height, 1);
                output.width = static_cast<uint32_t>(f->width);
                output.height = static_cast<uint32_t>(f->height);
                output.pts = f->pts;
                output.pts_valid = f->pts != std::numeric_limits<int64_t>::min();
                api.av_frame_unref(frame);
                check(copied, "Copy decoded frame");
                if (copied != size) throw std::runtime_error("Incomplete decoded frame copy");
                output.i420 = pixels.data();
                output.size = pixels.size();
                output.frame_index = frame_index++;
                return 1;
            }
            if (result != kAgain) check(result, "Receive decoded frame");
            if (flushed) throw std::runtime_error("Decoder requested more input after drain");
            result = api.av_read_frame(demux, packet);
            if (result == kEof) {
                check(api.avcodec_send_packet(decode, nullptr), "Drain MPEG-2 decoder");
                flushed = true;
                continue;
            }
            check(result, "Read MPEG packet");
            const PacketPrefix* p = static_cast<const PacketPrefix*>(packet);
            if (p->size < 0 || p->size > 16 * 1024 * 1024) {
                api.av_packet_unref(packet);
                throw std::runtime_error("Compressed packet size outside bounds");
            }
            if (p->stream_index == video_stream)
                result = api.avcodec_send_packet(decode, packet);
            else result = 0;
            api.av_packet_unref(packet);
            check(result, "Submit MPEG packet");
        }
        throw std::runtime_error("No video frame within packet-read bound");
    }
};

extern "C" MacrossPamDecoder* macross_pam_open(const char* media_path, const char* dll_directory,
                                               char* error, size_t error_capacity)
{
    copy_error(error, error_capacity, "");
    try {
        std::unique_ptr<MacrossPamDecoder> decoder(new MacrossPamDecoder);
        decoder->load(dll_directory);
        decoder->open(media_path);
        return decoder.release();
    } catch (const std::exception& e) {
        copy_error(error, error_capacity, e.what());
    } catch (...) {
        copy_error(error, error_capacity, "Unknown PAMF decoder open failure");
    }
    return nullptr;
}

extern "C" int macross_pam_next_frame(MacrossPamDecoder* decoder, MacrossPamFrame* frame)
{
    if (!decoder || !frame) return -1;
    std::memset(frame, 0, sizeof(*frame));
    if (decoder->failed) return -1;
    try {
        return decoder->next(*frame);
    } catch (const std::exception& e) {
        decoder->error = e.what();
    } catch (...) {
        decoder->error = "Unknown PAMF decoder frame failure";
    }
    decoder->failed = true;
    std::memset(frame, 0, sizeof(*frame));
    return -1;
}

extern "C" const char* macross_pam_last_error(const MacrossPamDecoder* decoder)
{
    return decoder ? decoder->error.c_str() : "Null PAMF decoder";
}

extern "C" void macross_pam_close(MacrossPamDecoder* decoder)
{
    delete decoder;
}
