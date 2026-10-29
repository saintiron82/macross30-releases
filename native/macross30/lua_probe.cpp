/* Diagnostic only: no guest writes, callback substitutions or error recovery. */
#include "lua_probe.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>
extern "C" unsigned char* vm_base;

namespace {
std::atomic<unsigned long long> entity_totals[12]{};
std::atomic<unsigned long long> entity_sequence{0}, entity_last_insert{0}, entity_last_clear{0};
std::atomic<uint32_t> entity_last_clear_stage{0};
void registry_probe(const void* context, unsigned reason, uint32_t status, uint64_t lr);
bool readable(uint32_t ea, size_t length) {
    if (!ea || uint64_t(ea) + length > 0x100000000ULL) return false;
    uintptr_t cursor = reinterpret_cast<uintptr_t>(vm_base + ea);
    const uintptr_t end = cursor + length;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
bool word(uint32_t ea, uint32_t& value) {
    if (!readable(ea, 4)) return false;
    const unsigned char* p = vm_base + ea;
    value = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    return true;
}
void dump(const char* label, uint32_t ea) {
    std::fprintf(stderr, "[lua-probe] %s=%08X words:", label, ea);
    for (unsigned n = 0; n < 8; ++n) {
        uint32_t value = 0;
        if (uint64_t(ea) + n * 4 + 4 <= 0x100000000ULL && word(ea + n * 4, value))
            std::fprintf(stderr, " %08X", value);
        else std::fprintf(stderr, " ????????");
    }
    std::fprintf(stderr, "\n");
}
}

extern "C" void macross_lua_precall_probe(const void* context, uint32_t state,
                                          uint64_t lr, uint64_t toc) {
    static const bool enabled = [] { const char* p = std::getenv("MACROSS_LUA_PROBE"); return p && !std::strcmp(p, "1"); }();
    if (!enabled) return;
    uint32_t ci = 0, slot = 0, closure = 0, opd = 0, code = 0, callee_toc = 0;
    bool valid = state <= 0xffffffebU && word(state + 0x14, ci) &&
        ci <= 0xfffffffbU && word(ci + 4, slot) && word(slot, closure) &&
        closure <= 0xffffffebU && word(closure + 0x10, opd) &&
        word(opd, code) && opd <= 0xfffffffbU && word(opd + 4, callee_toc);
    // Preserve the budget for bad closures after setup: four ordinary samples,
    // then twelve suspicious samples. Never assume a name pointer is an OPD.
    const bool suspicious = !valid || code < 0x10000 || code >= 0x900000 || (code & 3);
    static std::atomic<unsigned> ordinary{0}, abnormal{0};
    auto& counter = suspicious ? abnormal : ordinary;
    if (counter.fetch_add(1, std::memory_order_relaxed) >= (suspicious ? 12u : 4u)) return;
    std::fprintf(stderr, "[lua-probe] precall=%s ctx=%p hosttid=%lu state=%08X lr=%08llX toc=%08llX ci=%08X TValue=%08X closure=%08X OPD=%08X code=%08X callee_toc=%08X readable=%d\n",
        suspicious ? "SUSPICIOUS" : "sample", context, GetCurrentThreadId(), state,
        static_cast<unsigned long long>(lr), static_cast<unsigned long long>(toc),
        ci, slot, closure, opd, code, callee_toc, int(valid));
    dump("state", state); dump("TValue", slot); dump("closure", closure); dump("OPD", opd);
    // Independently inspect original OfferQuest name/function registration
    // reference slots. These are evidence, never used as recovery values.
    dump("OfferQuest refs", 0x00ACAB80);
    dump("OfferQuest initialized name/function pair", 0x00D95BD8);
}

extern "C" void macross_lua_protected_probe(const void* context, uint32_t state,
    uint32_t jump_buffer, uint64_t result, uint64_t stack, uint64_t lr,
    uint64_t toc, int after_setjmp) {
    static const bool enabled = [] { const char* p = std::getenv("MACROSS_LUA_PROBE"); return p && !std::strcmp(p, "1"); }();
    if (!enabled) return;
    static std::atomic<unsigned> count{0};
    if (count.fetch_add(1, std::memory_order_relaxed) >= 16) return;
    uint32_t protected_record = 0, status = 0;
    const bool record_ok = state <= 0xffffff8bU && word(state + 0x70, protected_record);
    const bool status_ok = jump_buffer <= 0xfffffddbU && word(jump_buffer + 0x220, status);
    std::fprintf(stderr, "[lua-probe] rawrunprotected %s ctx=%p hosttid=%lu state=%08X SP=%08llX lr=%08llX toc=%08llX jumpbuf=%08X result=%08llX L_errorJmp=%08X status=%08X readable=%d/%d\n",
        after_setjmp ? "AFTER-setjmp" : "BEFORE-setjmp", context, GetCurrentThreadId(), state,
        static_cast<unsigned long long>(stack), static_cast<unsigned long long>(lr),
        static_cast<unsigned long long>(toc), jump_buffer, static_cast<unsigned long long>(result),
        protected_record, status, int(record_ok), int(status_ok));
    dump("jumpbuf head", jump_buffer);
}

extern "C" void macross_lua_error_probe(const void* context, uint32_t state,
    uint32_t status, uint64_t stack, uint64_t lr) {
    static const bool enabled = [] { const char* p = std::getenv("MACROSS_LUA_PROBE"); return p && !std::strcmp(p, "1"); }();
    if (!enabled || !status) return;
    if (status == 2) registry_probe(context, 1, status, lr);
    static std::atomic<unsigned> errors{0};
    if (errors.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    uint32_t top = 0, tag = 0, object = 0, length = 0;
    const bool top_ok = state <= 0xfffffff3U && word(state + 8, top) && top >= 16;
    const uint32_t value = top_ok ? top - 16 : 0;
    const bool value_ok = top_ok && readable(value, 16) && word(value, object) && word(value + 8, tag);
    std::fprintf(stderr, "[lua-error] ctx=%p hosttid=%lu L=%08X status=%u SP=%08llX callerLR=%08llX top=%08X TValue=%08X tag=%u object=%08X readable=%d/%d\n",
        context, GetCurrentThreadId(), state, status, static_cast<unsigned long long>(stack),
        static_cast<unsigned long long>(lr), top, value, tag, object, int(top_ok), int(value_ok));
    dump("error TValue", value);
    /* Actual title layout: func003234F8 pushes TString pointers with tag4
     * into 16-byte TValue slots; func0033BF70 compares TString length at+C
     * and bytes at+10. Only decode this proven layout, never guess pointers. */
    if (!value_ok || tag != 4 || object > 0xffffffefU ||
        !readable(object, 16) || vm_base[object + 4] != 4 || !word(object + 12, length)) return;
    const uint32_t shown = length < 512 ? length : 512;
    if (!readable(object + 16, shown)) {
        std::fprintf(stderr, "[lua-error] TString=%08X length=%u payload unreadable\n", object, length);
        return;
    }
    std::fprintf(stderr, "[lua-error] TString=%08X length=%u shown=%u text=\"", object, length, shown);
    for (uint32_t n = 0; n < shown; ++n) {
        const unsigned char byte = vm_base[object + 16 + n];
        if (byte == '\n') std::fputs("\\n", stderr);
        else if (byte == '\r') std::fputs("\\r", stderr);
        else if (byte == '\t') std::fputs("\\t", stderr);
        else if (byte == '"' || byte == '\\') std::fprintf(stderr, "\\%c", byte);
        else if (byte < 32 || byte == 127) std::fprintf(stderr, "\\x%02X", byte);
        else std::fputc(byte, stderr);
    }
    std::fprintf(stderr, "\"%s\n", length > shown ? " (truncated)" : "");
}

namespace {
std::string probe_string(uint32_t ea) {
    std::string result;
    for (unsigned n = 0; n < 96; ++n) {
        if (uint64_t(ea) + n >= 0x100000000ULL || !readable(ea + n, 1)) return "<unreadable>";
        const unsigned char byte = vm_base[ea + n];
        if (!byte) return result;
        result += byte >= 32 && byte != 127 ? char(byte) : '?';
    }
    return result + "<truncated>";
}
bool binding_budget(bool suspicious) {
    static const bool enabled = [] { const char* p = std::getenv("MACROSS_LUA_PROBE"); return p && !std::strcmp(p, "1"); }();
    if (!enabled) return false;
    static std::atomic<unsigned> normal{0}, abnormal{0};
    return (suspicious ? abnormal : normal).fetch_add(1, std::memory_order_relaxed) < 16;
}
void registry_probe(const void* context, unsigned reason, uint32_t status, uint64_t lr) {
    static std::atomic<bool> captured[2]{};
    if (reason > 1 || captured[reason].exchange(true, std::memory_order_relaxed)) return;
    constexpr uint32_t owner = 0x00B94F60;
    uint32_t header = 0, root = 0, size_word = 0;
    const bool header_ok = word(owner + 0x58, header);
    const bool root_ok = header_ok && header <= 0xfffffff7U && word(header + 4, root);
    word(owner + 0x5c, size_word);
    std::fprintf(stderr, "[lua-registry] reason=%s status=%u callerLR=%08llX ctx=%p owner=%08X header=%08X root=%08X owner_plus5C=%08X readable=%d/%d empty_by_root=%d\n",
        reason ? "actual_Lua_status2" : "first_missing_PLAYER1_PID", status,
        static_cast<unsigned long long>(lr), context, owner, header, root, size_word, int(header_ok), int(root_ok),
        int(root_ok && (!root || root == header)));
    std::fprintf(stderr, "[lua-registry] entity_stage_total_entries 8EED8=%llu 90C5C=%llu 90CA4=%llu C5DE8=%llu CC100=%llu 70B87C_targetmap=%llu sequence=%llu last_insert_entry_seq=%llu last_clear_entry_seq=%llu last_clear_stage=%08X (entry counts, not successful insertions)\n",
        entity_totals[0].load(), entity_totals[1].load(), entity_totals[2].load(),
        entity_totals[3].load(), entity_totals[4].load(), entity_totals[5].load(),
        entity_sequence.load(), entity_last_insert.load(), entity_last_clear.load(), entity_last_clear_stage.load());
    std::fprintf(stderr, "[lua-registry] post_insert=%llu remove_pre=%llu remove_post=%llu create_lua_pre=%llu create_lua_post=%llu\n",
        entity_totals[6].load(), entity_totals[7].load(), entity_totals[8].load(), entity_totals[9].load(), entity_totals[10].load());
    std::fprintf(stderr, "[lua-registry] group_create_entries=%llu\n", entity_totals[11].load());
    dump("registry owner+50", owner + 0x50);
    dump("registry header", header);
    for (uint32_t special : {0x00B93D84u, 0x00B93D8Cu}) {
        uint32_t indirect = 0, object = 0, type = 0;
        const bool a = word(special, indirect);
        const bool b = a && word(indirect, object);
        const bool c = b && object <= 0xfffffff3U && word(object + 8, type);
        std::fprintf(stderr, "[lua-registry] special=%08X indirect=%08X entity=%08X type=%u readable=%d/%d/%d\n",
            special, indirect, object, type, int(a), int(b), int(c));
    }
    uint32_t pending[33]{}, visited[16]{};
    unsigned read = 0, write = 0, count = 0, unreadable = 0, sentinels = 0;
    if (root_ok && root && root != header) pending[write++] = root;
    while (read < write && count < 16) {
        const uint32_t node = pending[read++];
        if (!node || node == header) { ++sentinels; continue; }
        bool duplicate = false;
        for (unsigned n = 0; n < count; ++n) if (visited[n] == node) duplicate = true;
        if (duplicate) continue;
        visited[count++] = node;
        if (!readable(node, 0x16)) { ++unreadable; continue; }
        const unsigned nil = vm_base[node + 0x15];
        if (nil) { ++sentinels; continue; }
        uint32_t left = 0, right = 0, key = 0, entity = 0, type = 0;
        word(node, left); word(node + 8, right); word(node + 12, key); word(node + 16, entity);
        const bool type_ok = entity <= 0xfffffff3U && word(entity + 8, type);
        std::fprintf(stderr, "[lua-registry] node=%08X left=%08X right=%08X key=%08X name=%s entity=%08X type=%u type_readable=%d nil=%u\n",
            node, left, right, key, probe_string(key).c_str(), entity, type, int(type_ok), nil);
        if (write < 33 && left && left != header) pending[write++] = left;
        if (write < 33 && right && right != header) pending[write++] = right;
    }
    std::fprintf(stderr, "[lua-registry] bounded_snapshot visited=%u unreadable=%u sentinel_nodes=%u queued_remaining=%u node_limit=16 interpretation=%s\n",
        count, unreadable, sentinels, write - read,
        !root_ok ? "unreadable" : (!root || root == header) ? "empty_root" : "nonempty_root_review_nodes");
}
}
extern "C" void macross_lua_obj_probe(const void* context, int phase, uint32_t output,
    uint32_t name, uint32_t field, uint32_t entity, int32_t fieldenum) {
    const char* enabled = std::getenv("MACROSS_LUA_PROBE");
    if (!enabled || std::strcmp(enabled, "1") || phase == 0) return;
    const auto key = probe_string(field);
    if (key != "PID") return;
    if (phase == 1 && !entity && probe_string(name) == "PLAYER1") registry_probe(context, 0, 0, 0);
    uint32_t tag = 0, value = 0;
    const bool result_ok = phase == 2 && output <= 0xfffffff7U && word(output, tag) && word(output + 4, value);
    const bool bad = !entity || fieldenum < 0 || (result_ok && value == 0xffffffffU);
    if (!binding_budget(bad)) return;
    std::fprintf(stderr, "[lua-binding] GetObjInfo phase=%d ctx=%p name=%s field=%s entity=%08X fieldenum=%d out=%08X tag=%u value=%08X signed=%d readable=%d\n",
        phase, context, probe_string(name).c_str(), key.c_str(), entity, fieldenum,
        output, tag, value, int32_t(value), int(result_ok));
}
extern "C" void macross_lua_pilot_probe(const void* context, uint32_t output, uint32_t id,
    uint32_t field, uint32_t toc) {
    const char* enabled = std::getenv("MACROSS_LUA_PROBE");
    if (!enabled || std::strcmp(enabled, "1")) return;
    const auto key = probe_string(field);
    if (key != "VID" && key != "IDSTR") return;
    uint32_t tag = 0, value = 0, table = 0;
    const bool result_ok = output <= 0xfffffff7U && word(output, tag) && word(output + 4, value);
    const bool table_ok = toc >= 0x6198 && word(toc - 0x6198, table);
    const uint64_t row64 = uint64_t(table) + uint64_t(id) * 0xd8;
    const uint32_t row = table_ok && id <= 190 && row64 <= 0xffffffffULL ? uint32_t(row64) : 0;
    const bool bad = id > 190 || !result_ok || (tag == 0 && value == 0xffffffffU);
    if (!binding_budget(bad)) return;
    std::fprintf(stderr, "[lua-binding] GetPilotInfo ctx=%p id=%u signed_id=%d field=%s table=%08X row=%08X out=%08X tag=%u value=%08X signed=%d readable=%d table_readable=%d\n",
        context, id, int32_t(id), key.c_str(), table, row, output, tag, value, int32_t(value), int(result_ok), int(table_ok));
    if (result_ok && tag) std::fprintf(stderr, "[lua-binding] GetPilotInfo string=%s\n", probe_string(tag).c_str());
    if (row) dump("pilot row", row);
}

extern "C" void macross_entity_probe(const void* context, uint32_t stage,
    uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7,
    uint64_t lr, uint64_t toc) {
    static const bool enabled = [] { const char* p = std::getenv("MACROSS_LUA_PROBE"); return p && !std::strcmp(p, "1"); }();
    if (!enabled) return;
    const uint32_t stages[] = {0x0008EED8, 0x00090C5C, 0x00090CA4, 0x000C5DE8, 0x000CC100, 0x0070B87C,
                              0x8070B87C, 0x000CBA00, 0x800CBA00, 0x0030AFF0, 0x8030AFF0, 0x00090E7C};
    unsigned slot = 12;
    for (unsigned n = 0; n < 12; ++n) if (stages[n] == stage) slot = n;
    if (slot == 12 || ((stage == 0x0070B87C || stage == 0x8070B87C) && uint32_t(r4) != 0x00B94FB4)) return;
    const auto sequence = entity_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto sample = entity_totals[slot].fetch_add(1, std::memory_order_relaxed);
    if (stage == 0x0070B87C) entity_last_insert.store(sequence, std::memory_order_relaxed);
    if (stage == 0x000C5DE8 || stage == 0x000CC100) {
        entity_last_clear_stage.store(stage, std::memory_order_relaxed);
        entity_last_clear.store(sequence, std::memory_order_relaxed);
    }
    if (sample >= (stage == 0x00090E7C ? 8u : 16u)) return;
    uint32_t header = 0, root = 0, registry_size = 0;
    word(0x00B94FB8, header); word(0x00B94FBC, registry_size);
    if (header <= 0xfffffff7U) word(header + 4, root);
    std::fprintf(stderr, "[entity-probe] stage=%08X sample=%llu sequence=%llu ctx=%p hosttid=%lu r3=%08llX r4=%08llX r5=%08llX r6=%08llX r7=%08llX lr=%08llX toc=%08llX registry_header=%08X root=%08X size=%u\n",
        stage, sample, sequence, context, GetCurrentThreadId(), static_cast<unsigned long long>(r3),
        static_cast<unsigned long long>(r4), static_cast<unsigned long long>(r5),
        static_cast<unsigned long long>(r6), static_cast<unsigned long long>(r7),
        static_cast<unsigned long long>(lr), static_cast<unsigned long long>(toc), header, root, registry_size);
    const uint32_t record = uint32_t(r3);
    if (stage == 0x0070B87C || stage == 0x8070B87C) {
        uint32_t key = 0, entity = 0, type = 0;
        const uint32_t pair = uint32_t(r7);
        const bool valid = pair <= 0xfffffff7U && word(pair, key) && word(pair + 4, entity);
        const bool type_ok = entity <= 0xfffffff3U && word(entity + 8, type);
        std::fprintf(stderr, "[entity-probe] insert map=%08X pair=%08X key=%08X name=%s entity=%08X type=%u readable=%d/%d\n",
            uint32_t(r4), pair, key, probe_string(key).c_str(), entity, type, int(valid), int(type_ok));
        if (stage == 0x8070B87C) dump("postinsert result", uint32_t(r3));
    } else if (stage == 0x00090E7C) {
        uint32_t count = 0, array = 0;
        const bool valid = record <= 0xfffffff3U && word(record, count) && word(record + 8, array);
        const int32_t signed_group = int32_t(r4);
        const int64_t absolute_group = signed_group < 0 ? -int64_t(signed_group) : signed_group;
        const int16_t group = int16_t(absolute_group);
        std::fprintf(stderr, "[entity-group] header=%08X count=%u array=%08X signed_group=%d actual_short=%d flag5=%02X readable=%d\n",
            record, count, array, signed_group, int(group), unsigned(r5 & 255), int(valid));
        unsigned matches = 0, seen = 0;
        for (; valid && seen < count && seen < 16; ++seen) {
            const uint64_t address = uint64_t(array) + uint64_t(seen) * 0xd0;
            if (address > 0xffffffffULL || !readable(uint32_t(address), 0xcd)) break;
            const uint32_t row = uint32_t(address);
            uint32_t type = 0;
            word(row, type);
            const int16_t row_group = int16_t((unsigned(vm_base[row + 0x46]) << 8) | vm_base[row + 0x47]);
            const bool match = row_group == group;
            matches += match;
            std::fprintf(stderr, "[entity-group] index=%u row=%08X name=%s type=%u group=%d match=%d enabledCC=%u\n",
                seen, row, probe_string(row + 8).c_str(), type, int(row_group), int(match), unsigned(vm_base[row + 0xcc]));
        }
        std::fprintf(stderr, "[entity-group] reviewed=%u of=%u matching_reviewed=%u bounded_or_unreadable=%d\n",
            seen, count, matches, int(seen < count));
    } else if (stage == 0x00090CA4) {
        uint32_t count = 0, array = 0;
        const bool valid = record <= 0xfffffff3U && word(record, count) && word(record + 8, array);
        std::fprintf(stderr, "[entity-probe] bulk records=%08X count=%u stride=0xD0 readable=%d\n", array, count, int(valid));
        if (valid && count) dump("bulk first record", array);
    } else if (stage == 0x00090C5C || stage == 0x0008EED8) {
        uint32_t type = 0, name = 0, gate58 = 0;
        const bool valid = record <= 0xffffff2fU && readable(record, 0xcd);
        if (valid) {
            word(record, type); word(record + 8, name); word(record + 0x58, gate58);
            const unsigned gate34 = (unsigned(vm_base[record + 0x34]) << 8) | vm_base[record + 0x35];
            std::fprintf(stderr, "[entity-probe] record=%08X type=%u word8=%08X inline_name=%s enabledCC=%u gate34=%04X gate58=%08X\n",
                record, type, name, probe_string(record + 8).c_str(), unsigned(vm_base[record + 0xcc]), gate34, gate58);
        } else std::fprintf(stderr, "[entity-probe] record=%08X unreadable\n", record);
    } else if (stage == 0x000CBA00 || stage == 0x800CBA00) {
        uint32_t type = 0;
        const bool valid = record <= 0xfffffff3U && word(record + 8, type);
        std::fprintf(stderr, "[entity-probe] removal phase=%s entity=%08X type=%u readable=%d\n",
            stage == 0x000CBA00 ? "pre" : "post", record, type, int(valid));
        dump("removal entity", record);
    } else if (stage == 0x0030AFF0 || stage == 0x8030AFF0) {
        const uint32_t string_arg = uint32_t(r3 >> 32);
        double number_arg = 0;
        std::memcpy(&number_arg, &r4, sizeof(number_arg));
        std::fprintf(stderr, "[entity-probe] Lua create phase=%s r3_hi_stringEA=%08X text=%s r3_low=%08X r4_numeric_bits=%016llX numeric=%g (argument layout from original stack spills)\n",
            stage == 0x0030AFF0 ? "pre" : "post", string_arg,
            probe_string(string_arg).c_str(), uint32_t(r3), static_cast<unsigned long long>(r4), number_arg);
    } else {
        dump("owner first words", 0x00B94F60);
    }
}
