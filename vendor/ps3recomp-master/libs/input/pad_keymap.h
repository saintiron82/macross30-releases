/* Keyboard -> pad key mapping shared by cellPad (reads it) and the options
 * window (edits it). Stored in keymap.ini in the working directory, one
 * "name=0xVK" line per action (0 = unbound); missing names keep their
 * default. cellPad re-reads the file when it changes, so a new binding
 * applies while the game runs.
 *
 * Left stick: when none of ls_* is bound it mirrors the d-pad (the previous
 * fixed behaviour), so titles that steer with the stick stay playable. */
#ifndef PS3RECOMP_PAD_KEYMAP_H
#define PS3RECOMP_PAD_KEYMAP_H
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAD_KM_COUNT = 25 };
typedef struct {
    const char* name;       /* keymap.ini key */
    const wchar_t* label;   /* options window */
    unsigned short btn;     /* CELL_PAD_CTRL_* bit, 0 for a stick direction */
    unsigned char axis;     /* 0 button, 1 lx, 2 ly, 3 rx, 4 ry */
    signed char dir;        /* -1 toward 0, +1 toward 255 */
    int def;                /* default virtual-key, 0 = unbound */
} pad_km_action;

static const pad_km_action k_pad_km[PAD_KM_COUNT] = {
    { "up",       L"방향키 위",        1u << 4,  0,  0, VK_UP },
    { "down",     L"방향키 아래",      1u << 6,  0,  0, VK_DOWN },
    { "left",     L"방향키 왼쪽",      1u << 7,  0,  0, VK_LEFT },
    { "right",    L"방향키 오른쪽",    1u << 5,  0,  0, VK_RIGHT },
    { "cross",    L"✕ (엑스)",         1u << 14, 0,  0, 'Z' },
    { "circle",   L"○ (동그라미)",     1u << 13, 0,  0, 'X' },
    { "square",   L"□ (네모)",         1u << 15, 0,  0, 'A' },
    { "triangle", L"△ (세모)",         1u << 12, 0,  0, 'S' },
    { "l1",       L"L1",               1u << 10, 0,  0, 'Q' },
    { "r1",       L"R1",               1u << 11, 0,  0, 'W' },
    { "l2",       L"L2",               1u << 8,  0,  0, '1' },
    { "r2",       L"R2",               1u << 9,  0,  0, '2' },
    { "l3",       L"L3 (왼쪽 스틱 누름)", 1u << 1, 0, 0, 'C' },
    { "r3",       L"R3 (오른쪽 스틱 누름)", 1u << 2, 0, 0, 'V' },
    { "start",    L"START",            1u << 3,  0,  0, VK_RETURN },
    { "start2",   L"START (보조)",     1u << 3,  0,  0, VK_ESCAPE },
    { "select",   L"SELECT",           1u << 0,  0,  0, VK_TAB },
    { "ls_up",    L"왼쪽 스틱 위",     0, 2, -1, 0 },
    { "ls_down",  L"왼쪽 스틱 아래",   0, 2,  1, 0 },
    { "ls_left",  L"왼쪽 스틱 왼쪽",   0, 1, -1, 0 },
    { "ls_right", L"왼쪽 스틱 오른쪽", 0, 1,  1, 0 },
    { "rs_up",    L"오른쪽 스틱 위 (카메라)",     0, 4, -1, 'I' },
    { "rs_down",  L"오른쪽 스틱 아래 (카메라)",   0, 4,  1, 'K' },
    { "rs_left",  L"오른쪽 스틱 왼쪽 (카메라)",   0, 3, -1, 'J' },
    { "rs_right", L"오른쪽 스틱 오른쪽 (카메라)", 0, 3,  1, 'L' },
};

static void pad_km_defaults(int vk[PAD_KM_COUNT])
{
    for (int i = 0; i < PAD_KM_COUNT; i++) vk[i] = k_pad_km[i].def;
}
static void pad_km_load(int vk[PAD_KM_COUNT])
{
    pad_km_defaults(vk);
    FILE* f = fopen("keymap.ini", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[64]; unsigned v;
        if (sscanf(line, " %63[^=]=%x", key, &v) != 2) continue;
        for (int i = 0; i < PAD_KM_COUNT; i++)
            if (!strcmp(key, k_pad_km[i].name)) vk[i] = (int)(v & 0xFFu);
    }
    fclose(f);
}
static void pad_km_save(const int vk[PAD_KM_COUNT])
{
    FILE* f = fopen("keymap.ini", "w");
    if (!f) return;
    for (int i = 0; i < PAD_KM_COUNT; i++) fprintf(f, "%s=0x%02X\n", k_pad_km[i].name, (unsigned)vk[i]);
    fclose(f);
}
#endif
#endif
