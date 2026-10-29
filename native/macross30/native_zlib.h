#ifndef MACROSS_NATIVE_ZLIB_H
#define MACROSS_NATIVE_ZLIB_H
/* Host replacement for the Edge zlib inflate taskset (SPU image 433707155,
 * "PS3_Release/edgezlib_inflate_tas", queue consumer tasks of the LFQueue the
 * stubs.cpp bridge publishes into).
 *
 * The element is the 32-byte EdgeZlibInflateQTaskElement as the task reads it
 * (lifted main loop 0x30D8, popped into LS sp+0x30):
 *   +0x00 u32 input EA            +0x04 u32 output EA
 *   +0x08 u32 input size          +0x0C u32 bytes written to the output EA
 *   +0x10 u32 work-to-do counter EA | bit 0: 1 = raw deflate, 0 = stored
 *   +0x14 u32 event flag EA       +0x18 u16 event flag bits
 *   +0x1A u16 skip begin          +0x1C u16 skip end    +0x1E pad
 * The stream decodes to skip_begin + output_size + skip_end bytes; only the
 * middle output_size bytes are written (0x33D0). */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct macross_zlib_element {
    uint32_t ea_input, ea_output, input_size, output_size;
    uint32_t ea_counter;          /* word 4 with bit 0 cleared */
    uint32_t ea_event_flag;
    uint16_t event_bits, skip_begin, skip_end;
    int deflate;                  /* word 4 bit 0 */
} macross_zlib_element;

/* Pure decode of one element on host buffers, the part the unit test covers. */
void macross_zlib_element_parse(const uint8_t raw[32], macross_zlib_element* e);
/* Bytes the stream decodes to; 0 when the sum wraps (the SPU's 0x3340 error). */
uint64_t macross_zlib_element_total(const macross_zlib_element* e);
/* Raw deflate (no zlib header) into exactly out_cap bytes of room. Returns the
 * decoded length, or -1 on a corrupt stream / output overflow. */
int macross_raw_inflate(uint8_t* out, uint32_t out_cap, const uint8_t* in, uint32_t in_size);
/* Produce what the SPU writes for this element. scratch must hold
 * macross_zlib_element_total() bytes. On success returns 0 and sets *out and
 * *out_len (a pointer into input or scratch). Returns a negative code where
 * the SPU would take an error path, or the host cannot prove it would match:
 *  -1 size wrap, -2 stored size mismatch, -3 inflate failed or wrong length,
 *  -4 empty input. */
int macross_zlib_element_decode(const macross_zlib_element* e, const uint8_t* input,
                                uint8_t* scratch, const uint8_t** out, uint32_t* out_len);

/* Guest integration (native_zlib.cpp). Mode: MACROSS_NATIVE_ZLIB unset or
 * verify = SPU as before plus a native shadow decode that is compared at
 * completion, 1 = native, 0 = off. A file named no_native_zlib in the working
 * directory turns it off. */
enum { MACROSS_ZLIB_OFF = 0, MACROSS_ZLIB_NATIVE = 1, MACROSS_ZLIB_VERIFY = 2 };
int macross_native_zlib_mode(void);
/* Called from the LFQueue push bridge for a queue whose consumers run the
 * zlib task. Returns 1 when the element was taken (native mode: do not
 * publish it), 0 when the caller must publish it to the guest queue. */
int macross_native_zlib_submit(uint32_t queue, const uint8_t raw[32]);
/* Called after every successful cellSpursEventFlagWait: verify mode compares
 * the shadow decodes whose batches the SPU has finished. */
void macross_native_zlib_after_wait(uint32_t event_flag);
/* Provided by stubs.cpp: publish one element to the guest queue and wake its
 * consumer tasks (the SPU path, used for fallback). */
int macross_lf_publish(uint32_t queue, const uint8_t* element);

#ifdef __cplusplus
}
#endif
#endif
