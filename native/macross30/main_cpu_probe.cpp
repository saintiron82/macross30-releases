// Opt-in bounded observation of the guest main host thread. Samples are wall
// observations; GetThreadTimes reports CPU separately. Never used as an FPS test.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
uint64_t file_ticks(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
bool cpu_ticks(HANDLE thread, uint64_t& result) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetThreadTimes(thread, &c, &e, &k, &u)) return false;
    result = file_ticks(k) + file_ticks(u); return true;
}
unsigned setting(const char* name, unsigned fallback, unsigned maximum) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr; const unsigned long n = std::strtoul(value, &end, 10);
    return end != value && !*end && n <= maximum ? unsigned(n) : fallback;
}
struct Bucket { unsigned count = 0; std::string module; uint64_t rva = 0, function_rva = 0; bool unwind = false; };
void observe(DWORD tid, std::string output, unsigned delay, unsigned duration, unsigned interval) {
    Sleep(delay);
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    const DWORD open_error = h ? 0 : GetLastError();
    unsigned attempts = 0, suspended = 0, contexts = 0, suspend_errors = 0, context_errors = 0, resume_errors = 0;
    uint64_t cpu_begin = 0, cpu_end = 0;
    bool begin_ok = h && cpu_ticks(h, cpu_begin);
    std::map<uint64_t, Bucket> buckets;
    const auto started = GetTickCount64();
    while (h && GetTickCount64() - started < duration) {
        ++attempts;
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
        bool got = false;
        if (SuspendThread(h) != DWORD(-1)) {
            ++suspended;
            got = GetThreadContext(h, &ctx) != 0;
            if (!got) ++context_errors;
            // No allocation, module lookup, stack walk or logging while paused.
            if (ResumeThread(h) == DWORD(-1)) { ++resume_errors; break; }
        } else ++suspend_errors;
        if (got) {
            ++contexts;
            const uint64_t key = ctx.Rip & ~uint64_t(15);
            auto& b = buckets[key];
            if (!b.count) {
                HMODULE module = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCSTR>(ctx.Rip), &module)) {
                    char name[MAX_PATH]{}; GetModuleFileNameA(module, name, MAX_PATH);
                    b.module = name; auto slash = b.module.find_last_of("\\/");
                    if (slash != std::string::npos) b.module.erase(0, slash + 1);
                    b.rva = ctx.Rip - reinterpret_cast<uint64_t>(module);
                    DWORD64 image = 0;
                    const auto* fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
                    if (fn) { b.unwind = true; b.function_rva = image + fn->BeginAddress - reinterpret_cast<uint64_t>(module); }
                }
            }
            ++b.count;
        }
        Sleep(interval);
    }
    const auto elapsed = GetTickCount64() - started;
    const bool end_ok = h && cpu_ticks(h, cpu_end);
    if (h) CloseHandle(h);
    std::vector<Bucket> sorted;
    for (const auto& pair : buckets) sorted.push_back(pair.second);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
    std::ofstream file(output);
    file << "{\n\"host_thread\":" << tid << ",\"open_error\":" << open_error
         << ",\"attempts\":" << attempts << ",\"suspended\":" << suspended << ",\"contexts\":" << contexts
         << ",\"suspend_errors\":" << suspend_errors << ",\"context_errors\":" << context_errors
         << ",\"resume_errors\":" << resume_errors << ",\"elapsed_ms\":" << elapsed << ",\"cpu_ms\":";
    if (begin_ok && end_ok) file << double(cpu_end - cpu_begin) / 10000.0; else file << "null";
    file << ",\"buckets\":[";
    bool first = true;
    for (const auto& b : sorted) {
        if (!first) file << ','; first = false;
        file << "{\"count\":" << b.count << ",\"module\":" << std::quoted(b.module)
             << ",\"rva\":" << b.rva << ",\"unwind_extent_known\":" << (b.unwind ? "true" : "false")
             << ",\"function_rva\":" << b.function_rva << '}';
    }
    file << "],\"limits\":[\"Wall observations include waits and are not CPU-weighted stacks.\","
            "\"Raw RIP buckets require exact symbol/extent verification; nearest symbol alone is insufficient.\","
            "\"Sampling perturbs scheduling; this run is diagnostic, not an FPS acceptance run.\"]}\n";
}
}
extern "C" void macross_main_cpu_probe_install(void) {
    const char* on = std::getenv("MACROSS_MAIN_CPU_PROBE");
    const char* output = std::getenv("MACROSS_MAIN_CPU_PROBE_OUTPUT");
    if (!on || std::string(on) != "1" || !output || !*output) return;
    std::thread(observe, GetCurrentThreadId(), std::string(output),
                setting("MACROSS_MAIN_CPU_PROBE_FROM_MS", 80000, 500000),
                setting("MACROSS_MAIN_CPU_PROBE_DURATION_MS", 10000, 60000),
                std::max(1u, setting("MACROSS_MAIN_CPU_PROBE_INTERVAL_MS", 10, 1000))).detach();
}
#else
extern "C" void macross_main_cpu_probe_install(void) {}
#endif
