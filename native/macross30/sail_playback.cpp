/* Pull-driven SAIL video adapter for original Macross MPEG-2 PAMF streams.
 * Decoded pixels enter the game's own allocated frame buffers and RSX path.
 * No host overlay, simulated completion, or movie skip is provided here.
 * Callback and format ABI:
 * https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/Cell/Modules/cellSail.h
 */
#include "sail_playback.h"
#include "pam_decoder.h"
#include "pam_audio_decoder.h"
#include "movie_subtitle_timeline.h"
#include "movie_subtitle_renderer.h"
#include "../../vendor/ps3recomp-master/libs/video/rsx_null_backend.h"
#include "../../vendor/ps3recomp-master/libs/codec/cellSail.h"
#include "../../vendor/ps3recomp-master/libs/codec/cellPamf.h"
#include "../../vendor/ps3recomp-master/libs/codec/vdec_ffmpeg.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <atomic>
extern "C" uint64_t cellAudio_blocks_mixed(void);
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <charconv>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#endif

extern "C" {
extern uint8_t* vm_base;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t);
}

namespace {
using Clock = std::chrono::steady_clock;
constexpr int32_t kFailure = static_cast<int32_t>(0x806107FFu);
constexpr unsigned kBuffers = 3;
std::recursive_mutex lock;
uint32_t read32(uint32_t ea) {
    const uint8_t* p = vm_base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void write16(uint32_t ea, uint16_t v) { vm_base[ea] = uint8_t(v >> 8); vm_base[ea + 1] = uint8_t(v); }
void write32(uint32_t ea, uint32_t v) {
    for (unsigned n = 0; n < 4; ++n) vm_base[ea + n] = uint8_t(v >> (24 - n * 8));
}
void write64(uint32_t ea, uint64_t v) { write32(ea, uint32_t(v >> 32)); write32(ea + 4, uint32_t(v)); }
uint64_t guest(uint32_t opd, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    if (!opd) throw std::runtime_error("Missing required guest SAIL callback");
    return ppu_guest_call(opd, a0, a1, a2, a3, 0, 0, 0, 0);
}
struct Allocator { uint32_t alloc = 0, free = 0, arg = 0; };
struct Adapter {
    uint32_t self = 0, arg = 0;
    std::array<uint32_t, 5> callbacks{};
    std::array<uint8_t, 32> preferred{};
    bool has_preferred = false;
};
struct Player {
    uint32_t self = 0, adapter = 0, sound_adapter = 0, descriptor = 0;
    Allocator allocator;
    std::string uri;
    MacrossPamDecoder* decoder = nullptr;
    MacrossPamAudioDecoder* audio_decoder = nullptr;
    uint32_t audio_buffer = 0, audio_cursor = 0, audio_tag = 0;
    int64_t audio_pts = 0, timeline_pts = 0;
    std::vector<float> audio_pcm;
    std::array<uint8_t, 24> audio_format{};
    bool audio_eof = false, video_eof = false, audio_makeup = false;
    struct AudioStamp { uint32_t tag = 0; int64_t pts = 0; };
    std::array<AudioStamp, 64> audio_history{};
    uint32_t audio_anchor_tag = 0;
    int64_t audio_anchor_pts = 0;
    uint64_t audio_anchor_time = 0;
    uint32_t scratch = 0;
    std::array<uint32_t, kBuffers> buffers{};
    std::array<uint8_t, 32> format{};
    bool allocated = false, running = false, eos = false, pending = false, have_frame = false;
    // Held (without the global lock) while a video frame decodes, so audio
    // pulls are not stalled behind MPEG-2 decoding; release() takes it
    // before closing the decoder. Order: global lock -> video_mutex.
    std::mutex video_mutex;
    uint64_t audio_block_seen = UINT64_MAX;  // cellAudio block of the last served frame
    bool decoding = false;
    uint32_t session = 0, current_buffer = 0, current_tag = 0;
    uint32_t width = 0, height = 0;
    int coding = -1;
    int64_t first_pts = 0, last_decoded_pts = 0, pending_pts = 0, presented_pts = 0;
    std::vector<uint8_t> pending_pixels;
    // Immutable original of the currently presented frame; pending_pixels may
    // already contain the next decoded frame when F9 changes during a repeat.
    std::vector<uint8_t> presented_pixels;
    macross30::subtitles::Timeline subtitle_timeline;
    macross30::subtitles::Renderer subtitle_renderer;
    int64_t subtitle_origin = 0;
    uint32_t subtitle_epoch = UINT32_MAX;
    std::string_view subtitle_last_text;
    bool subtitle_frame_written = false;
    uint32_t frame_generation = 0; // Image content version, independent of media PTS/decode count.
    Clock::time_point started;
    ~Player() { if (decoder) macross_pam_close(decoder); if (audio_decoder) macross_pam_audio_close(audio_decoder); }
};
struct Database { uint32_t player=0, reader=0, header=0; Allocator allocator; };
std::unordered_map<uint32_t,Database> databases;
std::unordered_map<uint32_t, Allocator> allocators;
std::unordered_map<uint32_t, Adapter> adapters;
std::unordered_map<uint32_t, Adapter> sound_adapters;
std::unordered_map<uint32_t, std::shared_ptr<Player>> players;

std::string subtitle_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return s;
}
bool subtitle_basename(const std::string& s) {
    if (s.empty() || s.size() > 127 || s.find("..") != s.npos) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}
bool subtitle_sha_text(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
std::string subtitle_sha_stream(std::istream& input) {
#ifdef _WIN32
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> object;
        ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } h;
    if (BCryptOpenAlgorithmProvider(&h.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider failed");
    DWORD bytes = 0, object_bytes = 0;
    if (BCryptGetProperty(h.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &bytes, 0) < 0)
        throw std::runtime_error("SHA256 object query failed");
    h.object.resize(object_bytes);
    if (BCryptCreateHash(h.algorithm, &h.hash, h.object.data(), object_bytes, nullptr, 0, 0) < 0)
        throw std::runtime_error("SHA256 creation failed");
    std::array<unsigned char, 65536> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count && BCryptHashData(h.hash, buffer.data(), static_cast<ULONG>(count), 0) < 0)
            throw std::runtime_error("SHA256 update failed");
    }
    if (!input.eof()) throw std::runtime_error("SHA256 input read failed");
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(h.hash, digest.data(), digest.size(), 0) < 0)
        throw std::runtime_error("SHA256 finish failed");
    const char* hex = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned char c : digest) { result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    return result;
#else
    (void)input;
    throw std::runtime_error("Subtitle source verification requires Windows SHA256 provider");
#endif
}
struct SubtitleSourceHash {
    uintmax_t bytes;
    std::filesystem::file_time_type modified;
    std::string sha;
};
std::unordered_map<std::string, SubtitleSourceHash> subtitle_source_hashes;
std::string subtitle_source_sha(const std::string& path) {
    const auto file = std::filesystem::u8path(path);
    const auto bytes = std::filesystem::file_size(file);
    const auto modified = std::filesystem::last_write_time(file);
    auto i = subtitle_source_hashes.find(path);
    if (i != subtitle_source_hashes.end() && i->second.bytes == bytes && i->second.modified == modified) return i->second.sha;
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot hash original movie");
    const auto hash = subtitle_sha_stream(input);
    if (subtitle_source_hashes.size() >= 128) subtitle_source_hashes.clear();
    subtitle_source_hashes.insert_or_assign(path, SubtitleSourceHash{bytes, modified, hash});
    return hash;
}
void load_movie_subtitles(Player& p, const std::string& movie_path) noexcept {
    p.subtitle_last_text = {}; p.subtitle_timeline.Clear();
    p.subtitle_epoch = UINT32_MAX; p.subtitle_frame_written = false;
    const char* directory = std::getenv("MACROSS_SUBTITLE_DIR");
    if (!directory || !*directory) return; // No draft/content directory discovery.
    try {
        const auto root = std::filesystem::u8path(directory);
        const auto manifest = root / "tracks.tsv";
        if (std::filesystem::file_size(manifest) > 65536) throw std::runtime_error("Subtitle registry too large");
        std::ifstream input(manifest, std::ios::binary);
        std::string line;
        auto get_line = [&]() { if (!std::getline(input, line)) return false; if (!line.empty() && line.back() == '\r') line.pop_back(); return true; };
        if (!get_line() || line != "MACROSS_SUBTITLES_V1") throw std::runtime_error("Invalid subtitle registry header");
        const auto movie = subtitle_lower(std::filesystem::u8path(movie_path).filename().string());
        std::array<std::string, 7> selected{};
        std::unordered_map<std::string, bool> names;
        while (get_line()) {
            if (line.empty()) continue;
            std::array<std::string, 7> columns{}; size_t begin = 0;
            for (size_t n = 0; n < 7; ++n) {
                const auto end = line.find('\t', begin);
                if ((n < 6 && end == line.npos) || (n == 6 && end != line.npos)) throw std::runtime_error("Expected seven subtitle registry columns");
                columns[n] = line.substr(begin, end == line.npos ? line.npos : end - begin);
                begin = end == line.npos ? line.size() : end + 1;
            }
            columns[0] = subtitle_lower(columns[0]);
            if (!subtitle_basename(columns[0]) || !subtitle_basename(columns[1]) ||
                (columns[3] != "0" && columns[3] != "1") || !subtitle_sha_text(columns[4]) || !subtitle_sha_text(columns[5]) ||
                (columns[6] != "draft" && columns[6] != "verified") || !names.emplace(columns[0], true).second)
                throw std::runtime_error("Invalid/duplicate subtitle registry entry");
            if (columns[0] == movie) selected = columns;
        }
        if (input.bad()) throw std::runtime_error("Subtitle registry read failed");
        if (selected[0].empty() || selected[3] != "1") return;
        int64_t origin = -1;
        const auto parsed = std::from_chars(selected[2].data(), selected[2].data() + selected[2].size(), origin);
        if (parsed.ec != std::errc{} || parsed.ptr != selected[2].data() + selected[2].size() || origin < 0)
            throw std::runtime_error("Invalid subtitle PTS origin");
        if (subtitle_source_sha(movie_path) != selected[4]) throw std::runtime_error("Original movie SHA256 mismatch");
        const auto srt = root / selected[1];
        const auto size = std::filesystem::file_size(srt);
        if (!size || size > macross30::subtitles::Timeline::kMaxFileBytes) throw std::runtime_error("Invalid subtitle file size");
        std::ifstream cuefile(srt, std::ios::binary);
        std::string bytes(static_cast<size_t>(size), '\0');
        if (!cuefile.read(bytes.data(), bytes.size())) throw std::runtime_error("Cannot read subtitle file");
        std::istringstream hash_input(bytes);
        if (subtitle_sha_stream(hash_input) != selected[5]) throw std::runtime_error("Subtitle SRT SHA256 mismatch");
        std::string error;
        if (!p.subtitle_timeline.LoadSrt(movie, bytes, &error)) throw std::runtime_error(error);
        p.subtitle_origin = origin;
        std::fprintf(stderr, "[macross-subtitles] loaded movie=%s cues=%zu origin=%lld content_status=%s source_sha_verified=1 srt_sha_verified=1\n", movie.c_str(), p.subtitle_timeline.CueCount(), static_cast<long long>(origin), selected[6].c_str());
    } catch (const std::exception& e) {
        p.subtitle_last_text = {}; p.subtitle_timeline.Clear();
        std::fprintf(stderr, "[macross-subtitles] disabled for movie only: %s\n", e.what());
    }
}

std::shared_ptr<Player> player(uint32_t self) {
    auto i = players.find(self);
    if (i == players.end()) throw std::runtime_error("Unknown SAIL player");
    return i->second;
}
std::shared_ptr<Player> adapter_player(uint32_t ea) {
    for (auto& item : players) if (item.second->adapter == ea) return item.second;
    throw std::runtime_error("SAIL graphics adapter has no player");
}
Adapter& adapter(const Player& p) {
    auto i = adapters.find(p.adapter);
    if (i == adapters.end()) throw std::runtime_error("Unknown SAIL graphics adapter");
    return i->second;
}

void restore_original_subtitle_frame(Player& p) {
    if (!p.subtitle_frame_written || !p.allocated || !p.have_frame || p.presented_pixels.empty()) return;
    const auto buffer = p.buffers[p.current_buffer];
    if (p.coding == 3) std::memcpy(vm_base + buffer, p.presented_pixels.data(), p.presented_pixels.size());
    else vdec_store_picture(buffer, p.presented_pixels.data(), p.width, p.height, uint32_t(p.coding), 1, 255);
    p.subtitle_frame_written = false;
}

bool write_movie_subtitle_frame(Player& p, bool new_video_frame) {
    uint32_t epoch = 0;
    const bool enabled = rsx_null_backend_subtitles_enabled(&epoch) != 0;
    const auto text = p.subtitle_timeline.Update(enabled, p.session, p.uri,
        p.presented_pts, p.subtitle_origin, p.running && !p.eos);
    if (!new_video_frame && p.subtitle_epoch == epoch && text == p.subtitle_last_text) return false;
    const auto pixels = p.subtitle_renderer.GetFrame(p.presented_pixels,
        static_cast<int>(p.width), static_cast<int>(p.height), text, enabled);
    // Renderer failure/OFF returns the original span; audio/video keep running.
    const auto buffer = p.buffers[p.current_buffer];
    if (p.coding == 3) std::memcpy(vm_base + buffer, pixels.data(), pixels.size());
    else vdec_store_picture(buffer, pixels.data(), p.width, p.height, uint32_t(p.coding), 1, 255);
    p.subtitle_frame_written = pixels.data() != p.presented_pixels.data();
    p.subtitle_last_text = text; p.subtitle_epoch = epoch;
    ++p.frame_generation;
    return true;
}

void release(Player& p) {
    std::lock_guard<std::mutex> video(p.video_mutex);
    restore_original_subtitle_frame(p); // Before guest frame deallocation.
    p.running = false;
    auto sound = sound_adapters.find(p.sound_adapter);
    if (p.audio_makeup && sound != sound_adapters.end() && sound->second.callbacks[1])
        guest(sound->second.callbacks[1], sound->second.arg);
    p.audio_makeup = false;
    if (p.audio_buffer && p.allocator.free) guest(p.allocator.free, p.allocator.arg, 16, p.audio_buffer);
    p.audio_buffer = 0;
    p.audio_pcm.clear(); p.audio_cursor = 0;
    if (p.audio_decoder) macross_pam_audio_close(p.audio_decoder);
    p.audio_decoder = nullptr;
    auto i = adapters.find(p.adapter);
    if (p.allocated && i != adapters.end()) {
        // The same guest-owned pointer array used at allocation remains live.
        guest(i->second.callbacks[4], i->second.arg, kBuffers, p.scratch + 64);
    }
    p.allocated = false;
    p.buffers.fill(0);
    if (p.scratch && p.allocator.free) guest(p.allocator.free, p.allocator.arg, 16, p.scratch);
    p.scratch = 0;
    p.pending = p.have_frame = false;
    p.pending_pixels.clear();
    p.subtitle_last_text = {}; p.subtitle_timeline.Clear();
    p.subtitle_renderer.Clear(); p.presented_pixels.clear();
    p.subtitle_epoch = UINT32_MAX; p.subtitle_frame_written = false;
    if (p.decoder) macross_pam_close(p.decoder);
    p.decoder = nullptr;
}

std::string source_path(const std::string& uri) {
    const char* root = std::getenv("MACROSS_VFS_ROOT");
    if (!root || !*root) throw std::runtime_error("MACROSS_VFS_ROOT is required for SAIL playback");
    const std::string prefix = "x-cell-fs:///dev_bdvd/";
    if (uri.compare(0, prefix.size(), prefix)) throw std::runtime_error("Unsupported SAIL source URI");
    auto relative = std::filesystem::u8path(uri.substr(prefix.size())).lexically_normal();
    if (relative.is_absolute()) throw std::runtime_error("Invalid SAIL media path");
    for (const auto& part : relative) if (part == "..") throw std::runtime_error("SAIL media path escapes VFS");
    const auto encoded=(std::filesystem::u8path(root) / relative).u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()),encoded.size());
}

bool decode_pending(Player& p) {
    MacrossPamFrame frame{};
    int rc = macross_pam_next_frame(p.decoder, &frame);
    if (rc < 0) throw std::runtime_error(macross_pam_last_error(p.decoder));
    if (!rc) return false;
    if (p.width && (p.width != frame.width || p.height != frame.height))
        throw std::runtime_error("SAIL stream dimensions changed during playback");
    p.width = frame.width; p.height = frame.height;
    // MPEG-2 may omit PTS on B pictures. Preserve actual timestamps when present
    // and use the verified stream's 30000/1001 cadence only for missing PTS.
    p.pending_pts = frame.pts_valid ? frame.pts : p.last_decoded_pts + 3003;
    if (frame.frame_index == 0) p.first_pts = p.pending_pts;
    else if (p.pending_pts <= p.last_decoded_pts) throw std::runtime_error("Non-monotonic SAIL stream PTS");
    p.last_decoded_pts = p.pending_pts;
    p.pending_pixels.assign(frame.i420, frame.i420 + frame.size);
    p.pending = true;
    return true;
}

bool decode_audio(Player& p) {
    MacrossPamAudioFrame frame{};
    const int rc = macross_pam_audio_next_frame(p.audio_decoder, &frame);
    if (rc < 0) throw std::runtime_error(macross_pam_audio_last_error(p.audio_decoder));
    if (!rc) { p.audio_eof = true; return false; }
    if (frame.channels != 2 || frame.sample_rate != 48000 || !frame.pts_valid || frame.samples_per_channel != 2048)
        throw std::runtime_error("Unsupported SAIL audio frame configuration");
    p.audio_pcm.assign(frame.interleaved, frame.interleaved + frame.samples_per_channel * frame.channels);
    p.audio_cursor = 0; p.audio_pts = frame.pts90k;
    return true;
}

void finish_if_drained(Player& p) {
    if (!p.eos && p.video_eof && (!p.audio_decoder || p.audio_eof)) {
        restore_original_subtitle_frame(p);
        p.eos = true; p.running = false;
        p.subtitle_last_text = {}; p.subtitle_timeline.Stop();
        p.subtitle_renderer.Clear();
        cellSailPlaybackNotifyEos(p.self);
    }
}

void prepare(Player& p) {
    Adapter& a = adapter(p);
    if (!a.has_preferred) throw std::runtime_error("SAIL graphics preferred format is missing");
    p.coding = static_cast<int8_t>(a.preferred[0]);
    // Game func_001D3350 compares coding > 1 before its planar branch; the
    // comparison is CR3.GT (bit 14), not CR3.EQ. That branch accepts ONLY 3.
    // At 0x1D3694..0x1D36C4 it halves width/height/pitch, places the first
    // chroma plane at buffer + height*pitch, then advances pitch*height/4
    // for the second plane. Those are precisely contiguous I420 planes.
    // Coding 0/1 use one packed 32-bit texture, with respective channel
    // remaps 0xAAE4 / 0xAA93; use the SDK's ARGB / RGBA pixel conversion.
    if (p.coding != 0 && p.coding != 1 && p.coding != 3)
        throw std::runtime_error("Unsupported SAIL preferred coding " + std::to_string(p.coding));
    if (!p.allocator.alloc || !p.allocator.free || !p.allocator.arg)
        throw std::runtime_error("SAIL guest allocator callbacks/argument are missing");
    p.scratch = static_cast<uint32_t>(guest(p.allocator.alloc, p.allocator.arg, 16, 256));
    if (!p.scratch) throw std::runtime_error("SAIL guest scratch allocation failed");
    std::memset(vm_base + p.scratch, 0, 256);
    if (!decode_pending(p)) throw std::runtime_error("SAIL source contains no decoded video frames");
    std::fill(p.format.begin(), p.format.end(), 0);
    uint32_t f = p.scratch;
    vm_base[f] = uint8_t(p.coding); vm_base[f + 1] = 0; // progressive
    vm_base[f + 2] = 8; vm_base[f + 3] = 4; // 8-bit, 30000/1001
    write16(f + 4, uint16_t(p.width)); write16(f + 6, uint16_t(p.height));
    write32(f + 8, p.width * (p.coding == 3 ? 1 : 4));
    write32(f + 12, 255);
    vm_base[f + 16] = 1; vm_base[f + 17] = 1; vm_base[f + 18] = 0;
    std::memcpy(p.format.data(), vm_base + f, p.format.size());
    if (a.callbacks[0]) guest(a.callbacks[0], a.arg);
    guest(a.callbacks[2], a.arg, f, p.session);
    const uint32_t size = p.coding == 3 ? uint32_t(p.pending_pixels.size()) : p.width * p.height * 4;
    int32_t rc = static_cast<int32_t>(guest(a.callbacks[3], a.arg, size, kBuffers, p.scratch + 64));
    if (rc) throw std::runtime_error("SAIL guest frame allocation returned " + std::to_string(rc));
    p.allocated = true;
    for (unsigned n = 0; n < kBuffers; ++n) {
        p.buffers[n] = read32(p.scratch + 64 + n * 4);
        if (!p.buffers[n]) throw std::runtime_error("SAIL guest frame allocation returned a null buffer");
    }
    p.timeline_pts = p.first_pts;
    if (p.audio_decoder) {
        if (!decode_audio(p)) throw std::runtime_error("SAIL source contains no audio frames");
        p.timeline_pts = std::min(p.first_pts, p.audio_pts);
        p.audio_buffer = static_cast<uint32_t>(guest(p.allocator.alloc, p.allocator.arg, 16, 2048 * 2 * 4));
        if (!p.audio_buffer) throw std::runtime_error("SAIL guest audio allocation failed");
        auto& sa = sound_adapters.at(p.sound_adapter);
        if (sa.callbacks[0] && static_cast<int32_t>(guest(sa.callbacks[0], sa.arg)))
            throw std::runtime_error("SAIL sound makeup failed");
        p.audio_makeup = true;
        std::fill(p.audio_format.begin(), p.audio_format.end(), 0xFF);
        const uint32_t af = p.scratch + 128;
        std::memcpy(vm_base + af, p.audio_format.data(), 24);
        vm_base[af] = 1; vm_base[af + 1] = 2;
        write16(af + 2, 256); write32(af + 4, 48000);
        std::memcpy(p.audio_format.data(), vm_base + af, 24);
        if (sa.callbacks[2]) guest(sa.callbacks[2], sa.arg, af, p.session);
    }
    std::fprintf(stderr, "[macross-sail] prepared %ux%u coding=%d session=%u buffers=%08X/%08X/%08X\n",
        p.width, p.height, p.coding, p.session, p.buffers[0], p.buffers[1], p.buffers[2]);
}

template<class Fn> int32_t operation(uint32_t self, const char* name, Fn&& fn) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    try { fn(*player(self)); return 0; }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[macross-sail] %s player=%08X failed: %s\n", name, self, e.what());
        return kFailure;
    }
}
void allocator_init(uint32_t self, uint32_t callbacks, uint32_t arg) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    Allocator a;
    if (callbacks) { a.alloc = read32(callbacks); a.free = read32(callbacks + 4); }
    a.arg = arg; allocators[self] = a;
}
void initialize(uint32_t self, uint32_t allocator) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto p = std::make_shared<Player>(); p->self = self;
    auto i = allocators.find(allocator); if (i != allocators.end()) p->allocator = i->second;
    players[self] = p;
}
void graphics_init(uint32_t self, uint32_t callbacks, uint32_t arg) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    Adapter a; a.self = self; a.arg = arg;
    if (callbacks) for (unsigned n = 0; n < 5; ++n) a.callbacks[n] = read32(callbacks + n * 4);
    adapters[self] = a;
}
void sound_init(uint32_t self, uint32_t callbacks, uint32_t arg) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    Adapter a; a.self = self; a.arg = arg;
    if (callbacks) for (unsigned n = 0; n < 3; ++n) a.callbacks[n] = read32(callbacks + n * 4);
    sound_adapters[self] = a;
}
void sound_preferred(uint32_t self, uint32_t format) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto i = sound_adapters.find(self);
    if (i != sound_adapters.end() && format) {
        std::memcpy(i->second.preferred.data(), vm_base + format, 24);
        i->second.has_preferred = true;
    }
}
void preferred(uint32_t self, uint32_t format) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto i = adapters.find(self);
    if (i != adapters.end() && format) {
        std::memcpy(i->second.preferred.data(), vm_base + format, 32);
        i->second.has_preferred = true;
    }
}
int32_t attach(uint32_t self, uint32_t index, uint32_t a) {
    return operation(self, "attach", [&](Player& p) {
        if (index || !adapters.count(a)) throw std::runtime_error("Invalid graphics adapter attachment");
        p.adapter = a;
    });
}
int32_t attach_sound(uint32_t self, uint32_t index, uint32_t a) {
    return operation(self, "attach sound", [&](Player& p) {
        if (index || !sound_adapters.count(a)) throw std::runtime_error("Invalid sound adapter attachment");
        p.sound_adapter = a;
    });
}
int32_t descriptor(uint32_t self, uint32_t d, const char* uri) {
    return operation(self, "descriptor", [&](Player& p) { p.descriptor = d; p.uri = uri ? uri : ""; });
}
void destroy_database(uint32_t descriptor,uint32_t reader) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto i=databases.find(descriptor);if(i==databases.end())return;
    if(reader && reader!=i->second.reader)return;
    Database db=i->second;databases.erase(i);
    pamf_reader_forget(db.reader);
    if(db.header&&db.allocator.free)guest(db.allocator.free,db.allocator.arg,16,db.header);
}
int32_t create_database(uint32_t self,uint32_t descriptor,uint32_t reader,uint32_t size,const char* uri,uint64_t arg) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    uint32_t header_ea=0;Allocator allocator;
    try {
        auto p=player(self);allocator=p->allocator;
        if(arg||size<128||!reader||reader>UINT32_MAX-127||databases.count(descriptor)||
           !allocator.alloc||!allocator.free||!pamf_guest_buffer_writable(reader,128))throw std::runtime_error("Invalid PAMF database request");
        const std::string path=source_path(uri?uri:"");
        uint64_t file_size=std::filesystem::file_size(path);
        std::ifstream file(path,std::ios::binary);std::array<uint8_t,0x88> prefix{};
        if(!file.read(reinterpret_cast<char*>(prefix.data()),prefix.size()))throw std::runtime_error("Truncated PAMF prefix");
        auto be=[](const uint8_t* b){return uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];};
        uint64_t header_size=uint64_t(be(prefix.data()+8))*2048;
        if(be(prefix.data())!=0x50414D46||(be(prefix.data()+4)!=0x30303430&&be(prefix.data()+4)!=0x30303431)||
           header_size<0x88||header_size>16*1024*1024||header_size>file_size)
            throw std::runtime_error("Unsupported PAMF header");
        std::vector<uint8_t> header(static_cast<size_t>(header_size));
        file.seekg(0);if(!file.read(reinterpret_cast<char*>(header.data()),header.size()))throw std::runtime_error("Truncated PAMF header");
        uint64_t allocated=guest(allocator.alloc,allocator.arg,16,header_size);
        if(!allocated||allocated>UINT32_MAX-header_size)throw std::runtime_error("PAMF header allocation failed");
        header_ea=uint32_t(allocated);if(!pamf_guest_buffer_writable(header_ea,uint32_t(header_size)))throw std::runtime_error("PAMF header allocation is not writable guest memory");
        std::memcpy(vm_base+header_ea,header.data(),header.size());
        int32_t rc=cellPamfReaderInitialize(reinterpret_cast<CellPamfReader*>(uintptr_t(reader)),
            reinterpret_cast<void*>(uintptr_t(header_ea)),file_size,0);
        if(rc){guest(allocator.free,allocator.arg,16,header_ea);return rc;}
        databases.emplace(descriptor,Database{self,reader,header_ea,allocator});
        const uint64_t start=uint64_t(header[0x56])<<40|uint64_t(header[0x57])<<32|be(header.data()+0x58);
        const uint64_t end=uint64_t(header[0x5C])<<40|uint64_t(header[0x5D])<<32|be(header.data()+0x5E);
        std::fprintf(stderr,"[macross-sail-db] desc=%u reader=%08X header=%08X header_bytes=%llu file_bytes=%llu start_pts=%llu end_pts=%llu duration_ticks=%llu source=original_PAMF_header\n",
            descriptor,reader,header_ea,(unsigned long long)header_size,(unsigned long long)file_size,
            (unsigned long long)start,(unsigned long long)end,(unsigned long long)(end-start));
        return 0;
    }catch(const std::exception& e){
        pamf_reader_forget(reader);
        if(header_ea&&allocator.free)guest(allocator.free,allocator.arg,16,header_ea);
        std::fprintf(stderr,"[macross-sail] create database failed: %s\n",e.what());return kFailure;
    }
}
int32_t open(uint32_t self) {
    return operation(self, "open", [&](Player& p) {
        if (p.decoder) release(p);
        const char* libraries = std::getenv("MACROSS_FFMPEG_DIR");
        if (!libraries || !*libraries) throw std::runtime_error("MACROSS_FFMPEG_DIR is required");
        char error[1024] = {};
        const auto path = source_path(p.uri);
        p.decoder = macross_pam_open(path.c_str(), libraries, error, sizeof(error));
        if (!p.decoder) throw std::runtime_error(error);
        if (p.sound_adapter) {
            p.audio_decoder = macross_pam_audio_open(path.c_str(), error, sizeof(error));
            if (!p.audio_decoder) { release(p); throw std::runtime_error(error); }
        }
        p.audio_eof = p.video_eof = false; p.audio_tag = 0;
        p.audio_history.fill({}); p.audio_anchor_tag = 0;
        p.audio_block_seen = UINT64_MAX;
        p.eos = false; p.width = p.height = 0;
        load_movie_subtitles(p, path);
        std::fprintf(stderr, "[macross-sail] opened original movie %s\n", path.c_str());
    });
}
int32_t start(uint32_t self) {
    return operation(self, "start", [&](Player& p) {
        if (!p.decoder || p.running || p.allocated) throw std::runtime_error("SAIL player is not ready for Start");
        ++p.session;
        try { prepare(p); }
        catch (...) { release(p); throw; }
        p.started = Clock::now(); p.running = true; p.eos = false; p.have_frame = false;
        p.current_buffer = 0; p.current_tag = 0;
        p.frame_generation = 0; p.subtitle_epoch = UINT32_MAX;
    });
}
int32_t stop(uint32_t self) { return operation(self, "stop", [](Player& p) {
    if (p.running || p.have_frame)
        std::fprintf(stderr, "[macross-sail] guest Stop player=%08X session=%u presented=%u last_pts=%lld eos=%d\n",
            p.self, p.session, p.current_tag, static_cast<long long>(p.presented_pts), int(p.eos));
    release(p);
}); }
void close(uint32_t self) { (void)stop(self); }
void destroy(uint32_t self) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto i = players.find(self);
    if (i == players.end()) return;
    try { release(*i->second); } catch (const std::exception& e) { std::fprintf(stderr, "[macross-sail] cleanup: %s\n", e.what()); }
    for(auto db=databases.begin();db!=databases.end();){uint32_t d=db->first;bool owned=db->second.player==self;++db;if(owned)destroy_database(d,0);}
    players.erase(i);
}

int32_t get_format(uint32_t ea, uint32_t output) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    try {
        auto p = adapter_player(ea);
        if (!output || !p->allocated) return kFailure;
        std::memcpy(vm_base + output, p->format.data(), 32); return 0;
    } catch (...) { return kFailure; }
}
bool audio_time_position(const Player& p, int64_t pts, uint64_t& output) {
    if (!p.audio_anchor_tag || p.audio_anchor_time > uint64_t(INT64_MAX)) return false;
    const int64_t delta = pts - p.audio_anchor_pts;
    if (delta > INT64_MAX / 1000000 || delta < INT64_MIN / 1000000) return false;
    const int64_t us = delta * 1000000 / 90000;
    if (us < 0 && uint64_t(-us) > p.audio_anchor_time) return false;
    if (us > 0 && uint64_t(us) > uint64_t(INT64_MAX) - p.audio_anchor_time) return false;
    output = uint64_t(int64_t(p.audio_anchor_time) + us); return true;
}
int32_t pts_to_time(uint32_t ea, uint64_t pts, uint32_t output) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    try {
        auto p = adapter_player(ea);
        if (!output || pts > uint64_t(INT64_MAX)) return kFailure;
        // TimePosition: stream-relative. The title stores this as its playback
        // position; the audio clock (host uptime) made the opening fade at once.
        int64_t ticks = int64_t(pts) - p->timeline_pts;
        write64(output, ticks > 0 ? uint64_t(ticks) * 1000000 / 90000 : 0); return 0;
    } catch (...) { return kFailure; }
}
int32_t get_frame(uint32_t ea, uint32_t info, uint32_t previous, uint32_t flip_time, uint64_t flags) {
    (void)previous; (void)flags;
    std::unique_lock<std::recursive_mutex> guard(lock);
    std::shared_ptr<Player> p;
    try {
        p = adapter_player(ea);
        if (!info || !p->running || p->eos || p->video_eof) return 255;
        const auto now = Clock::now();
        int64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(now - p->started).count() * 90000 / 1000000;
        bool next = false;
        if (!p->pending && !p->decoding && ticks >= p->presented_pts - p->timeline_pts + 3003) {
            const uint32_t session = p->session;
            bool decoded = false;
            p->decoding = true;
            {
                std::unique_lock<std::mutex> video(p->video_mutex);
                guard.unlock();
                try { decoded = decode_pending(*p); }
                catch (...) { video.unlock(); guard.lock(); p->decoding = false; throw; }
            }
            guard.lock();
            p->decoding = false;
            if (p->session != session || !p->running || !p->allocated) return 255;
            if (!decoded) {
                p->video_eof = true;
                std::fprintf(stderr, "[macross-sail] real decoded EOF after %u presented frames\n", p->current_tag);
                finish_if_drained(*p);
                return 255;
            }
        }
        if (p->pending && ticks >= p->pending_pts - p->timeline_pts) {
            if (p->have_frame) p->current_buffer = (p->current_buffer + 1) % kBuffers;
            const uint32_t buffer = p->buffers[p->current_buffer];
            p->presented_pixels = p->pending_pixels;
            p->presented_pts = p->pending_pts; p->pending = false; p->have_frame = true;
            write_movie_subtitle_frame(*p, true);
            ++p->current_tag; next = true;
            if (const char* dump = std::getenv("MACROSS_SAIL_DUMP"); dump && *dump && p->current_tag % 30 == 0) {
                // Diagnostic: the exact I420 bytes handed to the guest.
                const std::string out = std::string(dump) + "/sail-" + std::to_string(p->current_tag) + "-" +
                    std::to_string(p->width) + "x" + std::to_string(p->height) + ".i420";
                if (FILE* fp = std::fopen(out.c_str(), "wb")) { std::fwrite(p->pending_pixels.data(), 1, p->pending_pixels.size(), fp); std::fclose(fp); }
            }
            if (p->current_tag <= 3 || p->current_tag % 300 == 0)
                std::fprintf(stderr, "[macross-sail] present frame=%u pts=%lld buffer=%08X coding=%d\n",
                    p->current_tag - 1, static_cast<long long>(p->presented_pts), buffer, p->coding);
        }
        if (!p->have_frame) return 255;
        // F9 can hide/show the cue while the same decoded frame repeats. Use
        // its original snapshot, not the next pending frame. Publish a fresh
        // image generation but never advance media PTS or decoded-frame count.
        if (!next && write_movie_subtitle_frame(*p, false)) next = true;
        write32(info, p->buffers[p->current_buffer]); write32(info + 4, p->session);
        write32(info + 8, p->frame_generation); write32(info + 12, 0); write64(info + 16, uint64_t(p->presented_pts));
        if (flip_time) write64(flip_time, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()));
        return (next ? 0x200 : 0x100) | 4; // NEXT/REPEATED, ON_TIME.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[macross-sail] frame failed: %s\n", e.what());
        if (p) { p->running = false; cellSailPlaybackNotifyError(p->self, kFailure); }
        return 255;
    }
}

std::shared_ptr<Player> sound_player(uint32_t ea) {
    for (auto& item : players) if (item.second->sound_adapter == ea) return item.second;
    throw std::runtime_error("SAIL sound adapter has no player");
}
int32_t sound_format(uint32_t ea, uint32_t output) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    try {
        auto p = sound_player(ea);
        if (!output || !p->audio_buffer) return kFailure;
        std::memcpy(vm_base + output, p->audio_format.data(), 24); return 0;
    } catch (...) { return kFailure; }
}
int32_t sound_pts_to_time(uint32_t ea, uint64_t pts, uint32_t output) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    try {
        auto p = sound_player(ea);
        if (!output || pts > uint64_t(INT64_MAX)) return kFailure;
        // Same stream-relative TimePosition as the graphics adapter.
        const int64_t ticks = int64_t(pts) - p->timeline_pts;
        write64(output, ticks > 0 ? uint64_t(ticks) * 1000000 / 90000 : 0); return 0;
    } catch (...) { return kFailure; }
}
int32_t sound_update(uint32_t ea, uint32_t tag, uint64_t time) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    try {
        auto p = sound_player(ea);
        const auto& mark = p->audio_history[tag % p->audio_history.size()];
        if (!tag || mark.tag != tag || tag < p->audio_anchor_tag || time > uint64_t(INT64_MAX)) return kFailure;
        p->audio_anchor_tag = tag; p->audio_anchor_pts = mark.pts; p->audio_anchor_time = time;
        return 0;
    }
    catch (...) { return kFailure; }
}
int32_t sound_finalize(uint32_t ea) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    for (auto& item : players) if (item.second->sound_adapter == ea) {
        if (item.second->running) return kFailure;
        item.second->sound_adapter = 0;
    }
    sound_adapters.erase(ea); return 0;
}
// [sail-audio-stats] every 5 s: why title audio pulls did or did not get PCM.
struct AudioStats { std::atomic<uint64_t> calls{0}, served{0}, early{0}, idle{0}, drained{0}, lock_wait_max_us{0}; Clock::time_point since = Clock::now(); };
AudioStats audio_stats;
void audio_stats_note(std::atomic<uint64_t>& field) {
    ++field;
    const auto now = Clock::now();
    if (now - audio_stats.since < std::chrono::seconds(5)) return;
    audio_stats.since = now;
    std::fprintf(stderr, "[sail-audio-stats] calls=%llu served=%llu early=%llu idle=%llu drained=%llu lock_wait_max_us=%llu (5s)\n",
        (unsigned long long)audio_stats.calls.exchange(0), (unsigned long long)audio_stats.served.exchange(0),
        (unsigned long long)audio_stats.early.exchange(0), (unsigned long long)audio_stats.idle.exchange(0),
        (unsigned long long)audio_stats.drained.exchange(0), (unsigned long long)audio_stats.lock_wait_max_us.exchange(0));
}
int32_t sound_frame(uint32_t ea, uint32_t samples, uint32_t info) {
    ++audio_stats.calls;
    const auto wait_from = Clock::now();
    std::lock_guard<std::recursive_mutex> guard(lock);
    { const uint64_t waited = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - wait_from).count());
      uint64_t prev = audio_stats.lock_wait_max_us.load();
      while (waited > prev && !audio_stats.lock_wait_max_us.compare_exchange_weak(prev, waited)) {} }
    std::shared_ptr<Player> p;
    try {
        p = sound_player(ea);
        if (!info || !p->running || !p->audio_decoder || p->audio_eof) { audio_stats_note(audio_stats.idle); return 255; }
        if (!samples || samples > 2048) return kFailure;
        if (p->audio_cursor == p->audio_pcm.size() / 2 && !decode_audio(*p)) {
            audio_stats_note(audio_stats.drained); finish_if_drained(*p); return 255;
        }
        // One frame per block the cellAudio mixer consumes. The title sums
        // each frame into the current block (cellAudioAdd*Data); pacing on a
        // wall clock out of phase with the mixer handed it two frames in one
        // block (crackle) and none in the next (silence).
        const uint64_t block = cellAudio_blocks_mixed();
        if (p->audio_block_seen != UINT64_MAX) {
            if (block == p->audio_block_seen) { audio_stats_note(audio_stats.early); return 255; }
            // Blocks the title missed played silence; drop their PCM so the
            // movie audio stays on the video's clock.
            uint64_t missed = block - p->audio_block_seen - 1;
            if (missed > 8) missed = 8;
            for (uint64_t m = 0; m < missed * samples; ) {
                if (p->audio_cursor == p->audio_pcm.size() / 2 && !decode_audio(*p)) {
                    audio_stats_note(audio_stats.drained); finish_if_drained(*p); return 255;
                }
                const uint64_t take = std::min<uint64_t>(missed * samples - m, p->audio_pcm.size() / 2 - p->audio_cursor);
                p->audio_cursor += uint32_t(take); m += take;
            }
        }
        p->audio_block_seen = block;
        if (p->audio_cursor == p->audio_pcm.size() / 2 && !decode_audio(*p)) {
            audio_stats_note(audio_stats.drained); finish_if_drained(*p); return 255;
        }
        const int64_t pts = p->audio_pts + int64_t(p->audio_cursor) * 90000 / 48000;
        // The game consumes this buffer synchronously through cellAudioAdd*.
        // Never return repeated PCM or pad an incomplete final block as success.
        uint32_t written = 0;
        while (written < samples) {
            if (p->audio_cursor == p->audio_pcm.size() / 2 && !decode_audio(*p)) {
                finish_if_drained(*p); return 255;
            }
            const uint32_t take = std::min(samples - written, uint32_t(p->audio_pcm.size() / 2) - p->audio_cursor);
            for (uint32_t i = 0; i < take * 2; ++i) {
                uint32_t bits;
                std::memcpy(&bits, &p->audio_pcm[p->audio_cursor * 2 + i], 4);
                write32(p->audio_buffer + (written * 2 + i) * 4, bits);
            }
            p->audio_cursor += take; written += take;
        }
        write32(info, p->audio_buffer); write32(info + 4, p->session);
        write32(info + 8, ++p->audio_tag); write32(info + 12, 0);
        p->audio_history[p->audio_tag % p->audio_history.size()] = {p->audio_tag, pts};
        write64(info + 16, uint64_t(pts));
        if (p->audio_tag <= 4 || p->audio_tag % 1024 == 0)
            std::fprintf(stderr, "[macross-sail-audio] session=%u tag=%u samples=%u pts=%lld buffer=%08X\n",
                p->session, p->audio_tag, samples, static_cast<long long>(pts), p->audio_buffer);
        audio_stats_note(audio_stats.served);
        return 0x204;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[macross-sail-audio] pull failed: %s\n", e.what());
        if (p) { p->running = false; cellSailPlaybackNotifyError(p->self, kFailure); }
        return 255;
    }
}
} // namespace

extern "C" void macross_sail_install(void) {
    const char* enabled = std::getenv("MACROSS_SAIL_PLAYBACK");
    if (!enabled || std::strcmp(enabled, "1")) return;
    g_cell_sail_playback.allocator_initialize = allocator_init;
    g_cell_sail_playback.initialize = initialize;
    g_cell_sail_playback.graphics_initialize = graphics_init;
    g_cell_sail_playback.preferred_format = preferred;
    g_cell_sail_playback.set_graphics = attach;
    g_cell_sail_playback.descriptor = descriptor;
    g_cell_sail_playback.create_database = create_database;
    g_cell_sail_playback.destroy_database = destroy_database;
    g_cell_sail_playback.open = open;
    g_cell_sail_playback.start = start;
    g_cell_sail_playback.stop = stop;
    g_cell_sail_playback.close = close;
    g_cell_sail_playback.destroy = destroy;
    g_cell_sail_playback.get_frame = get_frame;
    g_cell_sail_playback.get_format = get_format;
    g_cell_sail_playback.pts_to_time = pts_to_time;
    g_cell_sail_playback.sound_initialize = sound_init;
    g_cell_sail_playback.sound_preferred_format = sound_preferred;
    g_cell_sail_playback.set_sound = attach_sound;
    g_cell_sail_playback.sound_get_format = sound_format;
    g_cell_sail_playback.sound_pts_to_time = sound_pts_to_time;
    g_cell_sail_playback.sound_update_av_sync = sound_update;
    g_cell_sail_playback.sound_finalize = sound_finalize;
    g_cell_sail_playback.sound_get_frame = sound_frame;
    std::fprintf(stderr, "[macross-sail] opt-in original-video playback installed\n");
}
