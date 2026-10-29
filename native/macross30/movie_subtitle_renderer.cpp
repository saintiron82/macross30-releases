#include "movie_subtitle_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace macross30::subtitles {
namespace {
struct GdiSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    HFONT font = nullptr;
    HGDIOBJ old_font = nullptr;
    ~GdiSurface() {
        if (dc && old_font) SelectObject(dc, old_font);
        if (font) DeleteObject(font);
        if (dc && old_bitmap) SelectObject(dc, old_bitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};
std::uint8_t blend(std::uint8_t original, unsigned target, unsigned alpha) noexcept {
    return static_cast<std::uint8_t>((original*(255-alpha)+target*alpha+127)/255);
}
} // namespace

void Renderer::Clear() noexcept {
    cached_text_.clear();
    width_ = height_ = 0;
    cache_valid_ = false;
    rect_ = {};
    fill_.clear();
    outline_.clear();
    scratch_.clear();
}

bool Renderer::BuildMask(int width, int height, std::string_view utf8) {
    cache_valid_ = false;
    rect_ = {};
    fill_.clear();
    outline_.clear();
    cached_text_.assign(utf8);
    width_ = width;
    height_ = height;
    if (utf8.size() > 4096 || utf8.find('\0') != utf8.npos) return false;
    for (unsigned char c : utf8)
        if ((c < 32 && c != '\n' && c != '\t' && c != '\r') || c == 127) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (!count) return false;
    std::wstring text(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                           static_cast<int>(utf8.size()), text.data(), count) != count) return false;

    const int horizontal_margin = std::max(16, width*6/100);
    const int bottom_margin = std::max(16, height*5/100);
    const int radius = height >= 600 ? 2 : 1;
    const int padding = radius+2;
    const int available_width = (width-2*horizontal_margin-2*padding) & ~1;
    if (available_width < 32) return false;
    const int desired_height = std::max(18, (height*30+360)/720);
    const int minimum_height = std::max(14, (height*18+360)/720);
    GdiSurface surface;
    surface.dc = CreateCompatibleDC(nullptr);
    if (!surface.dc) return false;
    RECT measured{};
    int text_height = 0;
    int chosen_height = desired_height;
    const UINT flags = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL;
    for (; chosen_height >= minimum_height; --chosen_height) {
        if (surface.font) {
            SelectObject(surface.dc, surface.old_font);
            DeleteObject(surface.font);
            surface.font = nullptr;
        }
        surface.font = CreateFontW(-chosen_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                  FALSE, HANGUL_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
        if (!surface.font) return false;
        const HGDIOBJ previous = SelectObject(surface.dc, surface.font);
        if (!previous || previous == HGDI_ERROR) return false;
        if (!surface.old_font) surface.old_font = previous;
        TEXTMETRICW metrics{};
        if (!GetTextMetricsW(surface.dc, &metrics)) return false;
        measured = {0, 0, available_width, 0};
        if (!DrawTextW(surface.dc, text.data(), count, &measured, flags | DT_CALCRECT)) return false;
        text_height = measured.bottom-measured.top;
        // Reject overlong content instead of silently dropping lines or clipping.
        if (text_height > 0 && text_height <= 3*metrics.tmHeight &&
            measured.right-measured.left <= available_width &&
            text_height+2*padding+bottom_margin < height/2) break;
    }
    if (chosen_height < minimum_height) return false;
    int mask_width = available_width+2*padding;
    int mask_height = (text_height+2*padding+1)&~1;
    const auto mask_size = static_cast<std::size_t>(mask_width)*mask_height;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = mask_width;
    info.bmiHeader.biHeight = -mask_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    surface.bitmap = CreateDIBSection(surface.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!surface.bitmap || !bits) return false;
    surface.old_bitmap = SelectObject(surface.dc, surface.bitmap);
    if (!surface.old_bitmap || surface.old_bitmap == HGDI_ERROR) return false;
    std::memset(bits, 0, mask_size*4);
    if (SetBkMode(surface.dc, TRANSPARENT) == 0 ||
        SetTextColor(surface.dc, RGB(255,255,255)) == CLR_INVALID) return false;
    RECT draw = {padding, padding, padding+available_width, padding+text_height};
    if (!DrawTextW(surface.dc, text.data(), count, &draw, flags)) return false;
    if (!GdiFlush()) return false;
    ++rasterizations_;
    fill_.resize(mask_size);
    outline_.assign(mask_size, 0);
    const auto* pixels = static_cast<const std::uint8_t*>(bits);
    bool visible = false;
    for (std::size_t i=0; i<mask_size; ++i) {
        // GDI DIB alpha is unspecified. Use RGB coverage, never alpha.
        fill_[i] = std::max({pixels[4*i],pixels[4*i+1],pixels[4*i+2]});
        visible |= fill_[i] != 0;
    }
    if (!visible) return false;
    for (int y=0; y<mask_height; ++y) for (int x=0; x<mask_width; ++x) {
        unsigned coverage = 0;
        for (int dy=-radius; dy<=radius; ++dy) for (int dx=-radius; dx<=radius; ++dx) {
            const int xx=x+dx, yy=y+dy;
            if (xx>=0 && yy>=0 && xx<mask_width && yy<mask_height)
                coverage=std::max(coverage,static_cast<unsigned>(fill_[static_cast<std::size_t>(yy)*mask_width+xx]));
        }
        outline_[static_cast<std::size_t>(y)*mask_width+x] = static_cast<std::uint8_t>(coverage);
    }
    rect_ = {(width-mask_width)/2 & ~1,
             (height-bottom_margin-mask_height) & ~1, mask_width, mask_height};
    scratch_.resize(static_cast<std::size_t>(width)*height*3/2);
    cache_valid_ = true;
    return true;
}

std::span<const std::uint8_t> Renderer::GetFrame(std::span<const std::uint8_t> original,
                                                int width, int height,
                                                std::string_view text, bool enabled) noexcept {
    if (!enabled || text.empty()) return original;
    if (width < 64 || height < 64 || width > 8192 || height > 8192 ||
        (width & 1) || (height & 1)) return original;
    const auto pixels = static_cast<std::size_t>(width)*height;
    if (original.size() != pixels*3/2) return original;
    try {
        if (width_ != width || height_ != height || cached_text_ != text) {
            if (!BuildMask(width,height,text)) return original;
        }
        if (!cache_valid_) return original;
        // Always rebuild from the untouched input, never from a prior overlay.
        std::memcpy(scratch_.data(), original.data(), original.size());
        const auto& r=rect_;
        for (int y=0; y<r.height; ++y) for (int x=0; x<r.width; ++x) {
            const auto mask=static_cast<std::size_t>(y)*r.width+x;
            if (!outline_[mask]) continue;
            const auto at=static_cast<std::size_t>(r.y+y)*width+r.x+x;
            scratch_[at]=blend(blend(original[at],16,outline_[mask]),235,fill_[mask]);
        }
        const auto chroma=pixels/4;
        for (int y=0; y<r.height; y+=2) for (int x=0; x<r.width; x+=2) {
            const auto i=static_cast<std::size_t>(y)*r.width+x;
            const unsigned coverage=(static_cast<unsigned>(outline_[i])+outline_[i+1]+
                                     outline_[i+r.width]+outline_[i+r.width+1]+2)/4;
            if (!coverage) continue;
            const auto at=static_cast<std::size_t>((r.y+y)/2)*(width/2)+(r.x+x)/2;
            scratch_[pixels+at]=blend(original[pixels+at],128,coverage);
            scratch_[pixels+chroma+at]=blend(original[pixels+chroma+at],128,coverage);
        }
        return scratch_;
    } catch (...) {
        cache_valid_ = false;
        return original;
    }
}
} // namespace macross30::subtitles
