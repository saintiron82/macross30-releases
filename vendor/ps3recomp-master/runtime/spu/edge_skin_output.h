/* Private PUT payloads for skinning shadow checks. Never read a shared ring
 * after another job may have reused it; incomplete evidence cannot earn trust. */
#ifndef EDGE_SKIN_OUTPUT_H
#define EDGE_SKIN_OUTPUT_H
#include <stdint.h>
#include <string.h>
#define ES_OUTPUT_RECORDS 256u
#define ES_OUTPUT_BYTES (512u << 10)
typedef struct {
    uint32_t n, bytes; int overflow;
    struct { uint32_t ea, size, off; } rec[ES_OUTPUT_RECORDS];
    uint8_t data[ES_OUTPUT_BYTES];
} es_output_log;
static inline void es_output_reset(es_output_log* r)
{
    r->n = r->bytes = 0; r->overflow = 0;
}
static inline void es_output_put(es_output_log* r, uint32_t ea, const void* data, uint32_t size)
{
    if (!size || r->overflow) return;
    if (!data || (uint64_t)ea + size > UINT64_C(0x100000000) || r->n == ES_OUTPUT_RECORDS ||
        size > ES_OUTPUT_BYTES - r->bytes) { r->overflow = 1; return; }
    r->rec[r->n].ea = ea; r->rec[r->n].size = size; r->rec[r->n].off = r->bytes;
    memcpy(r->data + r->bytes, data, size); r->bytes += size; r->n++;
}
/* Replay overlapping PUTs with last-write-wins semantics. The first pass
 * verifies complete coverage; gaps/overflow never expose partial evidence. */
static inline int es_output_read(const es_output_log* r, uint32_t ea, uint8_t* out, uint32_t size)
{
    const uint64_t end = (uint64_t)ea + size;
    if (!r || r->overflow || !out || !size || end > UINT64_C(0x100000000)) return 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t at = ea;
        while (at < end) {
            int last = -1; uint64_t next = end;
            for (uint32_t i = 0; i < r->n; i++) {
                const uint64_t begin = r->rec[i].ea, stop = begin + r->rec[i].size;
                if (begin <= at && at < stop) last = (int)i;
                if (begin > at && begin < next) next = begin;
            }
            if (last < 0) return 0;
            const uint64_t stop = (uint64_t)r->rec[last].ea + r->rec[last].size;
            if (stop < next) next = stop;
            if (pass) memcpy(out + (size_t)(at - ea),
                r->data + r->rec[last].off + (size_t)(at - r->rec[last].ea), (size_t)(next - at));
            at = next;
        }
    }
    return 1;
}
#endif
