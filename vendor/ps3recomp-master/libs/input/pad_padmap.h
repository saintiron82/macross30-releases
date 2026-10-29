/* Controller button -> PS3 button mapping shared by cellPad (applies it) and
 * the options window (edits it). Every physical button of a host pad (SDL2
 * GameController names, positional: A = bottom ...) is given one PS3 button
 * or none; several may give the same one. It applies to every pad. Sticks
 * are not remapped.
 *
 * Stored in padmap.ini in the working directory, one "source=action" line
 * per physical button; missing lines keep their default. cellPad re-reads
 * the file when it changes, so a change applies while the game runs.
 *
 * L2/R2 analog values: a trigger mapped to L2 or R2 passes its analog value;
 * any other button mapped there gives 255 while held. A trigger mapped to a
 * digital PS3 button presses it past PAD_PM_TRIGGER_THRESHOLD. */
#ifndef PS3RECOMP_PAD_PADMAP_H
#define PS3RECOMP_PAD_PADMAP_H
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <wchar.h>
#define PAD_PM_L(s) L##s
typedef const wchar_t* pad_pm_label;
#else
#define PAD_PM_L(s) s
typedef const char* pad_pm_label;
#endif

#define PAD_PM_TRIGGER_THRESHOLD 30   /* 0..255, as XInput's */

/* Physical buttons (bit i of a pad's raw mask). */
enum {
    PAD_SRC_A, PAD_SRC_B, PAD_SRC_X, PAD_SRC_Y,
    PAD_SRC_BACK, PAD_SRC_START, PAD_SRC_LS, PAD_SRC_RS, PAD_SRC_LB, PAD_SRC_RB,
    PAD_SRC_UP, PAD_SRC_DOWN, PAD_SRC_LEFT, PAD_SRC_RIGHT,
    PAD_SRC_LT, PAD_SRC_RT, PAD_SRC_TOUCHPAD, PAD_SRC_MISC1,
    PAD_SRC_PADDLE1, PAD_SRC_PADDLE2, PAD_SRC_PADDLE3, PAD_SRC_PADDLE4,
    PAD_SRC_COUNT
};
/* PS3 buttons a physical one can give (index 0 = none). */
enum {
    PAD_ACT_NONE, PAD_ACT_CROSS, PAD_ACT_CIRCLE, PAD_ACT_SQUARE, PAD_ACT_TRIANGLE,
    PAD_ACT_L1, PAD_ACT_R1, PAD_ACT_L2, PAD_ACT_R2, PAD_ACT_L3, PAD_ACT_R3,
    PAD_ACT_SELECT, PAD_ACT_START, PAD_ACT_UP, PAD_ACT_DOWN, PAD_ACT_LEFT, PAD_ACT_RIGHT,
    PAD_ACT_COUNT
};

typedef struct {
    const char* name;      /* padmap.ini key */
    pad_pm_label label;    /* options window: Xbox name / PlayStation name */
    int def;               /* default PAD_ACT_* */
} pad_pm_source;

static const pad_pm_source k_pad_pm_src[PAD_SRC_COUNT] = {
    { "a",        PAD_PM_L("A  ·  ✕ (아래)"),              PAD_ACT_CROSS },
    { "b",        PAD_PM_L("B  ·  ○ (오른쪽)"),            PAD_ACT_CIRCLE },
    { "x",        PAD_PM_L("X  ·  □ (왼쪽)"),              PAD_ACT_SQUARE },
    { "y",        PAD_PM_L("Y  ·  △ (위)"),                PAD_ACT_TRIANGLE },
    { "back",     PAD_PM_L("View  ·  Create"),             PAD_ACT_SELECT },
    { "start",    PAD_PM_L("Menu  ·  Options"),            PAD_ACT_START },
    { "ls",       PAD_PM_L("왼쪽 스틱 누름"),              PAD_ACT_L3 },
    { "rs",       PAD_PM_L("오른쪽 스틱 누름"),            PAD_ACT_R3 },
    { "lb",       PAD_PM_L("LB  ·  L1"),                   PAD_ACT_L1 },
    { "rb",       PAD_PM_L("RB  ·  R1"),                   PAD_ACT_R1 },
    { "up",       PAD_PM_L("방향 패드 위"),                PAD_ACT_UP },
    { "down",     PAD_PM_L("방향 패드 아래"),              PAD_ACT_DOWN },
    { "left",     PAD_PM_L("방향 패드 왼쪽"),              PAD_ACT_LEFT },
    { "right",    PAD_PM_L("방향 패드 오른쪽"),            PAD_ACT_RIGHT },
    { "lt",       PAD_PM_L("LT  ·  L2 (트리거)"),          PAD_ACT_L2 },
    { "rt",       PAD_PM_L("RT  ·  R2 (트리거)"),          PAD_ACT_R2 },
    { "touchpad", PAD_PM_L("터치패드 클릭 (PS4/PS5)"),     PAD_ACT_SELECT },
    { "misc1",    PAD_PM_L("공유·캡처·마이크 버튼"),       PAD_ACT_NONE },
    { "paddle1",  PAD_PM_L("뒷면 패들 1"),                 PAD_ACT_NONE },
    { "paddle2",  PAD_PM_L("뒷면 패들 2"),                 PAD_ACT_NONE },
    { "paddle3",  PAD_PM_L("뒷면 패들 3"),                 PAD_ACT_NONE },
    { "paddle4",  PAD_PM_L("뒷면 패들 4"),                 PAD_ACT_NONE },
};

typedef struct {
    const char* name;      /* padmap.ini value */
    pad_pm_label label;
    unsigned short btn;    /* CELL_PAD_CTRL_* */
} pad_pm_action;

static const pad_pm_action k_pad_pm_act[PAD_ACT_COUNT] = {
    { "none",     PAD_PM_L("(없음)"),        0 },
    { "cross",    PAD_PM_L("✕"),             1u << 14 },
    { "circle",   PAD_PM_L("○"),             1u << 13 },
    { "square",   PAD_PM_L("□"),             1u << 15 },
    { "triangle", PAD_PM_L("△"),             1u << 12 },
    { "l1",       PAD_PM_L("L1"),            1u << 10 },
    { "r1",       PAD_PM_L("R1"),            1u << 11 },
    { "l2",       PAD_PM_L("L2"),            1u << 8 },
    { "r2",       PAD_PM_L("R2"),            1u << 9 },
    { "l3",       PAD_PM_L("L3"),            1u << 1 },
    { "r3",       PAD_PM_L("R3"),            1u << 2 },
    { "select",   PAD_PM_L("SELECT"),        1u << 0 },
    { "start",    PAD_PM_L("START"),         1u << 3 },
    { "up",       PAD_PM_L("방향키 위"),     1u << 4 },
    { "down",     PAD_PM_L("방향키 아래"),   1u << 6 },
    { "left",     PAD_PM_L("방향키 왼쪽"),   1u << 7 },
    { "right",    PAD_PM_L("방향키 오른쪽"), 1u << 5 },
};

static void pad_pm_defaults(int act[PAD_SRC_COUNT])
{
    for (int i = 0; i < PAD_SRC_COUNT; i++) act[i] = k_pad_pm_src[i].def;
}
static void pad_pm_load(int act[PAD_SRC_COUNT])
{
    pad_pm_defaults(act);
    FILE* f = fopen("padmap.ini", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[64];
        if (sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
        for (int i = 0; i < PAD_SRC_COUNT; i++) {
            if (strcmp(key, k_pad_pm_src[i].name)) continue;
            for (int a = 0; a < PAD_ACT_COUNT; a++)
                if (!strcmp(val, k_pad_pm_act[a].name)) act[i] = a;
        }
    }
    fclose(f);
}
static void pad_pm_save(const int act[PAD_SRC_COUNT])
{
    FILE* f = fopen("padmap.ini", "w");
    if (!f) return;
    for (int i = 0; i < PAD_SRC_COUNT; i++)
        fprintf(f, "%s=%s\n", k_pad_pm_src[i].name, k_pad_pm_act[act[i]].name);
    fclose(f);
}

/* Apply the mapping to one pad's raw state: raw = PAD_SRC_* bits, lt/rt the
 * analog triggers (0..255). Gives CELL_PAD_CTRL_* buttons and L2/R2 values. */
static void pad_pm_apply(const int act[PAD_SRC_COUNT], unsigned raw, unsigned char lt, unsigned char rt,
                         unsigned short* buttons, unsigned char* l2, unsigned char* r2)
{
    unsigned short b = 0;
    unsigned char v2[2] = { 0, 0 };   /* L2, R2 */
    for (int i = 0; i < PAD_SRC_COUNT; i++) {
        const int a = act[i];
        if (a <= PAD_ACT_NONE || a >= PAD_ACT_COUNT) continue;
        const int trig = i == PAD_SRC_LT || i == PAD_SRC_RT;
        const unsigned char analog = i == PAD_SRC_LT ? lt : i == PAD_SRC_RT ? rt : 0;
        const int down = trig ? analog > PAD_PM_TRIGGER_THRESHOLD : (raw >> i) & 1u;
        if (a == PAD_ACT_L2 || a == PAD_ACT_R2) {
            const unsigned char v = trig ? analog : (down ? 255 : 0);
            unsigned char* dst = &v2[a == PAD_ACT_R2];
            if (v > *dst) *dst = v;
        }
        if (down) b |= k_pad_pm_act[a].btn;
    }
    *buttons = b;
    *l2 = v2[0];
    *r2 = v2[1];
}
#endif
