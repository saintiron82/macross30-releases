#include "battle_voice_catalog.h"
#include <iterator>

namespace macross30::battle_voice {
namespace {
struct VoiceLine { std::string_view stem, text; };
constexpr VoiceLine lines[] = {
#include "battle_voice_catalog.inc"
};
bool valid(std::string_view key) {
    if (key.size() < 8 || key.size() > 63) return false;
    const auto slash = key.find('/');
    if (slash == 0 || slash == key.npos || key.size()-slash-1 != 6) return false;
    for (auto c : key.substr(0,slash))
        if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_')) return false;
    for (auto c : key.substr(slash+1)) if (c<'0'||c>'9') return false;
    return true;
}
}
std::string_view Find(std::string_view key) noexcept {
    if (!valid(key)) return {};
    size_t first=0, last=std::size(lines);
    while(first<last) {
        const size_t middle=first+(last-first)/2;
        if(lines[middle].stem<key) first=middle+1; else last=middle;
    }
    return first<std::size(lines)&&lines[first].stem==key ? lines[first].text : std::string_view{};
}
}
