#include "movie_subtitle_timeline.h"

#include <cstring>
#include <fstream>
#include <limits>

namespace macross30::subtitles {
namespace {
bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
bool ascii_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}
char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}
bool same_name(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
bool filename(std::string_view s, std::string_view extension) noexcept {
    if (s.size() < extension.size() + 1 || s.size() > 127 ||
        !ascii_alnum(s.front()) || s.find("..") != s.npos ||
        !same_name(s.substr(s.size() - extension.size()), extension)) return false;
    for (char c : s) if (!ascii_alnum(c) && c != '.' && c != '_' && c != '-') return false;
    // Block Windows device basenames as well as separators/ADS/traversal above.
    const auto stem = s.substr(0, s.find('.'));
    if (same_name(stem, "CON") || same_name(stem, "PRN") ||
        same_name(stem, "AUX") || same_name(stem, "NUL")) return false;
    if (stem.size() == 4 && (same_name(stem.substr(0, 3), "COM") ||
                            same_name(stem.substr(0, 3), "LPT")) &&
        stem[3] >= '1' && stem[3] <= '9') return false;
    return true;
}
bool utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) {
            if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) return false;
            continue;
        }
        unsigned n = 0, cp = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; minimum = 0x10000; }
        else return false;
        if (n > text.size() - i) return false;
        while (n--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp == 0xfeff || (cp >= 0x80 && cp <= 0x9f)) return false;
    }
    return true;
}
bool timestamp(std::string_view s, std::int64_t& pts) noexcept {
    if (s.size() != 12 || s[2] != ':' || s[5] != ':' || s[8] != ',') return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i != 2 && i != 5 && i != 8 && (s[i] < '0' || s[i] > '9')) return false;
    const int hour = (s[0]-'0')*10+s[1]-'0';
    const int minute = (s[3]-'0')*10+s[4]-'0';
    const int second = (s[6]-'0')*10+s[7]-'0';
    const int ms = (s[9]-'0')*100+(s[10]-'0')*10+s[11]-'0';
    if (minute >= 60 || second >= 60) return false;
    pts = ((static_cast<std::int64_t>(hour)*3600 + minute*60 + second)*1000+ms)*90;
    return true;
}
bool sequence(std::string_view s, std::size_t expected) noexcept {
    if (s.empty() || s.front() == '0' || s.size() > 4) return false;
    std::size_t n = 0;
    for (char c : s) { if (c < '0' || c > '9') return false; n = n*10+c-'0'; }
    return n == expected;
}
} // namespace

void Timeline::Stop() noexcept {
    selected_ = false;
    session_ = 0;
    origin_ = 0;
    uri_size_ = 0;
    uri_[0] = 0;
}
void Timeline::Clear() noexcept {
    Stop();
    cues_.clear();
    movie_.clear();
}

bool Timeline::LoadSrt(std::string_view movie_filename, std::string_view srt,
                       std::string* error) {
    Clear();
    if (error) error->clear();
    if (!filename(movie_filename, ".pam")) return fail(error, "Invalid movie basename");
    if (srt.empty() || srt.size() > kMaxFileBytes) return fail(error, "Invalid SRT byte size");
    if (srt.substr(0, 3) == "\xef\xbb\xbf") srt.remove_prefix(3);
    if (!utf8(srt)) return fail(error, "Invalid UTF-8 or forbidden control character");
    for (std::size_t i = 0; i < srt.size(); ++i)
        if (srt[i] == '\r' && (i+1 == srt.size() || srt[i+1] != '\n'))
            return fail(error, "Bare CR is not valid SRT");
    std::vector<Cue> parsed;
    std::size_t cursor = 0;
    auto line = [&]() {
        const auto end = srt.find('\n', cursor);
        const auto stop = end == srt.npos ? srt.size() : end;
        auto value = srt.substr(cursor, stop-cursor);
        cursor = end == srt.npos ? srt.size() : end+1;
        if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
        return value;
    };
    while (cursor < srt.size()) {
        auto number = line();
        if (number.empty()) continue; // Explicit blank separators/trailing blanks.
        if (parsed.size() == kMaxCues || !sequence(number, parsed.size()+1))
            return fail(error, "Invalid cue index or too many cues");
        if (cursor >= srt.size()) return fail(error, "Missing timestamp");
        const auto times = line();
        std::int64_t start, end;
        if (times.size() != 29 || times.substr(12, 5) != " --> " ||
            !timestamp(times.substr(0, 12), start) || !timestamp(times.substr(17), end) ||
            end <= start || (!parsed.empty() && start < parsed.back().end90k))
            return fail(error, "Invalid, overlapping or unordered cue times");
        std::string text;
        while (cursor < srt.size()) {
            const auto value = line();
            if (value.empty()) break;
            std::int64_t ignored_start, ignored_end;
            if (value.size() == 29 && value.substr(12, 5) == " --> " &&
                timestamp(value.substr(0, 12), ignored_start) &&
                timestamp(value.substr(17), ignored_end))
                return fail(error, "Missing blank separator between cues");
            if (!text.empty()) text.push_back('\n');
            text.append(value);
            if (text.size() > kMaxCueBytes) return fail(error, "Cue text too long");
        }
        if (text.empty()) return fail(error, "Empty cue text");
        parsed.push_back({start, end, std::move(text)});
    }
    if (parsed.empty()) return fail(error, "No cues");
    movie_.assign(movie_filename);
    cues_ = std::move(parsed);
    return true;
}

bool Timeline::LoadFile(const std::filesystem::path& directory,
                        std::string_view srt_filename, std::string_view movie_filename,
                        std::string* error) {
    Clear();
    if (error) error->clear();
    if (!filename(srt_filename, ".srt") || !filename(movie_filename, ".pam"))
        return fail(error, "Invalid explicit subtitle/movie basename");
    // Directory is supplied by trusted content registration. No URI-based search.
    std::ifstream input(directory / std::string(srt_filename), std::ios::binary | std::ios::ate);
    if (!input) return fail(error, "Cannot open explicitly requested SRT");
    const auto size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxFileBytes))
        return fail(error, "Invalid SRT file size");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return fail(error, "Cannot read complete SRT");
    return LoadSrt(movie_filename, bytes, error);
}

std::string_view Timeline::Update(bool enabled, std::uint64_t session,
                                  std::string_view uri, std::int64_t pts,
                                  std::int64_t origin, bool playing) noexcept {
    if (!playing || uri.empty() || uri.size() > kMaxUriBytes ||
        uri.find('\0') != uri.npos || origin < 0) { Stop(); return {}; }
    const auto slash = uri.find_last_of("/\\");
    const auto basename = slash == uri.npos ? uri : uri.substr(slash+1);
    if (cues_.empty() || !same_name(basename, movie_)) { Stop(); return {}; }
    if (!selected_ || session != session_ || origin != origin_ ||
        uri.size() != uri_size_ || std::memcmp(uri.data(), uri_.data(), uri.size()) != 0) {
        Stop();
        std::memcpy(uri_.data(), uri.data(), uri.size());
        uri_[uri.size()] = 0;
        uri_size_ = uri.size();
        session_ = session;
        origin_ = origin;
        selected_ = true;
    }
    if (!enabled || pts < origin) return {};
    // origin is nonnegative and pts >= origin, so subtraction cannot overflow.
    const auto relative = pts-origin;
    std::size_t lo = 0, hi = cues_.size();
    while (lo < hi) {
        const auto mid = lo+(hi-lo)/2;
        if (cues_[mid].start90k <= relative) lo = mid+1;
        else hi = mid;
    }
    if (!lo) return {};
    const auto& cue = cues_[lo-1];
    return relative < cue.end90k ? std::string_view(cue.text) : std::string_view{};
}
} // namespace macross30::subtitles
