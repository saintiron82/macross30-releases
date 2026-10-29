#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace macross30::subtitles {

// Explicitly loaded, owner-thread-only timeline. No movie/draft auto-discovery.
// The caller must authorize content and supply the manifest's PTS origin.
class Timeline final {
public:
    static constexpr std::size_t kMaxFileBytes = 1024 * 1024;
    static constexpr std::size_t kMaxCues = 4096;
    static constexpr std::size_t kMaxCueBytes = 4096;
    static constexpr std::size_t kMaxUriBytes = 1023;

    // A load attempt (including failure) clears all old content and selection.
    // movie_filename is a basename, e.g. "020.pam"; never a URI or path.
    bool LoadSrt(std::string_view movie_filename, std::string_view utf8_srt,
                 std::string* error = nullptr);
    bool LoadFile(const std::filesystem::path& trusted_directory,
                  std::string_view srt_filename, std::string_view movie_filename,
                  std::string* error = nullptr);
    void Clear() noexcept;
    void Stop() noexcept;

    // Absolute media PTS, NOT wall time. A paused movie uses the same PTS.
    // Start is inclusive, end exclusive. Seeking/repeated PTS never catches up.
    // playing=false handles stop/end; a session/URI/origin change resets selection.
    // OFF immediately returns empty while retaining explicitly loaded content.
    // This call performs no allocation, file I/O, string construction or logging.
    // Returned view remains valid until Load*/Clear/destruction; empty when hidden.
    std::string_view Update(bool enabled, std::uint64_t session,
                            std::string_view uri, std::int64_t pts90k,
                            std::int64_t origin_pts90k, bool playing) noexcept;
    std::size_t CueCount() const noexcept { return cues_.size(); }

private:
    struct Cue { std::int64_t start90k, end90k; std::string text; };
    std::vector<Cue> cues_;
    std::string movie_;
    std::array<char, kMaxUriBytes + 1> uri_{};
    std::size_t uri_size_ = 0;
    std::uint64_t session_ = 0;
    std::int64_t origin_ = 0;
    bool selected_ = false;
};

} // namespace macross30::subtitles
