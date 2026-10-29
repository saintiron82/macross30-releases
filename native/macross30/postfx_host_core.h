#ifndef MACROSS_POSTFX_HOST_CORE_H
#define MACROSS_POSTFX_HOST_CORE_H
/* Completion protocol of the two post-processing SPU tasksets, without the
 * pixel work (native/analysis/postfx-task-protocol.md has the derivation).
 *
 * posttask (image 318051041, taskset 0x119A5E00, 5 tasks). Task argument:
 *   w0 = 16-byte frame header (the shared work state)   [B+0x1980]
 *   w1 = u32 wake counter, reset by the PPU at each kick [0x00ADAC70]
 *   w2 = CellSpursEventFlag the last completer sets (bit 1) [B+0x80]
 * Header (big-endian):
 *   +0 u16 phase  +2 u16 next item  +4 u16 items done  +6 u16 items in phase
 *   +8 u16 phase count  +C u32 EA of the descriptor array (128 bytes each)
 * Descriptor: +0 u8 x count, +1 u8 y count (items = x*y), +3 u8 flags
 *   (bit 0: notify on phase completion), +4 u32 notify EA, +8 u32 notify value,
 *   +C plugin code EA, +10/+14 code size, +18 param EA, +1C param size,
 *   +40.. surface descriptors.
 * Claim (SPU 0x4180) and complete (SPU 0x3DE8) are GETLLAR/PUTLLC loops on the
 * header's first 8 bytes; complete advances the phase and, on the last one,
 * stores phase 0xFFFF and sets the event flag. Every woken task finally adds 1
 * to the wake counter (SPU 0x5DA0) before it waits for the next signal.
 *
 * Edge MLAA (image 452987026, taskset 0x119ADA00, 5 tasks). Task argument:
 *   w0 = this task's 64-byte block, w1 = the same event flag.
 * Block (big-endian): +0 label value, +4 label EA, +8 completion counter EA,
 *   +C/+10 surfaces, +14 barrier EA, ... +22 u8 mode flags. Only block 0 has
 *   +4/+8 set (PPU 0x84C948). After the last barrier the task PUTs the label,
 *   adds 1 to the counter and sets event flag bit 1. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    POSTFX_POST_ELF_EA = 0x00A37D80u,   /* TaskInfo ELF of image 318051041 */
    POSTFX_MLAA_ELF_EA = 0x00994680u,   /* TaskInfo ELF of image 452987026 */
    POSTFX_F10_CODE_EA = 0x00A68D00u,   /* posttask plugin family 10 (tone LUT) */
    POSTFX_F11_CODE_EA = 0x00A68100u,   /* posttask plugin family 11 (composite) */
    POSTFX_PHASE_DONE  = 0xFFFFu,
    POSTFX_MAX_PHASES  = 256u,
    POSTFX_EF_BITS     = 1u,
};

typedef struct postfx_state { uint16_t phase, next, done, total; } postfx_state;

typedef struct postfx_header {
    postfx_state state;
    uint16_t phase_count, pad;
    uint32_t desc_ea;
} postfx_header;

typedef struct postfx_desc {
    uint8_t x, y, flags;
    uint32_t notify_ea, notify_value, code_ea, code_size, dma_size, param_ea, param_size;
} postfx_desc;

typedef struct postfx_mlaa_block {
    uint32_t label_value, label_ea, counter_ea, barrier_ea;
    uint8_t mode;
} postfx_mlaa_block;

/* Big-endian decoding of guest bytes. */
uint64_t postfx_state_pack(postfx_state s);
postfx_state postfx_state_unpack(uint64_t v);
void postfx_header_parse(const uint8_t raw[16], postfx_header* h);
void postfx_desc_parse(const uint8_t raw[128], postfx_desc* d);
void postfx_mlaa_block_parse(const uint8_t raw[64], postfx_mlaa_block* b);
/* Items of a phase: the SPU's mpy of the two count bytes, kept as a u16. */
uint16_t postfx_desc_items(const postfx_desc* d);

/* Pure transition functions of the two atomic sections. claim: returns the
 * claimed item or 0xFFFF (frame done) / 0xFFFE (every item handed out, some
 * not completed yet); *out is the state to store (equal to s when nothing is
 * claimed). complete: H2+1; on the last item of a phase the phase advances
 * (next_items = items of the following phase, ignored when it was the last). */
uint16_t postfx_claim(postfx_state s, postfx_state* out);
typedef struct postfx_completion { postfx_state next; int advanced, finished; } postfx_completion;
postfx_completion postfx_complete(postfx_state s, uint16_t phase_count, uint16_t next_items);

/* Guest access for the virtual task. All callbacks are required except
 * phase_begin and yield. */
typedef struct postfx_guest {
    void* user;
    int (*read)(void* user, uint32_t ea, void* dst, uint32_t n);       /* 0 = bad range */
    uint64_t (*load64)(void* user, uint32_t ea);                       /* BE-decoded */
    int (*cas64)(void* user, uint32_t ea, uint64_t expect, uint64_t desired);  /* 1 = stored */
    void (*put32)(void* user, uint32_t ea, uint32_t value);            /* SPU 4-byte PUT */
    uint32_t (*inc32)(void* user, uint32_t ea);                        /* atomic +1, old */
    void (*ef_set)(void* user, uint32_t ea, uint16_t bits);
    void (*phase_begin)(void* user, uint16_t phase, const postfx_desc* d);
    int (*yield)(void* user, unsigned spins);                          /* 0 = give up */
} postfx_guest;

typedef struct postfx_post_args { uint32_t header_ea, counter_ea, flag_ea; } postfx_post_args;
void postfx_post_args_parse(const uint8_t raw[16], postfx_post_args* a);
void postfx_mlaa_args_parse(const uint8_t raw[16], uint32_t* block_ea, uint32_t* flag_ea);

enum {
    POSTFX_RUN_DONE = 0,         /* frame finished (by this or another task) */
    POSTFX_RUN_STALLED = -1,     /* yield gave up while others held items */
    POSTFX_RUN_BAD_HEADER = -2,  /* phase count 0 or > POSTFX_MAX_PHASES */
    POSTFX_RUN_BAD_DESC = -3,    /* descriptor unreadable / zero items / bad notify EA */
};
typedef struct postfx_run_stats { unsigned claimed, completed, phases_advanced, notifies, ef_sets; } postfx_run_stats;
/* One virtual posttask task: claim and complete until phase 0xFFFF, with the
 * notify PUTs and the final event-flag set exactly where the SPU does them.
 * No wake-counter increment (the caller does one per suppressed signal). */
int postfx_post_run(const postfx_guest* g, const postfx_post_args* a, postfx_run_stats* st);
/* Header and descriptors are validated before anything is written:
 * returns 0 or a POSTFX_RUN_* error. */
int postfx_post_validate(const postfx_guest* g, const postfx_post_args* a, postfx_header* h);

/* What a whole frame writes when it runs to the end from the header as
 * kicked (state {0,0,0,items of phase 0}). */
typedef struct postfx_post_expect {
    postfx_state final_state;
    unsigned label_count;
    uint32_t label_ea[POSTFX_MAX_PHASES], label_value[POSTFX_MAX_PHASES];  /* phase order */
    int has_f10, has_f11;
    uint16_t f10_phase, f11_phase;
    uint32_t f10_param_ea, f10_param_size, f11_param_ea, f11_param_size;
} postfx_post_expect;
int postfx_post_expect_compute(const postfx_guest* g, const postfx_post_args* a, postfx_post_expect* e);

/* One MLAA task's tail after its last barrier: label PUT, counter +1, event
 * flag. Returns a bit mask of what it did (1 label, 2 counter, 4 flag). */
unsigned postfx_mlaa_tail(const postfx_guest* g, const postfx_mlaa_block* b, uint32_t flag_ea);

#ifdef __cplusplus
}
#endif
#endif
