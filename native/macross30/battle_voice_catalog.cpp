#include "battle_voice_catalog.h"
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <charconv>

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
struct ExternalCatalog {
    bool requested=false, accepted=false;
    std::vector<std::pair<std::string,std::string>> entries;
    struct Page { uint64_t start; std::string text; };
    std::vector<std::vector<Page>> sequences;
    std::unordered_map<const char*,size_t> page_keys;
};
ExternalCatalog load_external() noexcept {
    ExternalCatalog result;
    const char* path=std::getenv("MACROSS_BATTLE_CATALOG_FILE");
    if(!path||!*path)return result;
    result.requested=true;
    try {
        const auto file=std::filesystem::u8path(path);
        if(!file.is_absolute()||std::filesystem::file_size(file)>8*1024*1024)return result;
        std::ifstream input(file,std::ios::binary);
        std::string line;
        if(!std::getline(input,line))return result;
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        const bool russian=line=="MACROSS_BATTLE_CAPTIONS_V2\tru-RU";
        const char* locale=std::getenv("MACROSS_GAME_LANGUAGE");
        const std::string prefix="MACROSS_BATTLE_CAPTIONS_V3\t";
        const bool unicode=line.compare(0,prefix.size(),prefix)==0;
        if(unicode&&(!locale||line.substr(prefix.size())!=locale))return result;
        if(russian&&(!locale||std::string_view(locale)!="ru-RU"))return result;
        const bool paged=line=="MACROSS_BATTLE_CAPTIONS_V2\ten-US"||russian||unicode;
        if(!paged&&line!="MACROSS_BATTLE_CAPTIONS_V1\ten-US")return result;
        while(std::getline(input,line)) {
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            const auto tab=line.find('\t');
            if(tab==line.npos||tab==0||tab+1==line.size())return result;
            uint64_t duration=0;size_t text_at=tab+1;
            if(paged) {
                const auto next=line.find('\t',text_at);if(next==line.npos)return result;
                const auto number=std::from_chars(line.data()+text_at,line.data()+next,duration);
                if(number.ec!=std::errc{}||number.ptr!=line.data()+next||duration==0||duration>120000)return result;
                text_at=next+1;
            }
            if(line.find('\t',text_at)!=line.npos)return result;
            const std::string key=line.substr(0,tab),encoded=line.substr(text_at);
            if(!valid(key)||encoded.size()>(paged?8192:508)||result.entries.size()>=50000)return result;
            // English sidecars use printable ASCII. This also excludes NUL,
            // embedded newlines and accidentally supplied Korean catalogs.
            std::string text;
            for(size_t i=0;i<encoded.size();++i) {
                const unsigned char c=encoded[i];
                if(c<32)return result;
                if(c>126) {
                    // Russian sidecars admit only well-formed Cyrillic UTF-8.
                    // English keeps its original printable-ASCII admission.
                    if((!russian&&!unicode)||c<0xC2||c>(unicode?0xF4:0xEF))return result;
                    const size_t bytes=c<0xE0?2:(c<0xF0?3:4);
                    const size_t first=i;
                    if(encoded.size()-i<bytes)return result;
                    unsigned cp=c&(bytes==2?31u:(bytes==3?15u:7u));
                    for(size_t j=1;j<bytes;j++) {
                        const unsigned char next=encoded[++i];
                        if((next&0xC0)!=0x80)return result;
                        cp=(cp<<6)|(next&63u);
                    }
                    if((bytes==2&&cp<0x80)||(bytes==3&&cp<0x800)||(bytes==4&&cp<0x10000)||
                       cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF)||
                       (cp>=0x80&&cp<0xA0)||cp==0x2028||cp==0x2029)return result;
                    if(!unicode&&!((cp>=0x400&&cp<=0x52F)||cp==0xA0||cp==0xAB||cp==0xBB||
                         (cp>=0x2010&&cp<=0x2027)||cp==0x20AC||cp==0x2116||cp==0x266A))return result;
                    text.append(encoded,first,bytes);continue;
                }
                if(c=='\\') {
                    if(++i==encoded.size())return result;
                    if(encoded[i]=='n')text.push_back('\n');
                    else if(paged&&encoded[i]=='f')text.push_back('\f');
                    else if(encoded[i]=='\\')text.push_back('\\');
                    else return result;
                } else text.push_back(char(c));
            }
            if(text.empty())return result;
            std::vector<ExternalCatalog::Page> pages;
            size_t start=0,weight=0;
            do {
                const auto end=text.find('\f',start);
                auto part=text.substr(start,end==text.npos?text.npos:end-start);
                if(part.empty()||part.size()>254||pages.size()>=16)return result;
                pages.push_back({weight,std::move(part)});weight+=pages.back().text.size();
                if(end==text.npos)break;
                start=end+1;
            } while(start<=text.size());
            if(!paged&&pages.size()!=1)return result;
            for(auto& page:pages)page.start=duration*page.start/weight;
            for(size_t i=1;i<pages.size();++i)if(pages[i].start<=pages[i-1].start)return result;
            if(!result.entries.empty()&&result.entries.back().first>=key)return result;
            result.entries.emplace_back(key,pages.front().text);
            result.sequences.push_back(std::move(pages));
        }
        if(!input.eof()||result.entries.empty())return result;
        for(size_t i=0;i<result.entries.size();++i)
            if(result.sequences[i].size()>1)result.page_keys.emplace(result.entries[i].second.data(),i);
        result.accepted=true;
    } catch(...) { result.accepted=false; }
    return result;
}
const ExternalCatalog* external_catalog() {
    // MSVC's unordered_map default constructor allocates its sentinel. Do not
    // even construct an external catalog on the normal embedded Korean path.
    static const bool requested=[] {const char* path=std::getenv("MACROSS_BATTLE_CATALOG_FILE");return path&&*path;}();
    if(!requested)return nullptr;
    static const ExternalCatalog value=load_external();return &value;
}
}
std::string_view Find(std::string_view key) noexcept {
    if (!valid(key)) return {};
    const auto* external=external_catalog();
    if(external) {
        // An explicitly selected English catalog is fail-closed: no fallback
        // to embedded Korean text on malformed input or an unknown voice.
        if(!external->accepted)return {};
        size_t first=0,last=external->entries.size();
        while(first<last) {
            const size_t middle=first+(last-first)/2;
            if(external->entries[middle].first<key)first=middle+1;else last=middle;
        }
        return first<external->entries.size()&&external->entries[first].first==key
            ? std::string_view(external->entries[first].second):std::string_view{};
    }
    size_t first=0, last=std::size(lines);
    while(first<last) {
        const size_t middle=first+(last-first)/2;
        if(lines[middle].stem<key) first=middle+1; else last=middle;
    }
    return first<std::size(lines)&&lines[first].stem==key ? lines[first].text : std::string_view{};
}
std::string_view PageAt(std::string_view first,uint64_t elapsed_ms) noexcept {
    const auto* external=external_catalog();
    if(!external||!external->accepted)return first;
    const auto found=external->page_keys.find(first.data());
    if(found==external->page_keys.end())return first;
    const auto& pages=external->sequences[found->second];
    size_t index=0;
    while(index+1<pages.size()&&pages[index+1].start<=elapsed_ms)++index;
    return pages[index].text;
}
}
