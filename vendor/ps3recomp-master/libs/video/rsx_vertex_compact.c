#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    /* The format is the same for every component: choose it once, not once
     * per component (this runs per vertex per attribute on the renderer
     * thread). Same results as the per-component switch it replaces,
     * including size 0 -> defaults/1 and an unknown type -> 0. */
    const u32 n = size < 4u ? size : 4u;
    if (!n)
        return 1;
    switch (type) {
    case RSX_VTX_TYPE_FLOAT:
        for (u32 c = 0; c < n; c++) {
            u32 w; memcpy(&w, source + c * 4, 4);
            w = __builtin_bswap32(w);
            memcpy(&out[c], &w, 4);
        }
        return 1;
    case RSX_VTX_TYPE_HALF:
        for (u32 c = 0; c < n; c++) out[c] = rsx_compact_be_f16(source + c * 2);
        return 1;
    case RSX_VTX_TYPE_UNORM8:
        for (u32 c = 0; c < n; c++) out[c] = source[c] / 255.0f;
        return 1;
    case RSX_VTX_TYPE_UINT8:
        for (u32 c = 0; c < n; c++) out[c] = (float)source[c];
        return 1;
    case RSX_VTX_TYPE_SNORM16:
        for (u32 c = 0; c < n; c++) {
            const s16 value = (s16)((source[c * 2] << 8) | source[c * 2 + 1]);
            out[c] = value / 32767.0f;
        }
        return 1;
    case RSX_VTX_TYPE_SINT16:
        for (u32 c = 0; c < n; c++) {
            const s16 value = (s16)((source[c * 2] << 8) | source[c * 2 + 1]);
            out[c] = (float)value;
        }
        return 1;
    case RSX_VTX_TYPE_CMP32: {
        const u32 word =
            ((u32)source[0] << 24) | ((u32)source[1] << 16) |
            ((u32)source[2] << 8) | (u32)source[3];
        s32 x = (s32)(word & 0x7FF);
        s32 y = (s32)((word >> 11) & 0x7FF);
        s32 z = (s32)((word >> 22) & 0x3FF);
        if (x & 0x400) x -= 0x800;
        if (y & 0x400) y -= 0x800;
        if (z & 0x200) z -= 0x400;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        out[3] = 1.0f;
        return 1;
    }
    default:
        return 0;
    }
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_dsp_vertex_default(rsx, attr, fetch->default_value);
        if (attr == 3 &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (frequency 0/1) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        if (fetch->desc.frequency <= 1) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_element_index(
                        refs[i].vertex_id, refs[i].base_index, 0, 0);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix =
            (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        float* value = (float*)(out + slot * 16u);
        if (!fetch->desc.type || !fetch->desc.size) {
            memcpy(value, fetch->default_value, 16);
            continue;
        }

        const u32 element = rsx_vertex_element_index(
            ref->vertex_id, ref->base_index, fetch->desc.frequency,
            (plan->divider_mask >> attr) & 1u);
        const u8* source = NULL;
        if (fetch->stable_base &&
            element >= fetch->stable_first && element <= fetch->stable_last) {
            source = fetch->stable_base +
                (u64)(element - fetch->stable_first) * fetch->stride;
        } else if (plan->inline_data) {
            /* An inline draw has no vertex array to fall back to: the array
             * offsets still hold whatever the last ordinary draw bound, and
             * reading them would feed this draw another mesh's vertices.
             * An attribute the stream does not supply gets its default,
             * which is what the hardware feeds. */
            source = NULL;
        } else if (plan->guest_ptr) {
            const u32 offset =
                plan->base_offset + fetch->desc.offset +
                element * fetch->stride;
            source = plan->guest_ptr(
                plan->guest_user, fetch->desc.location, offset,
                fetch->elem_size);
        }

        if (source && rsx_vertex_decode_element(
                fetch->desc.type, fetch->desc.size, source, value))
            continue;
        if (attr == 0)
            return 0;
        memcpy(value, fetch->default_value, 16);
    }
    return 1;
}

/* Fetch vertices vertex_first .. vertex_first+count-1 (one base index, as
 * rsx_vertex_remap_build_dense leaves them) attribute by attribute instead
 * of one vertex at a time. Same per-component arithmetic as
 * rsx_vertex_decode_element. Returns 0 (caller falls back to
 * rsx_vertex_fetch_one) for inline data, dividers or instanced attributes. */
int rsx_vertex_fetch_range(
    const rsx_vertex_fetch_plan* plan, u32 vertex_first, u32 base_index,
    u32 count, u8* out)
{
    if (!plan || !out || !count || plan->inline_data || !plan->guest_ptr)
        return 0;
    const u32 ostride = plan->layout.stride;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        if (!fetch->desc.type || !fetch->desc.size)
            continue;
        /* Frequency 0/1 is plain per-vertex fetch whatever the divider
         * (modulo) bit says (rsx_vertex_element_index). */
        if (fetch->desc.frequency > 1)
            return 0;
    }
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        u8* dst = out + slot * 16u;
        if (!fetch->desc.type || !fetch->desc.size) {
            for (u32 i = 0; i < count; i++) memcpy(dst + (u64)i * ostride, fetch->default_value, 16);
            continue;
        }
        const u32 e0 = rsx_vertex_element_index(vertex_first, base_index, 0, 0);
        if (rsx_vertex_element_index(vertex_first + count - 1u, base_index, 0, 0) != e0 + count - 1u)
            return 0;
        const u32 stride = fetch->stride;
        const u8* src = NULL;
        if (fetch->stable_base && e0 >= fetch->stable_first && e0 + count - 1u <= fetch->stable_last) {
            src = fetch->stable_base + (u64)(e0 - fetch->stable_first) * stride;
        } else {
            const u64 off = (u64)plan->base_offset + fetch->desc.offset + (u64)e0 * stride;
            const u64 span = (u64)(count - 1u) * stride + fetch->elem_size;
            if (off > UINT_MAX || span > UINT_MAX || off + span > 0x100000000ull) return 0;
            src = plan->guest_ptr(plan->guest_user, fetch->desc.location, (u32)off, (u32)span);
            if (!src) return 0;
        }
        const u32 n = fetch->desc.size < 4u ? fetch->desc.size : 4u;
        if (fetch->desc.type == RSX_VTX_TYPE_FLOAT) {
            for (u32 i = 0; i < count; i++) {
                const u8* sp = src + (u64)i * stride;
                float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                for (u32 c = 0; c < n; c++) {
                    u32 w; memcpy(&w, sp + c * 4, 4);
                    w = __builtin_bswap32(w);
                    memcpy(&v[c], &w, 4);
                }
                memcpy(dst + (u64)i * ostride, v, 16);
            }
            continue;
        }
        if (fetch->desc.type == RSX_VTX_TYPE_CMP32) {
            for (u32 i = 0; i < count; i++) {
                const u8* sp = src + (u64)i * stride;
                const u32 word = ((u32)sp[0] << 24) | ((u32)sp[1] << 16) | ((u32)sp[2] << 8) | (u32)sp[3];
                s32 x = (s32)(word & 0x7FF), y = (s32)((word >> 11) & 0x7FF), z = (s32)((word >> 22) & 0x3FF);
                if (x & 0x400) x -= 0x800;
                if (y & 0x400) y -= 0x800;
                if (z & 0x200) z -= 0x400;
                const float v[4] = { (float)x / 1023.0f, (float)y / 1023.0f, (float)z / 511.0f, 1.0f };
                memcpy(dst + (u64)i * ostride, v, 16);
            }
            continue;
        }
        if (fetch->desc.type == RSX_VTX_TYPE_HALF) {
            for (u32 i = 0; i < count; i++) {
                const u8* sp = src + (u64)i * stride;
                float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                for (u32 c = 0; c < n; c++) v[c] = rsx_compact_be_f16(sp + c * 2);
                memcpy(dst + (u64)i * ostride, v, 16);
            }
            continue;
        }
        for (u32 i = 0; i < count; i++) {
            float* v = (float*)(dst + (u64)i * ostride);
            if (!rsx_vertex_decode_element(fetch->desc.type, fetch->desc.size, src + (u64)i * stride, v)) {
                if (attr == 0) return 0;
                memcpy(v, fetch->default_value, 16);
            }
        }
    }
    return 1;
}

/* Fetch arbitrary refs attribute by attribute (the order of
 * rsx_vertex_fetch_one, results identical): needs every active attribute's
 * prepared stable range (rsx_vertex_fetch_plan_prepare) and no dividers or
 * instanced frequency. Returns 0 to fall back to rsx_vertex_fetch_one. */
int rsx_vertex_fetch_gather(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
    u32 count, u8* out)
{
    if (!plan || !out || !refs || !count || plan->inline_data)
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        if (!fetch->desc.type || !fetch->desc.size)
            continue;
        if (fetch->desc.frequency > 1 || !fetch->stable_base)
            return 0;
    }
    for (u32 i = 0; i < count; i++) {   /* every element inside every stable range */
        const u32 e = rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index, 0, 0);
        for (u32 slot = 0; slot < plan->layout.count; slot++) {
            const rsx_vertex_fetch_attr* fetch = &plan->attr[plan->layout.attrs[slot]];
            if (fetch->stable_base && (e < fetch->stable_first || e > fetch->stable_last))
                return 0;
        }
    }
    const u32 ostride = plan->layout.stride;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        u8* dst = out + slot * 16u;
        if (!fetch->desc.type || !fetch->desc.size) {
            for (u32 i = 0; i < count; i++) memcpy(dst + (u64)i * ostride, fetch->default_value, 16);
            continue;
        }
        const u8* base = fetch->stable_base;
        const u32 first = fetch->stable_first, stride = fetch->stride;
        const u32 n = fetch->desc.size < 4u ? fetch->desc.size : 4u;
        if (fetch->desc.type == RSX_VTX_TYPE_FLOAT) {
            for (u32 i = 0; i < count; i++) {
                const u32 e = rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index, 0, 0);
                const u8* sp = base + (u64)(e - first) * stride;
                float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                for (u32 c = 0; c < n; c++) {
                    u32 w; memcpy(&w, sp + c * 4, 4);
                    w = __builtin_bswap32(w);
                    memcpy(&v[c], &w, 4);
                }
                memcpy(dst + (u64)i * ostride, v, 16);
            }
            continue;
        }
        for (u32 i = 0; i < count; i++) {
            const u32 e = rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index, 0, 0);
            float* v = (float*)(dst + (u64)i * ostride);
            if (!rsx_vertex_decode_element(fetch->desc.type, fetch->desc.size, base + (u64)(e - first) * stride, v)) {
                if (attr == 0) return 0;
                memcpy(v, fetch->default_value, 16);
            }
        }
    }
    return 1;
}

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

int rsx_vertex_remap_build_dense(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || !refs || !unique_count || count < 6)
        return 0;
    const u32 base = refs[0].base_index;
    u32 lo = refs[0].vertex_id, hi = lo;
    for (u32 i = 1; i < count; i++) {
        if (refs[i].base_index != base) return 0;
        const u32 v = refs[i].vertex_id;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    const u32 range = hi - lo + 1u;
    if (range > count) return 0;   /* at most one unused vertex fetched per index */
    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }
    for (u32 i = 0; i < count; i++)
        remap->occurrence_to_unique[i] = refs[i].vertex_id - lo;
    for (u32 i = 0; i < range; i++) {
        refs[i].vertex_id = lo + i;
        refs[i].base_index = base;
    }
    *unique_count = range;
    return 1;
}

/* rsx_vertex_remap_index: static inline in rsx_vertex_compact.h */

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
