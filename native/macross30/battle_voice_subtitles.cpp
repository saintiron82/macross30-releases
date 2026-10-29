// Optional reaction-voice captions. No RNG, audio commands, queue completion,
// sample counters or guest timers are changed by this observer.
#include "battle_voice_file_subtitles.h"
#include "battle_voice_catalog.h"
#include "recompiled/ppu_recomp.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <string>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
// Runtime allocator shares the ordinary PPU-stack allocator lock. It reserves
// one private 256 KiB callback stack and returns usable_end - 0x100.
extern "C" uint32_t ppu_allocate_callback_stack(void);

namespace {
constexpr uint32_t main_toc = 0x00AD16BC, core_toc = 0x00AC1BA0;
constexpr size_t stack_bytes = 0x10000, helper_gap = 0x1000;
constexpr uint32_t callback_stack_bytes = 0x40000, callback_top_padding = 0x100;
constexpr size_t max_text_bytes = 254;
std::atomic<uint64_t> script_epoch{0};
std::atomic<uint64_t> voice_generation{0};
std::atomic<DWORD> ui_thread_id{0};
thread_local bool internal_call = false;

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("MACROSS_BATTLE_SUBTITLES");
        return value && std::strcmp(value, "1") == 0;
    }();
    return on;
}

bool accessible(uint32_t ea, size_t bytes, bool write = false) {
    if (!vm_base || !ea || !bytes || uint64_t(ea) + bytes > 0x100000000ULL) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(vm_base) + ea;
    if (start < reinterpret_cast<uintptr_t>(vm_base) || start + bytes < start) return false;
    uintptr_t cursor = start;
    const uintptr_t end = start + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD p = info.Protect & 255;
        const bool writable = p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                              p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
        const bool readable = writable || p == PAGE_READONLY || p == PAGE_EXECUTE_READ;
        if (!readable || (write && !writable)) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

std::optional<std::string_view> guest_text(uint32_t ea, size_t limit) {
    // Read only through the actual NUL, even at a committed page's boundary.
    if (!vm_base || !ea || !limit) return {};
    size_t used=0;
    const char* start=reinterpret_cast<const char*>(vm_base+ea);
    while (used<limit) {
        const uint64_t address=uint64_t(ea)+used;
        if(address>0xFFFFFFFFULL||!accessible(uint32_t(address),1)) return {};
        MEMORY_BASIC_INFORMATION info{};
        const char* cursor=start+used;
        if(!VirtualQuery(cursor,&info,sizeof(info))) return {};
        const uintptr_t end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        const size_t available=end-reinterpret_cast<uintptr_t>(cursor);
        const size_t chunk=available<limit-used?available:limit-used;
        if(!chunk) return {};
        if(const auto nul=static_cast<const char*>(std::memchr(cursor,0,chunk)))
            return std::string_view(start,size_t(nul-start));
        used+=chunk;
    }
    return {};
}

void admission_gap(const char* reason) {
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
        std::fprintf(stderr, "[battle-subtitles] admission skipped: %s\n", reason);
}

void trace_event(const char* event, std::string_view value) {
    static const bool on = [] {
        const char* flag = std::getenv("MACROSS_BATTLE_SUBTITLE_DIAG");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    static std::atomic<unsigned> count{0};
    if (on && count.fetch_add(1, std::memory_order_relaxed) < 64)
        std::fprintf(stderr, "[battle-subtitles] %s: %.*s\n", event, int(value.size()), value.data());
}

struct LayoutProbe { std::string source, text; };
const LayoutProbe& layout_probe() {
    static const LayoutProbe probe=[] {
        LayoutProbe value;
        const char* flag=std::getenv("MACROSS_BATTLE_CAPTION_LAYOUT_PROBE");
        if(!flag||std::strcmp(flag,"1")!=0)return value;
        auto invalid=[](const char* why){std::fprintf(stderr,"[battle-subtitles] layout-probe invalid: %s; normal captions retained\n",why);};
        const char* file=std::getenv("MACROSS_BATTLE_CAPTION_LAYOUT_PROBE_FILE");
        if(!file||!file[0]){invalid("missing file");return value;}
        const size_t length=std::strlen(file);
        const bool absolute=(length>2&&((file[0]>='A'&&file[0]<='Z')||(file[0]>='a'&&file[0]<='z'))&&file[1]==':'&&(file[2]=='\\'||file[2]=='/')) || (length>2&&file[0]=='\\'&&file[1]=='\\');
        if(!absolute||length>=32768){invalid("file must be an absolute UTF8 path");return value;}
        std::array<wchar_t,32768> wide{};
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,file,-1,wide.data(),int(wide.size()))){invalid("invalid UTF8 file path");return value;}
        FILE* input=_wfopen(wide.data(),L"rb");
        if(!input){invalid("cannot open file");return value;}
        std::array<char,255> bytes{};const size_t count=std::fread(bytes.data(),1,bytes.size(),input);
        const bool io_error=std::ferror(input)!=0;std::fclose(input);
        if(io_error||!count||count>max_text_bytes||std::memchr(bytes.data(),0,count)){invalid("empty, oversized, NUL or read error");return value;}
        std::array<wchar_t,255> chars{};
        const int converted=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),int(count),chars.data(),int(chars.size()));
        if(!converted){invalid("invalid UTF8 caption");return value;}
        for(int i=0;i<converted;++i)if((chars[i]<32&&chars[i]!='\n')||(chars[i]>=127&&chars[i]<=159)){invalid("unsupported caption control");return value;}
        const std::string_view text(bytes.data(),count);
        const auto lf=text.find('\n');
        if(!text.starts_with("[배치 시험]")||lf==std::string_view::npos||lf==0||lf+1==count||text.find('\n',lf+1)!=std::string_view::npos){invalid("expected labelled two-line caption");return value;}
        value.source.assign(file,length);value.text.assign(text);return value;
    }();
    return probe;
}
std::string_view layout_text(std::string_view normal,std::string_view stem) {
    // Unknown voices remain unknown. Default path keeps Find's immutable view,
    // with no file access, string allocation or guest state change.
    if(normal.empty())return normal;
    const auto& probe=layout_probe();if(probe.text.empty())return normal;
    trace_event("layout-probe-source",probe.source);
    trace_event("layout-probe-stem",stem);
    trace_event("layout-probe",probe.text);
    return probe.text;
}

struct Playback {
    bool valid = false;
    bool busy = false;
    uint32_t voice = 0;
};

Playback playback() {
    // Read the same status used by EQWaitVoice (00070DDC), without its reset.
    if (!accessible(core_toc - 0x5014, 4) || !accessible(core_toc + 0x7D4, 4) ||
        !accessible(core_toc - 0x4F8C, 4)) return {};
    const uint32_t voice = vm_read32(core_toc - 0x5014);
    const uint32_t stream = vm_read32(core_toc + 0x7D4);
    const uint32_t one_shot = vm_read32(core_toc - 0x4F8C);
    if (!accessible(voice, 0x78) || !accessible(stream, 4) || !accessible(one_shot, 1)) return {};
    const int32_t a = int32_t(vm_read32(voice + 0x70));
    const int32_t b = int32_t(vm_read32(voice + 0x74));
    const bool done = vm_read8(one_shot) != 0 ||
        (vm_read32(stream) == 0 && (a == 0 || b == 0 || (a < 0 && b < 0)));
    return {true, !done, voice};
}

uint32_t caption_object() {
    if (!accessible(main_toc - 0x6220, 4)) return 0;
    const uint32_t slot = vm_read32(main_toc - 0x6220);
    if (!accessible(slot, 4)) return 0;
    const uint32_t parent = vm_read32(slot);
    if (!accessible(parent, 0x990)) return 0;
    const uint32_t object = vm_read32(parent + 0x98C);
    return accessible(object, 0x110, true) ? object : 0;
}

struct Pending {
    ppu_context* context = nullptr;
    uint8_t* memory = nullptr;
    std::string_view text;
    uint64_t epoch = 0;
    uint64_t generation = 0;
    Playback before;
};
// A proposal never retains a guest-context pointer or worker-stack lifetime.
// Catalog string_views refer to immutable compiled storage.
struct Proposal {
    uint8_t* memory = nullptr;
    std::string_view text;
    uint64_t epoch = 0;
    uint64_t generation = 0;
    uint32_t voice = 0;
};
struct Owner {
    uint8_t* memory = nullptr;
    uint32_t object = 0;
    uint32_t voice = 0;
    std::string_view text;
    uint64_t epoch = 0;
    uint64_t generation = 0;
    bool active = false;
};
thread_local Pending pending;
// Only the thread bound by the first native caption-update tick may access its
// owner or call the guest UI. Voice observers may run on any host/PPU thread.
thread_local Owner owner;
std::mutex proposal_mutex;
Proposal proposal;
std::atomic<bool> proposal_available{false};

bool current(const Proposal& value) {
    return value.memory == vm_base &&
        value.epoch == script_epoch.load(std::memory_order_acquire) &&
        value.generation == voice_generation.load(std::memory_order_acquire);
}

bool publish(const Proposal& value) {
    std::lock_guard<std::mutex> lock(proposal_mutex);
    if (!current(value)) return false;
    proposal = value;
    proposal_available.store(true, std::memory_order_release);
    return true;
}

void retry_proposal(const Proposal& value) {
    std::lock_guard<std::mutex> lock(proposal_mutex);
    if (!current(value) || proposal_available.load(std::memory_order_relaxed)) return;
    proposal = value;
    proposal_available.store(true, std::memory_order_release);
}

std::optional<Proposal> take_proposal() {
    std::lock_guard<std::mutex> lock(proposal_mutex);
    if (!proposal_available.load(std::memory_order_relaxed)) return {};
    const Proposal result = proposal;
    proposal = {};
    proposal_available.store(false, std::memory_order_release);
    return result;
}

bool still_ours(const Owner& value) {
    return value.active && value.memory == vm_base &&
        value.epoch == script_epoch.load(std::memory_order_acquire) &&
        caption_object() == value.object && accessible(value.object, value.text.size() + 1) &&
        std::memcmp(vm_base + value.object, value.text.data(), value.text.size()) == 0 &&
        vm_base[value.object + value.text.size()] == 0;
}

struct HelperStack {
    uint8_t* memory = nullptr;
    uint32_t base = 0;
    uint32_t top = 0;
    bool attempted = false;
    bool ready = false;
};
thread_local HelperStack helper_stack;

void helper_failure(const char* reason, const ppu_context* ctx, uint32_t object) {
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    const uint64_t sp = ctx ? ctx->gpr[1] : 0;
    MEMORY_BASIC_INFORMATION info{};
    const bool queried = vm_base && sp <= 0xFFFFFFFFULL &&
        VirtualQuery(vm_base + uint32_t(sp), &info, sizeof(info)) != 0;
    const uintptr_t vm = reinterpret_cast<uintptr_t>(vm_base);
    const uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
    std::fprintf(stderr,
        "[battle-subtitles] helper-frame reason=%s sp=%016llX object=%08X "
        "caller_region=%08llX bytes=%llu state=%08lX protect=%08lX "
        "owned_base=%08X owned_top=%08X\n", reason,
        static_cast<unsigned long long>(sp), object,
        static_cast<unsigned long long>(queried && region >= vm ? region-vm : 0),
        static_cast<unsigned long long>(queried ? info.RegionSize : 0),
        queried ? info.State : 0UL, queried ? info.Protect : 0UL,
        helper_stack.base, helper_stack.top);
}

bool private_reservation(uint32_t base, uint32_t bytes) {
    if (!vm_base || !base || uint64_t(base)+bytes > 0x100000000ULL) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(vm_base)+base;
    const uintptr_t end = start+bytes;
    if (start < reinterpret_cast<uintptr_t>(vm_base) || end < start) return false;
    uintptr_t cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.AllocationBase != vm_base ||
            (info.State != MEM_RESERVE && info.State != MEM_COMMIT)) return false;
        // Committing our usable span must not turn an existing guard/noaccess
        // page writable. The runtime allocator's bottom guard lies OUTSIDE it.
        if (info.State == MEM_COMMIT && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

bool prepare_helper_stack(const ppu_context* ctx, uint32_t object) {
    if (helper_stack.memory != vm_base) helper_stack = {vm_base};
    if (!helper_stack.attempted) {
        helper_stack.attempted = true; // Never consume a new allocation each tick.
        uint8_t* const allocation_memory = vm_base;
        const uint32_t top = ppu_allocate_callback_stack();
        if (allocation_memory != vm_base) { helper_failure("VM changed during allocation", ctx, object); return false; }
        if (!top || (top & 15) || top < callback_stack_bytes-callback_top_padding) {
            helper_failure("callback allocation failed", ctx, object); return false;
        }
        const uint32_t base = top-(callback_stack_bytes-callback_top_padding);
        helper_stack.base = base;
        helper_stack.top = top;
        if ((base & 0xFFF) || !private_reservation(base, callback_stack_bytes)) {
            helper_failure("callback reservation contract", ctx, object); return false;
        }
        if (VirtualAlloc(vm_base+base, callback_stack_bytes, MEM_COMMIT, PAGE_READWRITE) != vm_base+base ||
            !accessible(base, callback_stack_bytes, true)) {
            helper_failure("owned callback commit failed", ctx, object); return false;
        }
        helper_stack.ready = true;
        const uint64_t original_sp = ctx ? ctx->gpr[1] : 0;
        const bool old_span = original_sp >= stack_bytes && original_sp <= 0xFFFFFFFFULL &&
            accessible(uint32_t(original_sp-stack_bytes), stack_bytes, true);
        std::fprintf(stderr,
            "[battle-subtitles] helper-stack owned base=%08X top=%08X usable=%u "
            "caller_sp=%016llX caller_below64k_committed=%s\n",
            base, top, callback_stack_bytes,
            static_cast<unsigned long long>(original_sp), old_span ? "yes" : "no");
    }
    if (!helper_stack.ready || helper_stack.memory != vm_base ||
        !accessible(helper_stack.base, callback_stack_bytes, true)) {
        helper_failure("owned callback bounds unavailable", ctx, object); return false;
    }
    return true;
}

// Use an allocator-owned private guest stack, not memory below the caller's
// actual SP. The flat VM is demand-paged: a valid caller need not have 64 KiB
// already committed below it. Preserve the complete context/TLS and private
// helper slice, including exceptional exits; never touch caller stack bytes.
struct GuestCall {
    ppu_context* context;
    ppu_context saved;
    ppu_context* active;
    void (*trampoline)(void*);
    uint8_t* memory;
    uint32_t begin;
    bool old_internal;
    std::array<uint8_t, stack_bytes> stack;

    GuestCall(ppu_context* ctx, uint32_t start)
        : context(ctx), saved(*ctx), active(g_active_ctx), trampoline(g_trampoline_fn),
          memory(vm_base), begin(start), old_internal(internal_call) {
        std::memcpy(stack.data(), memory + begin, stack.size());
        internal_call = true;
        g_trampoline_fn = nullptr;
        g_active_ctx = ctx;
    }
    ~GuestCall() {
        std::memcpy(memory + begin, stack.data(), stack.size());
        *context = saved;
        g_active_ctx = active;
        g_trampoline_fn = trampoline;
        internal_call = old_internal;
    }
    GuestCall(const GuestCall&) = delete;
    GuestCall& operator=(const GuestCall&) = delete;
};

bool set_caption(ppu_context* ctx, uint32_t object, std::string_view text) {
    if (!ctx || text.size() > max_text_bytes || text.find('\0') != text.npos) {
        helper_failure("caption text contract", ctx, object); return false;
    }
    const uint64_t sp = ctx->gpr[1];
    if (sp > 0xFFFFFFFFULL || (sp & 15) || !accessible(uint32_t(sp), 0x30)) {
        helper_failure("caller linkage unavailable", ctx, object); return false;
    }
    if (!prepare_helper_stack(ctx, object)) return false;
    const uint32_t begin = helper_stack.top-uint32_t(stack_bytes);
    if (uint64_t(object) < uint64_t(helper_stack.base)+callback_stack_bytes &&
        uint64_t(object)+0x110 > helper_stack.base) {
        helper_failure("caption overlaps callback stack", ctx, object); return false;
    }
    const uint32_t call_sp = helper_stack.top-uint32_t(helper_gap);
    const uint32_t string_ea = call_sp + 0x200;
    GuestCall guard(ctx, begin);
    if (!text.empty()) std::memcpy(vm_base + string_ea, text.data(), text.size());
    vm_base[string_ea + text.size()] = 0;
    ctx->gpr[1] = call_sp;
    ctx->gpr[2] = main_toc;
    ctx->gpr[3] = string_ea;
    ctx->lr = 0;
    func_00308E2C(ctx);
    while (g_trampoline_fn) {
        const auto fn = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
    }
    return true;
}

void retire_owner(ppu_context* ctx) {
    if (!owner.active) return;
    if (!still_ours(owner)) { owner = {}; return; }
    const Owner previous = owner;
    owner = {};
    try {
        if (!set_caption(ctx, previous.object, {})) owner = previous;
        else trace_event("retired", previous.text);
    } catch (...) {
        owner = previous;
        throw; // Preserve observer ownership as well as the guest unwind.
    }
}
}

extern "C" void macross_battle_subtitles_selected(ppu_context* ctx, uint32_t stem_ea) {
    if (!enabled() || internal_call) return;
    pending = {};
    if (!ctx) return;
    const uint64_t generation = voice_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto read=guest_text(stem_ea,64);
    if(!read) return;
    // A new selected request must not keep the preceding voice's line alive,
    // even if this stem has no known text or helper admission later fails.
    const auto stem=*read;
    trace_event("selected", stem);
    const auto text = layout_text(macross30::battle_voice::Find(stem),stem);
    pending = {ctx, vm_base, text, script_epoch.load(std::memory_order_acquire), generation, playback()};
}

extern "C" void macross_battle_subtitles_file_selected(ppu_context* ctx, uint32_t filename_ea) {
    if (!enabled() || internal_call) return;
    pending = {};
    if (!ctx) return;
    const uint64_t generation = voice_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto read=guest_text(filename_ea,320);
    if(!read) return;
    const char* chars=read->data();
    std::array<char,320> normalized{};
    const size_t size = read->size();
    for (size_t i=0;i<size;++i) {
        const unsigned char c=static_cast<unsigned char>(chars[i]);
        if (c>=128) return;
        normalized[i]=c>='A'&&c<='Z'?char(c-'A'+'a'):char(c);
    }
    std::string_view file(normalized.data(),size);
    constexpr std::string_view mounted="/dev_bdvd/ps3_game/usrdir/";
    if (file.starts_with(mounted)) file.remove_prefix(mounted.size());
    constexpr std::string_view installed="/dev_hdd0/game/bljs10184_install/usrdir/";
    if (file.starts_with(installed)) file.remove_prefix(installed.size());
    constexpr std::string_view prefix="data/sound/voice/", suffix=".at3";
    std::string_view key;
    if(file.starts_with(prefix)&&file.ends_with(suffix))
        key=file.substr(prefix.size(),file.size()-prefix.size()-suffix.size());
    trace_event("file-selected",file);
    const auto text=layout_text(macross30::battle_voice::Find(key),key);
    pending={ctx,vm_base,text,script_epoch.load(std::memory_order_acquire),generation,playback()};
}

extern "C" void macross_battle_subtitles_commit(ppu_context* ctx) {
    if (!enabled() || internal_call) return;
    const Pending request = pending;
    pending = {};
    if (!ctx || request.context != ctx || request.memory != vm_base) return;
    if (request.epoch != script_epoch.load(std::memory_order_acquire) ||
        request.generation != voice_generation.load(std::memory_order_acquire) ||
        request.text.empty() || request.text.size() > max_text_bytes) return;
    const Playback after = playback();
    if (!request.before.valid || !after.valid || request.before.busy || !after.busy ||
        request.before.voice != after.voice) {
        admission_gap("no unambiguous idle-to-busy native transition");
        return;
    }
    // No UI object, helper frame, or guest context escapes the observer thread.
    // The UI update consumes the immutable proposal no earlier than next tick.
    if (publish({request.memory, request.text, request.epoch, request.generation, after.voice}))
        trace_event("proposed", request.text);
}

extern "C" void macross_battle_subtitles_tick(ppu_context* ctx) {
    if (!enabled() || internal_call || !ctx) return;
    const DWORD thread = GetCurrentThreadId();
    DWORD bound = ui_thread_id.load(std::memory_order_acquire);
    if (!bound) {
        DWORD expected = 0;
        ui_thread_id.compare_exchange_strong(expected, thread, std::memory_order_acq_rel);
        bound = ui_thread_id.load(std::memory_order_acquire);
    }
    if (bound != thread) return;
    // No mutex, VM traversal or scratch copying in the normal idle fast path.
    if (!owner.active && !proposal_available.load(std::memory_order_acquire)) return;
    if (owner.active) {
        if (!still_ours(owner)) owner = {};
        else {
            const Playback now = playback();
            if (owner.generation != voice_generation.load(std::memory_order_acquire) ||
                !now.valid || now.voice != owner.voice || !now.busy) retire_owner(ctx);
        }
    }
    // If retirement could not acquire a safe helper frame, keep both ownership
    // and the queued proposal for a later UI tick; never overwrite the old pane.
    if (owner.active || !proposal_available.load(std::memory_order_acquire)) return;
    const auto queued = take_proposal(); // Unlock before entering any guest code.
    if (!queued || !current(*queued)) return;
    const Proposal value = *queued;
    const Playback now = playback();
    if (!now.valid || !now.busy || now.voice != value.voice) return;
    const uint32_t object = caption_object();
    if (!object) { retry_proposal(value); return; }
    if (vm_read8(object) != 0) return; // Original script text always wins.
    if (!current(value)) return;
    if (!set_caption(ctx, object, value.text)) {
        admission_gap("UI guest helper frame unavailable");
        retry_proposal(value);
        return;
    }
    Owner candidate{vm_base, object, value.voice, value.text, value.epoch, value.generation, true};
    if (still_ours(candidate)) {
        owner = candidate;
        trace_event("owned", value.text);
        // A different voice may have been selected while the guest helper ran.
        if (owner.generation != voice_generation.load(std::memory_order_acquire)) retire_owner(ctx);
    }
}

extern "C" void macross_battle_subtitles_script_caption(ppu_context*) {
    if (!enabled() || internal_call) return;
    script_epoch.fetch_add(1, std::memory_order_acq_rel);
    // Do not touch thread-local UI ownership or a worker's queued bytes here.
    // Epoch validation makes all older proposals/owners stale on the UI tick.
}

extern "C" void macross_battle_subtitles_reset(ppu_context*) {
    if (!enabled() || internal_call) return;
    pending = {};
    voice_generation.fetch_add(1, std::memory_order_acq_rel);
}
