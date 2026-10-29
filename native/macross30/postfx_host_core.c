/* Post-processing taskset completion protocol, pure part (see
 * postfx_host_core.h and native/analysis/postfx-task-protocol.md). No guest
 * memory or runtime state here: tools/tests/test_postfx_host.py builds it on
 * its own and runs it against the game's lifted SPU code. */
#include "postfx_host_core.h"

#include <string.h>

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint64_t postfx_state_pack(postfx_state s)
{
    return ((uint64_t)s.phase << 48) | ((uint64_t)s.next << 32) | ((uint64_t)s.done << 16) | s.total;
}

postfx_state postfx_state_unpack(uint64_t v)
{
    postfx_state s;
    s.phase = (uint16_t)(v >> 48);
    s.next = (uint16_t)(v >> 32);
    s.done = (uint16_t)(v >> 16);
    s.total = (uint16_t)v;
    return s;
}

void postfx_header_parse(const uint8_t raw[16], postfx_header* h)
{
    h->state.phase = be16(raw);
    h->state.next = be16(raw + 2);
    h->state.done = be16(raw + 4);
    h->state.total = be16(raw + 6);
    h->phase_count = be16(raw + 8);
    h->pad = be16(raw + 10);
    h->desc_ea = be32(raw + 12);
}

void postfx_desc_parse(const uint8_t raw[128], postfx_desc* d)
{
    d->x = raw[0];
    d->y = raw[1];
    d->flags = raw[3];
    d->notify_ea = be32(raw + 4);
    d->notify_value = be32(raw + 8);
    d->code_ea = be32(raw + 12);
    d->code_size = be32(raw + 16);
    d->dma_size = be32(raw + 20);
    d->param_ea = be32(raw + 24);
    d->param_size = be32(raw + 28);
}

void postfx_mlaa_block_parse(const uint8_t raw[64], postfx_mlaa_block* b)
{
    b->label_value = be32(raw);
    b->label_ea = be32(raw + 4);
    b->counter_ea = be32(raw + 8);
    b->barrier_ea = be32(raw + 20);
    b->mode = raw[0x22];
}

void postfx_post_args_parse(const uint8_t raw[16], postfx_post_args* a)
{
    a->header_ea = be32(raw);
    a->counter_ea = be32(raw + 4);
    a->flag_ea = be32(raw + 8);
}

void postfx_mlaa_args_parse(const uint8_t raw[16], uint32_t* block_ea, uint32_t* flag_ea)
{
    *block_ea = be32(raw);
    *flag_ea = be32(raw + 4);
}

/* 0x3FF4..0x4008: mpy of the two count bytes, stored as the low halfword. */
uint16_t postfx_desc_items(const postfx_desc* d)
{
    return (uint16_t)((unsigned)d->x * (unsigned)d->y);
}

/* SPU 0x4180: phase 0xFFFF -> 0xFFFF; next == total -> 0xFFFE; else next+1. */
uint16_t postfx_claim(postfx_state s, postfx_state* out)
{
    *out = s;
    if (s.phase == POSTFX_PHASE_DONE) return 0xFFFFu;
    if (s.next == s.total) return 0xFFFEu;
    out->next = (uint16_t)(s.next + 1u);
    return s.next;
}

/* SPU 0x3F84..0x4014. The halfword adds wrap like ahi does. */
postfx_completion postfx_complete(postfx_state s, uint16_t phase_count, uint16_t next_items)
{
    postfx_completion c;
    c.next = s;
    c.advanced = c.finished = 0;
    c.next.done = (uint16_t)(s.done + 1u);
    if (c.next.done != s.total) return c;
    c.advanced = 1;
    c.next.phase = (uint16_t)(s.phase + 1u);
    if (c.next.phase == phase_count) {
        c.next.phase = POSTFX_PHASE_DONE;
        c.finished = 1;
        return c;
    }
    c.next.next = 0;
    c.next.done = 0;
    c.next.total = next_items;
    return c;
}

static int read_desc(const postfx_guest* g, uint32_t desc_ea, uint16_t index, postfx_desc* d)
{
    uint8_t raw[128];
    if (!g->read(g->user, desc_ea + 128u * index, raw, sizeof raw)) return 0;
    postfx_desc_parse(raw, d);
    return 1;
}

int postfx_post_validate(const postfx_guest* g, const postfx_post_args* a, postfx_header* h)
{
    uint8_t raw[16];
    if (!a->header_ea || (a->header_ea & 7u) || !g->read(g->user, a->header_ea, raw, sizeof raw))
        return POSTFX_RUN_BAD_HEADER;
    postfx_header_parse(raw, h);
    if (!h->phase_count || h->phase_count > POSTFX_MAX_PHASES || (h->desc_ea & 15u))
        return POSTFX_RUN_BAD_HEADER;
    if (a->counter_ea & 3u) return POSTFX_RUN_BAD_HEADER;      /* 0x5DA0 halts */
    if (a->flag_ea & 127u) return POSTFX_RUN_BAD_HEADER;       /* 0x5458 refuses */
    for (uint16_t i = 0; i < h->phase_count; ++i) {
        postfx_desc d;
        if (!read_desc(g, h->desc_ea, i, &d)) return POSTFX_RUN_BAD_DESC;
        /* No items: the SPU tasks would spin on 0xFFFE for ever. */
        if (!postfx_desc_items(&d)) return POSTFX_RUN_BAD_DESC;
        /* flags bit 1: the SPU stops (0x3C20 stop 0x3FFF) before the plugin. */
        if (d.flags & 2u) return POSTFX_RUN_BAD_DESC;
        /* The notify PUT halts on an EA that is not 16-byte aligned. */
        if ((d.flags & 1u) && (!d.notify_ea || (d.notify_ea & 15u))) return POSTFX_RUN_BAD_DESC;
    }
    return 0;
}

int postfx_post_run(const postfx_guest* g, const postfx_post_args* a, postfx_run_stats* st)
{
    postfx_header h;
    const int bad = postfx_post_validate(g, a, &h);
    if (bad) return bad;
    postfx_run_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof *st);
    unsigned spins = 0;
    int last_begun = -1;
    for (;;) {
        /* claim */
        const uint64_t raw = g->load64(g->user, a->header_ea);
        const postfx_state s = postfx_state_unpack(raw);
        postfx_state claimed;
        const uint16_t item = postfx_claim(s, &claimed);
        if (item == 0xFFFFu) return POSTFX_RUN_DONE;
        if (item == 0xFFFEu) {
            if (!g->yield || !g->yield(g->user, ++spins)) return POSTFX_RUN_STALLED;
            continue;
        }
        if (s.phase >= h.phase_count) return POSTFX_RUN_BAD_HEADER;
        if (!g->cas64(g->user, a->header_ea, raw, postfx_state_pack(claimed))) continue;
        spins = 0;
        ++st->claimed;
        postfx_desc d;
        if (!read_desc(g, h.desc_ea, s.phase, &d)) return POSTFX_RUN_BAD_DESC;
        if (g->phase_begin && last_begun != (int)s.phase) {
            last_begun = s.phase;
            g->phase_begin(g->user, s.phase, &d);
        }
        /* complete */
        postfx_completion c;
        for (;;) {
            const uint64_t raw2 = g->load64(g->user, a->header_ea);
            const postfx_state s2 = postfx_state_unpack(raw2);
            uint16_t next_items = 0;
            if ((uint16_t)(s2.done + 1u) == s2.total && (uint16_t)(s2.phase + 1u) != h.phase_count) {
                postfx_desc dn;
                if ((uint16_t)(s2.phase + 1u) >= h.phase_count ||
                    !read_desc(g, h.desc_ea, (uint16_t)(s2.phase + 1u), &dn))
                    return POSTFX_RUN_BAD_DESC;
                next_items = postfx_desc_items(&dn);
            }
            c = postfx_complete(s2, h.phase_count, next_items);
            if (g->cas64(g->user, a->header_ea, raw2, postfx_state_pack(c.next))) break;
        }
        ++st->completed;
        if (c.advanced) ++st->phases_advanced;
        /* 0x401C: the notify PUT of the completed phase, then (all phases
         * done) the event flag through callback 0x30D8. */
        if (c.advanced && (d.flags & 1u)) {
            g->put32(g->user, d.notify_ea, d.notify_value);
            ++st->notifies;
        }
        if (c.finished && a->flag_ea) {
            g->ef_set(g->user, a->flag_ea, POSTFX_EF_BITS);
            ++st->ef_sets;
        }
    }
}

int postfx_post_expect_compute(const postfx_guest* g, const postfx_post_args* a, postfx_post_expect* e)
{
    memset(e, 0, sizeof *e);
    postfx_header h;
    const int bad = postfx_post_validate(g, a, &h);
    if (bad) return bad;
    /* The frame as kicked by the PPU (0x84D49C) is {0, 0, 0, items of phase
     * 0}. Within a phase only the last completion changes more than the two
     * counters, so walk it phase by phase: every item claimed, all but one
     * completed, then that last completion. (The offline test checks this
     * against item-by-item runs of the SPU code.) */
    postfx_desc d, dn;
    if (!read_desc(g, h.desc_ea, 0, &d)) return POSTFX_RUN_BAD_DESC;
    for (uint16_t p = 0; p < h.phase_count; ++p) {
        const uint16_t items = postfx_desc_items(&d);
        if (!items) return POSTFX_RUN_STALLED;
        if (d.code_ea == POSTFX_F10_CODE_EA) {
            e->has_f10 = 1; e->f10_phase = p;
            e->f10_param_ea = d.param_ea; e->f10_param_size = d.param_size;
        } else if (d.code_ea == POSTFX_F11_CODE_EA) {
            e->has_f11 = 1; e->f11_phase = p;
            e->f11_param_ea = d.param_ea; e->f11_param_size = d.param_size;
        }
        uint16_t next_items = 0;
        if ((uint16_t)(p + 1u) != h.phase_count) {
            if (!read_desc(g, h.desc_ea, (uint16_t)(p + 1u), &dn)) return POSTFX_RUN_BAD_DESC;
            next_items = postfx_desc_items(&dn);
        }
        const postfx_state last = { p, items, (uint16_t)(items - 1u), items };
        const postfx_completion c = postfx_complete(last, h.phase_count, next_items);
        if (!c.advanced) return POSTFX_RUN_STALLED;
        if ((d.flags & 1u) && e->label_count < POSTFX_MAX_PHASES) {
            e->label_ea[e->label_count] = d.notify_ea;
            e->label_value[e->label_count] = d.notify_value;
            ++e->label_count;
        }
        if (c.finished) { e->final_state = c.next; return 0; }
        d = dn;
    }
    return POSTFX_RUN_STALLED;
}

unsigned postfx_mlaa_tail(const postfx_guest* g, const postfx_mlaa_block* b, uint32_t flag_ea)
{
    unsigned did = 0;
    /* 0x340C: PUT the 4-byte label value (0x36B0 halts on EA & 3). */
    if (b->label_ea) { g->put32(g->user, b->label_ea, b->label_value); did |= 1u; }
    /* 0x3418: no counter -> straight back to the signal wait. */
    if (!b->counter_ea) return did;
    g->inc32(g->user, b->counter_ea);
    did |= 2u;
    if (flag_ea) { g->ef_set(g->user, flag_ea, POSTFX_EF_BITS); did |= 4u; }
    return did;
}
