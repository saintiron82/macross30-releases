/*
 * Controller test view (options window -> "컨트롤러 테스트"): what cellPad
 * sees from each host controller and what the title reads on port 0.
 * Implemented in cellPad.c.
 */
#ifndef PS3RECOMP_PAD_TEST_H
#define PS3RECOMP_PAD_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#define PAD_TEST_MAX_DEVICES 8

typedef struct {
    int connected;
    unsigned short buttons;           /* CELL_PAD_CTRL_* */
    unsigned char lx, ly, rx, ry;     /* 0..255, centre 128, up = 0 */
    unsigned char l2, r2;             /* 0..255 */
    unsigned raw;                     /* PAD_SRC_* physical buttons held (pad_padmap.h) */
} PadTestState;

typedef struct {
    char name[64];                    /* UTF-8 */
    char type[32];                    /* "DualSense", "Xbox One/Series", ... */
    PadTestState st;
} PadTestDevice;

typedef struct {
    int backend;                      /* 0 = cellPad not started, 1 = XInput, 2 = SDL2 */
    char sdl_version[16];             /* "2.32.10" with SDL2 */
    int ndev;
    PadTestDevice dev[PAD_TEST_MAX_DEVICES];
    int keyboard;                     /* a mapped key is down */
    int blocked;                      /* the title gets neutral input */
    PadTestState port0;               /* what the title reads: pads + keyboard */
} PadTestInfo;

/* Fill *out (polls the host if due). Safe from any thread. */
void cellPad_test_info(PadTestInfo* out);
/* While on, the title reads released buttons and centred sticks, so pressing
 * buttons in the test window does not also play the game. Counted: each
 * on needs its own off. */
void cellPad_test_block(int on);
/* Run both motors on every pad (on = 1) or restore the title's rumble (0). */
void cellPad_test_rumble(int on);

#ifdef __cplusplus
}
#endif

#endif
