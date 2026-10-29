#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macross30::subtitles {

// Owner-thread-only CPU overlay. Does not own or modify the original I420 frame.
class Renderer final {
public:
    struct Rect { int x = 0, y = 0, width = 0, height = 0; };
    // I420 = tightly packed Y, U, V planes; even dimensions required.
    // OFF/empty/invalid/failure returns the exact input span, with no OFF allocation
    // or GDI call. ON returns cached scratch, valid until next GetFrame/Clear.
    // Text/dimension changes rasterize once. Every ON frame starts from original,
    // so repeated frames never accumulate/ghost previously displayed text.
    std::span<const std::uint8_t> GetFrame(std::span<const std::uint8_t> original_i420,
                                         int width, int height,
                                         std::string_view utf8_text,
                                         bool enabled) noexcept;
    void Clear() noexcept;
    Rect LastRect() const noexcept { return rect_; }
    std::uint64_t Rasterizations() const noexcept { return rasterizations_; }

private:
    bool BuildMask(int width, int height, std::string_view text);
    std::string cached_text_;
    int width_ = 0, height_ = 0;
    bool cache_valid_ = false;
    Rect rect_{};
    std::vector<std::uint8_t> fill_, outline_, scratch_;
    std::uint64_t rasterizations_ = 0;
};
} // namespace macross30::subtitles
