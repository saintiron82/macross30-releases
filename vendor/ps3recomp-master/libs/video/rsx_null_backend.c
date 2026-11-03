/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Implements rsx_backend callbacks using a simple Win32 GDI window.
 * No GPU rendering — just clears to the RSX clear color and presents.
 * Used for debugging command buffer flow before a real backend exists.
 */

#include "rsx_null_backend.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "../../runtime/ps3_log.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include "macross_ui_language.h"
#include "../misc/custom_music.h"

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    HWND     hwnd;
    HDC      hdc;
    u32      width;
    u32      height;

    /* Current clear color (ARGB -> COLORREF) */
    COLORREF clear_color;

    /* Frame counter */
    u64      frame_count;
    u64      last_fps_time;
    u32      fps;

    int      window_closed;

    /* Title-bar stats: base caption, and frames/time at the last refresh. */
    char     title[128];
    u64      title_frames;
    ULONGLONG title_time;
} NullBackendState;

static NullBackendState s_state;

/* ---------------------------------------------------------------------------
 * Win32 window procedure
 * -----------------------------------------------------------------------*/

static int s_present_suppressed = 0;

/* ---------------------------------------------------------------------------
 * Options window (F9) and borderless fullscreen (F11 / Alt+Enter).
 * Settings live in settings.ini in the working directory. Fullscreen, vsync
 * bloom, subtitles and the frame-rate unlock apply at once. Resolution is
 * read by the launcher and applies from the next start.
 * -----------------------------------------------------------------------*/
typedef struct { int scale_idx, fullscreen, vsync, fps_unlock, bloom, subtitles, language, hd_textures; } NullSettings;
static NullSettings s_set = { 2, 0, 1, 0, 1, 1, 1, 0 };   /* HD textures opt in; apply on next start. */
static volatile LONG s_hd_textures_at_start;
int rsx_null_backend_hd_textures_enabled(void)
{
    return (int)InterlockedCompareExchange(&s_hd_textures_at_start, 0, 0);
}
/* One atomic word: bit0 enabled, upper31 bits change epoch. The movie
 * consumer never reads UI-thread-owned s_set. Display permission alone does
 * not authorize loading unreviewed subtitle tracks. */
static volatile LONG s_subtitle_state = 1;
static void null_subtitles_publish(int enabled)
{
    LONG before = InterlockedCompareExchange(&s_subtitle_state, 0, 0);
    for (;;) {
        if (((u32)before & 1u) == (u32)(enabled != 0)) return;
        const LONG after = (LONG)((((u32)before + 2u) & ~1u) | (u32)(enabled != 0));
        const LONG actual = InterlockedCompareExchange(&s_subtitle_state, after, before);
        if (actual == before) return;
        before = actual;
    }
}
int rsx_null_backend_subtitles_enabled(u32* epoch)
{
    const u32 state = (u32)InterlockedCompareExchange(&s_subtitle_state, 0, 0);
    if (epoch) *epoch = state >> 1;
    return (int)(state & 1u);
}
int rsx_null_backend_movie_subtitles_enabled(u32* epoch)
{
    return rsx_null_backend_subtitles_enabled(epoch);
}
static const char* const k_scale_val[6] = { "1", "1.25", "1.5", "2", "2.5", "3" };
static const char* const k_language_val[3] = { "ja-JP", "ko-KR", "en-US" };
static HWND s_opt_hwnd;
static int (*s_fps_apply)(int);
void rsx_null_backend_set_fps_callback(int (*apply)(int)) { s_fps_apply = apply; }
static HMENU s_menu_bar;
static RECT s_windowed_rect;
static int s_is_fullscreen;
void rsx_live_draw_set_vsync(int on);

static void null_settings_load(void)
{
    const char* gate = getenv("MACROSS_FRAME_VBLANKS");
    if (gate) s_set.fps_unlock = !strcmp(gate, "1");
    FILE* f = fopen("settings.ini", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[64]; char val[64];
        if (sscanf(line, " %63[^=]=%63s", key, val) != 2) continue;
        if (!strcmp(key, "render_scale")) {
            for (int i = 0; i < 6; i++) if (!strcmp(val, k_scale_val[i])) s_set.scale_idx = i;
        } else if (!strcmp(key, "language")) {
            for (int i = 0; i < 3; i++) if (!strcmp(val, k_language_val[i])) s_set.language = i;
        } else if (!strcmp(key, "fullscreen")) s_set.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "vsync")) s_set.vsync = atoi(val) != 0;
        else if (!strcmp(key, "fps_unlock")) s_set.fps_unlock = atoi(val) != 0;
        else if (!strcmp(key, "subtitles") && (!strcmp(val, "0") || !strcmp(val, "1"))) s_set.subtitles = val[0] == '1';
        else if (!strcmp(key, "hd_textures") && (!strcmp(val, "0") || !strcmp(val, "1"))) s_set.hd_textures = val[0] == '1';
    }
    fclose(f);
    null_subtitles_publish(s_set.subtitles);
    InterlockedExchange(&s_hd_textures_at_start, s_set.hd_textures);
}
static void null_settings_save(void)
{
    FILE* f = fopen("settings.ini", "w");
    if (!f) return;
    fprintf(f, "render_scale=%s\nfullscreen=%d\nvsync=%d\nfps_unlock=%d\nbloom=%d\nsubtitles=%d\nlanguage=%s\nhd_textures=%d\n",
            k_scale_val[s_set.scale_idx], s_set.fullscreen, s_set.vsync, s_set.fps_unlock, s_set.bloom, s_set.subtitles, k_language_val[s_set.language], s_set.hd_textures);
    fclose(f);
}
static void null_apply_bloom(void)
{
    /* Bloom is part of the default image, not a user option. */
    s_set.bloom = 1;
    DeleteFileA("pp_nobloom");
}
static void null_apply_fullscreen(HWND hwnd, int on)
{
    if (!hwnd || on == s_is_fullscreen) return;
    if (on) {
        GetWindowRect(hwnd, &s_windowed_rect);
        MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(hwnd, NULL);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (s_menu_bar) SetMenu(hwnd, s_menu_bar);
        SetWindowPos(hwnd, NULL, s_windowed_rect.left, s_windowed_rect.top,
                     s_windowed_rect.right - s_windowed_rect.left, s_windowed_rect.bottom - s_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    s_is_fullscreen = on;
}

/* ---------------------------------------------------------------------------
 * Key settings window (from the options window): keyboard -> pad bindings in
 * keymap.ini (libs/input/pad_keymap.h). Click a row, press a key: Esc
 * cancels, Delete unbinds. cellPad re-reads the file, so it applies at once.
 * The class name starts with "ps3recomp_opt", which cellPad treats as "not
 * the game": keys pressed here do not reach the title.
 * -----------------------------------------------------------------------*/
#include "../input/pad_keymap.h"
enum { ID_KEY_BASE = 300, ID_KEY_DEFAULTS = 290, ID_KEY_CLOSE = 291, ID_KEY_TIMER = 1 };
static HWND s_key_hwnd;
static int s_key_vk[PAD_KM_COUNT];
static int s_key_capture = -1;
static unsigned char s_key_down0[256];
static void null_key_name(int vk, wchar_t* out, int cap)
{
    if (!vk) { wcsncpy(out, macross_ui_string("input.none"), cap); return; }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk) {   /* extended keys need bit 24 for the right name */
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DIVIDE: case VK_RCONTROL:
    case VK_RMENU: case VK_NUMLOCK:
        sc |= 0x100; break;
    }
    if (!sc || !GetKeyNameTextW((LONG)(sc << 16), out, cap)) _snwprintf(out, cap, macross_ui_string("input.key_code"), vk);
}
static void null_key_refresh(int i)
{
    HWND b = GetDlgItem(s_key_hwnd, ID_KEY_BASE + i);
    if (!b) return;
    wchar_t name[64];
    if (i == s_key_capture) wcsncpy(name, macross_ui_string("input.key_prompt"), 64);
    else null_key_name(s_key_vk[i], name, 64);
    SetWindowTextW(b, name);
}
static void null_key_finish(int vk)   /* vk < 0: cancel */
{
    const int i = s_key_capture;
    s_key_capture = -1;
    KillTimer(s_key_hwnd, ID_KEY_TIMER);
    if (i < 0) return;
    if (vk >= 0) {
        for (int j = 0; j < PAD_KM_COUNT; j++)   /* one key, one action */
            if (vk && j != i && s_key_vk[j] == vk) { s_key_vk[j] = 0; null_key_refresh(j); }
        s_key_vk[i] = vk;
        pad_km_save(s_key_vk);
    }
    null_key_refresh(i);
}
static LRESULT CALLBACK null_key_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= ID_KEY_BASE && id < ID_KEY_BASE + PAD_KM_COUNT) {
            if (s_key_capture >= 0) null_key_finish(-1);
            s_key_capture = id - ID_KEY_BASE;
            for (int v = 0; v < 256; v++) s_key_down0[v] = (GetAsyncKeyState(v) & 0x8000) != 0;
            SetTimer(hwnd, ID_KEY_TIMER, 20, NULL);
            null_key_refresh(s_key_capture);
        } else if (id == ID_KEY_DEFAULTS) {
            if (s_key_capture >= 0) null_key_finish(-1);
            pad_km_defaults(s_key_vk); pad_km_save(s_key_vk);
            for (int i = 0; i < PAD_KM_COUNT; i++) null_key_refresh(i);
        } else if (id == ID_KEY_CLOSE) DestroyWindow(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == ID_KEY_TIMER && s_key_capture >= 0) {
            for (int v = 8; v < 256; v++) {
                /* mouse buttons and the generic shift/ctrl/alt (left/right
                 * variants are bindable) */
                if (v == VK_SHIFT || v == VK_CONTROL || v == VK_MENU) continue;
                const int down = (GetAsyncKeyState(v) & 0x8000) != 0;
                if (down && !s_key_down0[v]) {
                    null_key_finish(v == VK_ESCAPE ? -1 : v == VK_DELETE ? 0 : v);
                    break;
                }
                s_key_down0[v] = (unsigned char)down;
            }
        }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_KEY_TIMER); s_key_capture = -1; s_key_hwnd = NULL;
        if (s_opt_hwnd) SetForegroundWindow(s_opt_hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void null_open_keys(HWND owner)
{
    if (s_key_hwnd) { SetForegroundWindow(s_key_hwnd); return; }
    static int registered;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = null_key_wndproc; wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ps3recomp_opt_keys";
        RegisterClassExW(&wc);
        registered = 1;
    }
    pad_km_load(s_key_vk);
    const int rows = (PAD_KM_COUNT + 1) / 2, rh = 30;
    const int kw = 860, kh = 40 + rows * rh + 110;
    RECT orc; GetWindowRect(owner, &orc);
    s_key_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"ps3recomp_opt_keys", macross_ui_string("input.keyboard.title"),
                                 WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                 (orc.left + orc.right - kw) / 2, (orc.top + orc.bottom - kh) / 2, kw, kh,
                                 owner, NULL, hi, NULL);
    if (!s_key_hwnd) return;
    static HFONT font;
    if (!font) font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    HWND c;
#define KEY_CTL(cls, text, style, x, y, w, h, id) \
    c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (style), x, y, w, h, s_key_hwnd, (HMENU)(INT_PTR)(id), hi, NULL); \
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    for (int i = 0; i < PAD_KM_COUNT; i++) {
        const int col = i / rows, row = i % rows;
        const int x = 16 + col * 420, y = 14 + row * rh;
        KEY_CTL(L"STATIC", k_pad_km[i].label, SS_CENTERIMAGE, x, y, 210, 26, 0)
        KEY_CTL(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, x + 214, y, 190, 26, ID_KEY_BASE + i)
        null_key_refresh(i);
    }
    const int by = 20 + rows * rh;
    KEY_CTL(L"STATIC", macross_ui_string("input.keyboard.help"),
            0, 16, by, 820, 44, 0)
    KEY_CTL(L"BUTTON", macross_ui_string("common.defaults"), BS_PUSHBUTTON | WS_TABSTOP, 16, by + 50, 140, 30, ID_KEY_DEFAULTS)
    KEY_CTL(L"BUTTON", macross_ui_string("common.close"), BS_PUSHBUTTON | WS_TABSTOP, kw - 130, by + 50, 100, 30, ID_KEY_CLOSE)
#undef KEY_CTL
}
/* ---------------------------------------------------------------------------
 * Controller test window (from the options window): which controllers are
 * connected and recognised, and a live picture of the buttons, sticks and
 * triggers the title reads on port 0 (pads and keyboard merged). While it is
 * open the title gets neutral input (cellPad_test_block), so buttons pressed
 * here do not also play the game. Its class name does not start with
 * "ps3recomp_opt", so the keyboard is read here too.
 * -----------------------------------------------------------------------*/
#include "../input/pad_test.h"
#include "../input/cellPad.h"
enum { ID_PT_RUMBLE = 401, ID_PT_CLOSE, ID_PT_TIMER = 2, ID_PT_RUMBLE_OFF = 3 };
static HWND s_pt_hwnd;
static HFONT s_pt_font, s_pt_small;
static PadTestInfo s_pt_info;

static void pt_text(HDC dc, const wchar_t* t, int x, int y, int w, int h, UINT fmt)
{
    RECT r = { x, y, x + w, y + h };
    DrawTextW(dc, t, -1, &r, fmt | DT_NOPREFIX);
}
/* A button: rounded box (round = ellipse), filled when pressed. */
static void pt_button(HDC dc, int x, int y, int w, int h, int round, const wchar_t* label, int on)
{
    HBRUSH b = CreateSolidBrush(on ? RGB(255, 150, 40) : RGB(232, 232, 232));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(BLACK_PEN));
    if (round) Ellipse(dc, x, y, x + w, y + h);
    else RoundRect(dc, x, y, x + w, y + h, 8, 8);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
    SetTextColor(dc, on ? RGB(255, 255, 255) : RGB(40, 40, 40));
    pt_text(dc, label, x, y, w, h, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(0, 0, 0));
}
/* A trigger: bar filled to its value, label and number. */
static void pt_trigger(HDC dc, int x, int y, int w, const wchar_t* label, int v, int on)
{
    HBRUSH bg = CreateSolidBrush(RGB(232, 232, 232)), fg = CreateSolidBrush(on ? RGB(255, 150, 40) : RGB(150, 190, 240));
    RECT r = { x, y, x + w, y + 24 };
    FillRect(dc, &r, bg);
    RECT f = { x, y, x + w * v / 255, y + 24 };
    FillRect(dc, &f, fg);
    FrameRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    DeleteObject(bg); DeleteObject(fg);
    wchar_t t[32]; _snwprintf(t, 32, L"%s  %d", label, v);
    pt_text(dc, t, x, y, w, 24, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
/* A stick: circle, dot at its position, filled when clicked (L3/R3). */
static void pt_stick(HDC dc, int cx, int cy, int rad, int sx, int sy, const wchar_t* label, int click)
{
    HBRUSH b = CreateSolidBrush(click ? RGB(255, 200, 140) : RGB(240, 240, 240));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    SelectObject(dc, ob); DeleteObject(b);
    HPEN grey = CreatePen(PS_DOT, 1, RGB(160, 160, 160));
    SelectObject(dc, grey);
    MoveToEx(dc, cx - rad, cy, NULL); LineTo(dc, cx + rad, cy);
    MoveToEx(dc, cx, cy - rad, NULL); LineTo(dc, cx, cy + rad);
    SelectObject(dc, op); DeleteObject(grey);
    const int dx = cx + (sx - 128) * rad / 128, dy = cy + (sy - 128) * rad / 128;
    HBRUSH dot = CreateSolidBrush((sx != 128 || sy != 128) ? RGB(220, 60, 30) : RGB(90, 90, 90));
    ob = SelectObject(dc, dot);
    Ellipse(dc, dx - 9, dy - 9, dx + 9, dy + 9);
    SelectObject(dc, ob); DeleteObject(dot);
    wchar_t t[48]; _snwprintf(t, 48, L"%s   X %3d  Y %3d", label, sx, sy);
    pt_text(dc, t, cx - 110, cy + rad + 6, 220, 22, DT_CENTER | DT_SINGLELINE);
}
static void pt_pressed_names(unsigned short b, wchar_t* out, int cap)
{
    static const struct { unsigned short bit; const wchar_t* name; } n[] = {
        { CELL_PAD_CTRL_CROSS, L"×" }, { CELL_PAD_CTRL_CIRCLE, L"○" }, { CELL_PAD_CTRL_SQUARE, L"□" },
        { CELL_PAD_CTRL_TRIANGLE, L"△" }, { CELL_PAD_CTRL_L1, L"L1" }, { CELL_PAD_CTRL_R1, L"R1" },
        { CELL_PAD_CTRL_L2, L"L2" }, { CELL_PAD_CTRL_R2, L"R2" }, { CELL_PAD_CTRL_L3, L"L3" },
        { CELL_PAD_CTRL_R3, L"R3" }, { CELL_PAD_CTRL_UP, L"↑" }, { CELL_PAD_CTRL_DOWN, L"↓" },
        { CELL_PAD_CTRL_LEFT, L"←" }, { CELL_PAD_CTRL_RIGHT, L"→" }, { CELL_PAD_CTRL_SELECT, L"SELECT" },
        { CELL_PAD_CTRL_START, L"START" },
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof n / sizeof n[0]; i++)
        if (b & n[i].bit) { if (out[0]) wcsncat(out, L" ", cap - wcslen(out) - 1); wcsncat(out, n[i].name, cap - wcslen(out) - 1); }
}
static const wchar_t* pt_type_ko(const char* type)
{
    if (!strcmp(type, "DualSense"))        return macross_ui_string("controller.dualsense");
    if (!strcmp(type, "DualShock 4"))      return macross_ui_string("controller.dualshock4");
    if (!strcmp(type, "DualShock 3"))      return macross_ui_string("controller.dualshock3");
    if (!strcmp(type, "Xbox One/Series"))  return macross_ui_string("controller.xbox_series");
    if (!strcmp(type, "Xbox 360"))         return macross_ui_string("controller.xbox360");
    if (!strcmp(type, "Switch Pro"))       return macross_ui_string("controller.switch_pro");
    if (!strcmp(type, "Xbox (XInput)"))    return macross_ui_string("controller.xinput");
    return macross_ui_string("controller.generic");
}
static void pt_paint(HWND hwnd, HDC dc)
{
    RECT cr; GetClientRect(hwnd, &cr);
    FillRect(dc, &cr, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, s_pt_font);
    const PadTestInfo* in = &s_pt_info;
    wchar_t t[256];

    /* backend and devices */
    if (in->backend == 2)
        _snwprintf(t, 256, macross_ui_string("controller.backend.sdl"), in->sdl_version);
    else if (in->backend == 1)
        _snwprintf(t, 256, macross_ui_string("controller.backend.xinput"));
    else
        _snwprintf(t, 256, macross_ui_string("controller.not_initialized"));
    pt_text(dc, t, 16, 10, cr.right - 32, 22, DT_SINGLELINE);
    _snwprintf(t, 256, macross_ui_string("controller.count"), in->ndev);
    pt_text(dc, t, 16, 38, 400, 22, DT_SINGLELINE);
    SelectObject(dc, s_pt_small);
    if (!in->ndev) {
        pt_text(dc, macross_ui_string("controller.none"),
                30, 64, cr.right - 46, 22, DT_SINGLELINE);
    }
    for (int i = 0; i < in->ndev && i < 4; i++) {
        wchar_t name[64], pressed[96];
        MultiByteToWideChar(CP_UTF8, 0, in->dev[i].name, -1, name, 64);
        pt_pressed_names(in->dev[i].st.buttons, pressed, 96);
        _snwprintf(t, 256, macross_ui_string("controller.device_status"), i + 1, name, pt_type_ko(in->dev[i].type),
                   pressed[0] ? pressed : macross_ui_string("input.none"));
        pt_text(dc, t, 30, 64 + i * 22, cr.right - 46, 22, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    _snwprintf(t, 256, macross_ui_string("input.keyboard_status"), in->keyboard ? macross_ui_string("input.pressed") : macross_ui_string("input.waiting"));
    pt_text(dc, t, 30, 156, cr.right - 46, 22, DT_SINGLELINE);

    /* what the title reads */
    SelectObject(dc, s_pt_font);
    const PadTestState* p = &in->port0;
    const unsigned short b = p->buttons;
    const int top = 190;
    HPEN line = CreatePen(PS_SOLID, 1, RGB(170, 170, 170));
    HGDIOBJ op = SelectObject(dc, line);
    MoveToEx(dc, 16, top, NULL); LineTo(dc, cr.right - 16, top);
    SelectObject(dc, op); DeleteObject(line);
    pt_text(dc, macross_ui_string("input.combined"), 16, top + 6, 400, 22, DT_SINGLELINE);

    pt_trigger(dc, 40, top + 40, 150, L"L2", p->l2, (b & CELL_PAD_CTRL_L2) != 0);
    pt_button(dc, 40, top + 70, 150, 28, 0, L"L1", (b & CELL_PAD_CTRL_L1) != 0);
    pt_trigger(dc, cr.right - 190, top + 40, 150, L"R2", p->r2, (b & CELL_PAD_CTRL_R2) != 0);
    pt_button(dc, cr.right - 190, top + 70, 150, 28, 0, L"R1", (b & CELL_PAD_CTRL_R1) != 0);

    const int dx = 115, dy = top + 160, s = 36;   /* d-pad centre */
    pt_button(dc, dx - s / 2, dy - s / 2 - s, s, s, 0, L"↑", (b & CELL_PAD_CTRL_UP) != 0);
    pt_button(dc, dx - s / 2, dy + s / 2, s, s, 0, L"↓", (b & CELL_PAD_CTRL_DOWN) != 0);
    pt_button(dc, dx - s / 2 - s, dy - s / 2, s, s, 0, L"←", (b & CELL_PAD_CTRL_LEFT) != 0);
    pt_button(dc, dx + s / 2, dy - s / 2, s, s, 0, L"→", (b & CELL_PAD_CTRL_RIGHT) != 0);

    const int fx = cr.right - 115, fy = dy, f = 40;   /* face buttons centre */
    pt_button(dc, fx - f / 2, fy - f / 2 - f, f, f, 1, L"△", (b & CELL_PAD_CTRL_TRIANGLE) != 0);
    pt_button(dc, fx - f / 2, fy + f / 2, f, f, 1, L"×", (b & CELL_PAD_CTRL_CROSS) != 0);
    pt_button(dc, fx - f / 2 - f, fy - f / 2, f, f, 1, L"□", (b & CELL_PAD_CTRL_SQUARE) != 0);
    pt_button(dc, fx + f / 2, fy - f / 2, f, f, 1, L"○", (b & CELL_PAD_CTRL_CIRCLE) != 0);

    const int mid = cr.right / 2;
    pt_button(dc, mid - 105, dy - 14, 90, 28, 0, L"SELECT", (b & CELL_PAD_CTRL_SELECT) != 0);
    pt_button(dc, mid + 15, dy - 14, 90, 28, 0, L"START", (b & CELL_PAD_CTRL_START) != 0);

    SelectObject(dc, s_pt_small);
    pt_stick(dc, mid - 120, top + 290, 52, p->lx, p->ly, macross_ui_string("controller.left_stick"), (b & CELL_PAD_CTRL_L3) != 0);
    pt_stick(dc, mid + 120, top + 290, 52, p->rx, p->ry, macross_ui_string("controller.right_stick"), (b & CELL_PAD_CTRL_R3) != 0);

    pt_text(dc, in->blocked ? macross_ui_string("input.paused")
                            : L"",
            16, top + 380, cr.right - 32, 22, DT_SINGLELINE);
}
static LRESULT CALLBACK null_padtest_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == ID_PT_TIMER) {
            cellPad_test_info(&s_pt_info);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == ID_PT_RUMBLE_OFF) {
            KillTimer(hwnd, ID_PT_RUMBLE_OFF);
            cellPad_test_rumble(0);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_PT_RUMBLE) {
            cellPad_test_rumble(1);
            SetTimer(hwnd, ID_PT_RUMBLE_OFF, 700, NULL);
        } else if (LOWORD(wp) == ID_PT_CLOSE) DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        /* drawn off screen, then copied: no flicker at 30 updates a second */
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        pt_paint(hwnd, mem);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_PT_TIMER); KillTimer(hwnd, ID_PT_RUMBLE_OFF);
        cellPad_test_rumble(0);
        cellPad_test_block(0);
        s_pt_hwnd = NULL;
        if (s_opt_hwnd) SetForegroundWindow(s_opt_hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void null_open_padtest(HWND owner)
{
    if (s_pt_hwnd) { SetForegroundWindow(s_pt_hwnd); return; }
    static int registered;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = null_padtest_wndproc; wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = NULL;
        wc.lpszClassName = L"ps3recomp_padtest";
        RegisterClassExW(&wc);
        registered = 1;
    }
    if (!s_pt_font) s_pt_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    if (!s_pt_small) s_pt_small = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    RECT wr = { 0, 0, 760, 640 };
    AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    RECT orc; GetWindowRect(owner, &orc);
    cellPad_test_block(1);
    cellPad_test_info(&s_pt_info);
    s_pt_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"ps3recomp_padtest", macross_ui_string("controller.test.title"),
                                WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_CLIPCHILDREN,
                                (orc.left + orc.right - ww) / 2, (orc.top + orc.bottom - wh) / 2, ww, wh,
                                owner, NULL, hi, NULL);
    if (!s_pt_hwnd) { cellPad_test_block(0); return; }
    HWND c = CreateWindowExW(0, L"BUTTON", macross_ui_string("controller.test.vibration"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                             16, 596, 140, 32, s_pt_hwnd, (HMENU)(INT_PTR)ID_PT_RUMBLE, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_pt_font, TRUE);
    c = CreateWindowExW(0, L"BUTTON", macross_ui_string("common.close"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        760 - 116, 596, 100, 32, s_pt_hwnd, (HMENU)(INT_PTR)ID_PT_CLOSE, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_pt_font, TRUE);
    SetTimer(s_pt_hwnd, ID_PT_TIMER, 33, NULL);
}
/* ---------------------------------------------------------------------------
 * Pad button settings window (from the options window): each physical pad
 * button gets a PS3 button from a list (pad_padmap.h, saved to padmap.ini;
 * cellPad re-reads it, so it applies at once to every pad). Pressing a pad
 * button lights its row, so the player sees which row is which button on
 * their controller. The title gets neutral input while it is open.
 * -----------------------------------------------------------------------*/
#include "../input/pad_padmap.h"
enum { ID_PM_BASE = 500, ID_PM_LABEL = 540, ID_PM_DEFAULTS = 590, ID_PM_SWAP, ID_PM_CLOSE, ID_PM_TIMER = 4 };
static HWND s_pm_hwnd;
static int s_pm_act[PAD_SRC_COUNT];
static unsigned s_pm_raw;
static HBRUSH s_pm_hot;
static void pad_pm_refresh_combos(void)
{
    for (int i = 0; i < PAD_SRC_COUNT; i++)
        SendDlgItemMessageW(s_pm_hwnd, ID_PM_BASE + i, CB_SETCURSEL, (WPARAM)s_pm_act[i], 0);
}
static LRESULT CALLBACK null_padmap_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_PM_BASE && id < ID_PM_BASE + PAD_SRC_COUNT && code == CBN_SELCHANGE) {
            const LRESULT sel = SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < PAD_ACT_COUNT) { s_pm_act[id - ID_PM_BASE] = (int)sel; pad_pm_save(s_pm_act); }
        } else if (id == ID_PM_DEFAULTS) {
            pad_pm_defaults(s_pm_act); pad_pm_save(s_pm_act); pad_pm_refresh_combos();
        } else if (id == ID_PM_SWAP) {
            for (int i = 0; i < PAD_SRC_COUNT; i++)
                s_pm_act[i] = s_pm_act[i] == PAD_ACT_CROSS ? PAD_ACT_CIRCLE
                            : s_pm_act[i] == PAD_ACT_CIRCLE ? PAD_ACT_CROSS : s_pm_act[i];
            pad_pm_save(s_pm_act); pad_pm_refresh_combos();
        } else if (id == ID_PM_CLOSE) DestroyWindow(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == ID_PM_TIMER) {
            PadTestInfo in;
            cellPad_test_info(&in);
            unsigned raw = 0;
            for (int d = 0; d < in.ndev; d++) raw |= in.dev[d].st.raw;
            const unsigned changed = raw ^ s_pm_raw;
            s_pm_raw = raw;
            for (int i = 0; i < PAD_SRC_COUNT; i++)
                if ((changed >> i) & 1u) InvalidateRect(GetDlgItem(hwnd, ID_PM_LABEL + i), NULL, TRUE);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID((HWND)lp);
        if (id >= ID_PM_LABEL && id < ID_PM_LABEL + PAD_SRC_COUNT && ((s_pm_raw >> (id - ID_PM_LABEL)) & 1u)) {
            SetBkColor((HDC)wp, RGB(255, 150, 40));
            SetTextColor((HDC)wp, RGB(255, 255, 255));
            return (LRESULT)s_pm_hot;
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_PM_TIMER);
        cellPad_test_block(0);
        s_pm_hwnd = NULL; s_pm_raw = 0;
        if (s_opt_hwnd) SetForegroundWindow(s_opt_hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void null_open_padmap(HWND owner)
{
    if (s_pm_hwnd) { SetForegroundWindow(s_pm_hwnd); return; }
    static int registered;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = null_padmap_wndproc; wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ps3recomp_opt_pad";
        RegisterClassExW(&wc);
        registered = 1;
    }
    if (!s_pm_hot) s_pm_hot = CreateSolidBrush(RGB(255, 150, 40));
    pad_pm_load(s_pm_act);
    const int rows = (PAD_SRC_COUNT + 1) / 2, rh = 34, top = 84;
    const int cw = 880, ch = top + rows * rh + 56;
    RECT wr = { 0, 0, cw, ch };
    AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    RECT orc; GetWindowRect(owner, &orc);
    s_pm_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"ps3recomp_opt_pad", macross_ui_string("controller.bindings.title"),
                                WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                (orc.left + orc.right - ww) / 2, (orc.top + orc.bottom - wh) / 2, ww, wh,
                                owner, NULL, hi, NULL);
    if (!s_pm_hwnd) return;
    cellPad_test_block(1);
    static HFONT font;
    if (!font) font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    HWND c;
#define PM_CTL(cls, text, style, x, y, w, h, id) \
    c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (style), x, y, w, h, s_pm_hwnd, (HMENU)(INT_PTR)(id), hi, NULL); \
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    PM_CTL(L"STATIC", macross_ui_string("controller.bindings.help"),
           0, 16, 8, cw - 32, 68, 0)
    for (int i = 0; i < PAD_SRC_COUNT; i++) {
        const int col = i / rows, row = i % rows;
        const int x = 16 + col * 440, y = top + row * rh;
        PM_CTL(L"STATIC", k_pad_pm_src[i].label, SS_CENTERIMAGE, x, y, 236, 28, ID_PM_LABEL + i)
        PM_CTL(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x + 242, y, 170, 420, ID_PM_BASE + i)
        for (int a = 0; a < PAD_ACT_COUNT; a++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)k_pad_pm_act[a].label);
        SendMessageW(c, CB_SETCURSEL, (WPARAM)s_pm_act[i], 0);
    }
    const int by = top + rows * rh + 14;
    PM_CTL(L"BUTTON", macross_ui_string("common.defaults"), BS_PUSHBUTTON | WS_TABSTOP, 16, by, 140, 32, ID_PM_DEFAULTS)
    PM_CTL(L"BUTTON", macross_ui_string("controller.swap_confirm"), BS_PUSHBUTTON | WS_TABSTOP, 166, by, 180, 32, ID_PM_SWAP)
    PM_CTL(L"BUTTON", macross_ui_string("common.close"), BS_PUSHBUTTON | WS_TABSTOP, cw - 116, by, 100, 32, ID_PM_CLOSE)
#undef PM_CTL
    SetTimer(s_pm_hwnd, ID_PM_TIMER, 40, NULL);
}
static HWND s_music_hwnd;
enum { ID_MUSIC_PATH=701,ID_MUSIC_CHOOSE,ID_MUSIC_OPEN,ID_MUSIC_REFRESH,ID_MUSIC_STATUS };
static void null_music_status(int refresh)
{
    char path[32768];wchar_t wide[32768],status[256];
    if(cm_get_folder(path,sizeof path)==0) {
        MultiByteToWideChar(CP_UTF8,0,path,-1,wide,32768);
        SetDlgItemTextW(s_music_hwnd,ID_MUSIC_PATH,wide);
    }
    int count=0;CmTrack track;
    if(refresh)count=cm_refresh();else while(cm_track((unsigned)count,&track))count++;
    if(count<0)SetDlgItemTextW(s_music_hwnd,ID_MUSIC_STATUS,macross_ui_string("music.scan_error"));
    else {
        _snwprintf(status,256,macross_ui_string("music.track_count"),count);
        SetDlgItemTextW(s_music_hwnd,ID_MUSIC_STATUS,status);
    }
}
static int CALLBACK null_music_browse(HWND hwnd,UINT msg,LPARAM lp,LPARAM data)
{
    (void)lp;if(msg==BFFM_INITIALIZED)SendMessageW(hwnd,BFFM_SETSELECTIONW,TRUE,data);return 0;
}
static LRESULT CALLBACK null_music_wndproc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    (void)lp;
    if(msg==WM_COMMAND) {
        int id=LOWORD(wp);
        if(id==ID_MUSIC_REFRESH) {null_music_status(1);return 0;}
        if(id==ID_MUSIC_OPEN||id==ID_MUSIC_CHOOSE) {
            char path[32768];wchar_t wide[32768];
            if(cm_get_folder(path,sizeof path))return 0;
            MultiByteToWideChar(CP_UTF8,0,path,-1,wide,32768);
            if(id==ID_MUSIC_OPEN) {ShellExecuteW(hwnd,L"open",wide,NULL,NULL,SW_SHOWNORMAL);return 0;}
            HRESULT com=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
            BROWSEINFOW bi={0};bi.hwndOwner=hwnd;bi.lpszTitle=macross_ui_string("music.choose");
            bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;bi.lpfn=null_music_browse;bi.lParam=(LPARAM)wide;
            LPITEMIDLIST chosen=SHBrowseForFolderW(&bi);
            if(chosen) {
                wchar_t selected[MAX_PATH];
                if(SHGetPathFromIDListW(chosen,selected)) {
                    WideCharToMultiByte(CP_UTF8,0,selected,-1,path,sizeof path,NULL,NULL);
                    if(cm_set_folder(path))MessageBoxW(hwnd,macross_ui_string("music.folder_error"),macross_ui_string("music.title"),MB_OK|MB_ICONERROR);
                    null_music_status(1);
                }
                CoTaskMemFree(chosen);
            }
            if(SUCCEEDED(com))CoUninitialize();return 0;
        }
    }
    if(msg==WM_CLOSE){DestroyWindow(hwnd);return 0;}
    if(msg==WM_DESTROY){s_music_hwnd=NULL;return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void null_open_music(HWND owner)
{
    if(s_music_hwnd){SetForegroundWindow(s_music_hwnd);return;}
    static int registered;HINSTANCE hi=GetModuleHandleW(NULL);
    if(!registered) {
        WNDCLASSEXW wc={0};wc.cbSize=sizeof wc;wc.lpfnWndProc=null_music_wndproc;wc.hInstance=hi;
        wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        wc.lpszClassName=L"macross_custom_music";RegisterClassExW(&wc);registered=1;
    }
    RECT r;GetWindowRect(owner,&r);
    s_music_hwnd=CreateWindowExW(WS_EX_TOPMOST,L"macross_custom_music",macross_ui_string("music.title"),
        WS_CAPTION|WS_SYSMENU|WS_VISIBLE,(r.left+r.right-660)/2,(r.top+r.bottom-270)/2,660,270,owner,NULL,hi,NULL);
    if(!s_music_hwnd)return;
    static HFONT font;if(!font)font=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Malgun Gothic");
    HWND c;
#define MUSIC_CTL(cls,text,style,x,y,w,h,id) \
    c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|(style),x,y,w,h,s_music_hwnd,(HMENU)(INT_PTR)(id),hi,NULL); \
    SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE);
    MUSIC_CTL(L"STATIC",macross_ui_string("music.folder"),0,16,12,610,22,0)
    MUSIC_CTL(L"EDIT",L"",WS_BORDER|ES_READONLY|ES_AUTOHSCROLL,16,38,610,28,ID_MUSIC_PATH)
    MUSIC_CTL(L"BUTTON",macross_ui_string("music.choose"),BS_PUSHBUTTON|WS_TABSTOP,16,80,195,32,ID_MUSIC_CHOOSE)
    MUSIC_CTL(L"BUTTON",macross_ui_string("music.open_folder"),BS_PUSHBUTTON|WS_TABSTOP,224,80,195,32,ID_MUSIC_OPEN)
    MUSIC_CTL(L"BUTTON",macross_ui_string("music.refresh"),BS_PUSHBUTTON|WS_TABSTOP,432,80,194,32,ID_MUSIC_REFRESH)
    MUSIC_CTL(L"STATIC",L"",0,16,128,610,24,ID_MUSIC_STATUS)
    MUSIC_CTL(L"STATIC",macross_ui_string("music.help"),0,16,160,610,60,0)
#undef MUSIC_CTL
    null_music_status(1);
}

enum { ID_OPT_SCALE = 101, ID_OPT_FULL, ID_OPT_VSYNC, ID_OPT_BLOOM, ID_OPT_UNLOCK, ID_OPT_CLOSE, ID_OPT_KEYS, ID_OPT_PADTEST, ID_OPT_PADMAP, ID_OPT_SUBTITLES, ID_OPT_MUSIC, ID_OPT_HD_TEXTURES };
static LRESULT CALLBACK null_opt_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        const int checked = ((id >= ID_OPT_FULL && id <= ID_OPT_UNLOCK) || id == ID_OPT_SUBTITLES || id == ID_OPT_HD_TEXTURES) &&
                            SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (id == ID_OPT_SCALE && code == CBN_SELCHANGE) {
            const LRESULT sel = SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < 6) s_set.scale_idx = (int)sel;
        } else if (id == ID_OPT_FULL) { s_set.fullscreen = checked; null_apply_fullscreen(s_state.hwnd, checked); }
        else if (id == ID_OPT_VSYNC) { s_set.vsync = checked; rsx_live_draw_set_vsync(checked); }
        else if (id == ID_OPT_UNLOCK && code == BN_CLICKED) {
            s_set.fps_unlock = s_fps_apply ? s_fps_apply(checked) : checked;
            SendMessageW((HWND)lp, BM_SETCHECK, s_set.fps_unlock ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        else if (id == ID_OPT_SUBTITLES && code == BN_CLICKED) { s_set.subtitles = checked; null_subtitles_publish(checked); }
        else if (id == ID_OPT_HD_TEXTURES && code == BN_CLICKED) { s_set.hd_textures = checked; }
        else if (id == ID_OPT_CLOSE) { DestroyWindow(hwnd); return 0; }
        else if (id == ID_OPT_KEYS) { null_open_keys(hwnd); return 0; }
        else if (id == ID_OPT_PADTEST) { null_open_padtest(hwnd); return 0; }
        else if (id == ID_OPT_PADMAP) { null_open_padmap(hwnd); return 0; }
        else if (id == ID_OPT_MUSIC) { null_open_music(hwnd); return 0; }
        else return DefWindowProcW(hwnd, msg, wp, lp);
        null_settings_save();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (s_key_hwnd) DestroyWindow(s_key_hwnd);
        if (s_pt_hwnd) DestroyWindow(s_pt_hwnd);
        if (s_pm_hwnd) DestroyWindow(s_pm_hwnd);
        if (s_music_hwnd) DestroyWindow(s_music_hwnd);
        s_opt_hwnd = NULL; if (s_state.hwnd) SetForegroundWindow(s_state.hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void null_open_options(HWND owner)
{
    if (s_opt_hwnd) { SetForegroundWindow(s_opt_hwnd); return; }
    static int registered;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = null_opt_wndproc; wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ps3recomp_options";
        RegisterClassExW(&wc);
        registered = 1;
    }
    /* Centred on the game window (also over it in fullscreen: topmost). */
    RECT orc; GetWindowRect(owner, &orc);
    const int ow = 470, oh = 554;
    const int ox = (orc.left + orc.right - ow) / 2, oy = (orc.top + orc.bottom - oh) / 2;
    s_opt_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"ps3recomp_options", macross_ui_string("options.title"),
                                 WS_CAPTION | WS_SYSMENU | WS_VISIBLE, ox, oy, ow, oh,
                                 owner, NULL, hi, NULL);
    if (!s_opt_hwnd) return;
    static HFONT font;
    if (!font) font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    HWND c;
#define OPT_CTL(cls, text, style, x, y, w, h, id) \
    c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (style), x, y, w, h, s_opt_hwnd, (HMENU)(INT_PTR)(id), hi, NULL); \
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    OPT_CTL(L"STATIC", macross_ui_string("options.resolution"), 0, 20, 16, 400, 22, 0)
    OPT_CTL(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 20, 40, 400, 200, ID_OPT_SCALE)
    {
        const wchar_t* const labels[6] = { macross_ui_string("options.resolution.original"), L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3200 x 1800", L"3840 x 2160 (4K)" };
        for (int i = 0; i < 6; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)labels[i]);
        SendMessageW(c, CB_SETCURSEL, (WPARAM)s_set.scale_idx, 0);
    }
    OPT_CTL(L"BUTTON", macross_ui_string("options.fullscreen"), BS_AUTOCHECKBOX | WS_TABSTOP, 20, 84, 400, 24, ID_OPT_FULL)
    SendMessageW(c, BM_SETCHECK, s_set.fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    OPT_CTL(L"BUTTON", macross_ui_string("options.vsync"), BS_AUTOCHECKBOX | WS_TABSTOP, 20, 114, 400, 24, ID_OPT_VSYNC)
    SendMessageW(c, BM_SETCHECK, s_set.vsync ? BST_CHECKED : BST_UNCHECKED, 0);
#if defined(MACROSS_RELEASE_FPS_LOCKED) && MACROSS_RELEASE_FPS_LOCKED
    OPT_CTL(L"BUTTON", macross_ui_string("options.uncap.unavailable"), BS_AUTOCHECKBOX, 20, 174, 400, 24, ID_OPT_UNLOCK)
    SendMessageW(c, BM_SETCHECK, BST_UNCHECKED, 0);
    EnableWindow(c, FALSE);
#else
    OPT_CTL(L"BUTTON", macross_ui_string("options.uncap"), BS_AUTOCHECKBOX | WS_TABSTOP, 20, 174, 400, 24, ID_OPT_UNLOCK)
    SendMessageW(c, BM_SETCHECK, s_set.fps_unlock ? BST_CHECKED : BST_UNCHECKED, 0);
#endif
    OPT_CTL(L"BUTTON", macross_ui_string("options.subtitles"), BS_AUTOCHECKBOX | WS_TABSTOP, 20, 204, 400, 24, ID_OPT_SUBTITLES)
    SendMessageW(c, BM_SETCHECK, s_set.subtitles ? BST_CHECKED : BST_UNCHECKED, 0);
#if defined(MACROSS_RELEASE_FPS_LOCKED) && MACROSS_RELEASE_FPS_LOCKED
    OPT_CTL(L"STATIC", macross_ui_string("options.restart_help.locked"), 0, 20, 278, 420, 110, 0)
#else
    OPT_CTL(L"STATIC", macross_ui_string("options.restart_help"), 0, 20, 278, 420, 110, 0)
#endif
    OPT_CTL(L"BUTTON", macross_ui_string("options.hd_textures"), BS_AUTOCHECKBOX | WS_TABSTOP, 20, 234, 420, 24, ID_OPT_HD_TEXTURES)
    SendMessageW(c, BM_SETCHECK, s_set.hd_textures ? BST_CHECKED : BST_UNCHECKED, 0);
    OPT_CTL(L"STATIC", macross_ui_string("options.hd_textures.help"), 0, 40, 258, 400, 20, 0)
    OPT_CTL(L"BUTTON", macross_ui_string("input.keyboard.open"), BS_PUSHBUTTON | WS_TABSTOP, 20, 392, 200, 30, ID_OPT_KEYS)
    OPT_CTL(L"BUTTON", macross_ui_string("controller.bindings.open"), BS_PUSHBUTTON | WS_TABSTOP, 230, 392, 200, 30, ID_OPT_PADMAP)
    OPT_CTL(L"BUTTON", macross_ui_string("controller.test.open"), BS_PUSHBUTTON | WS_TABSTOP, 20, 432, 200, 30, ID_OPT_PADTEST)
    OPT_CTL(L"BUTTON", macross_ui_string("common.close"), BS_PUSHBUTTON | WS_TABSTOP, 340, 432, 90, 30, ID_OPT_CLOSE)
    OPT_CTL(L"BUTTON", macross_ui_string("music.title"), BS_PUSHBUTTON | WS_TABSTOP, 20, 472, 410, 30, ID_OPT_MUSIC)
#undef OPT_CTL
}

static LRESULT CALLBACK null_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_state.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Fill with current clear color */
        HBRUSH brush = CreateSolidBrush(s_state.clear_color);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);

        /* Draw debug overlay text */
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        char buf[128];
        snprintf(buf, sizeof(buf), "macross30 null backend | %u FPS | frame %llu",
                 s_state.fps, (unsigned long long)s_state.frame_count);
        TextOutA(hdc, 10, 10, buf, (int)strlen(buf));

        char buf2[64];
        snprintf(buf2, sizeof(buf2), "Clear: #%06X",
                 (unsigned)(s_state.clear_color & 0xFFFFFF));
        TextOutA(hdc, 10, 30, buf2, (int)strlen(buf2));

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == 0x1F90u) { null_open_options(hwnd); return 0; }
        if (LOWORD(wp) == 0x1FA0u) {
            s_set.fullscreen = !s_is_fullscreen;
            null_apply_fullscreen(hwnd, s_set.fullscreen);
            null_settings_save();
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0u) == 0x1F90u) { null_open_options(hwnd); return 0; }
        break;
    case WM_KEYDOWN:
        /* Keyboard buttons belong to cellPad. Escape is a menu/START key,
         * not an implicit request to destroy an interactive game window.
         * F9 opens the options window, F11 toggles fullscreen. */
        if (wp == VK_F9) null_open_options(hwnd);
        else if (wp == VK_F11) {
            s_set.fullscreen = !s_is_fullscreen;
            null_apply_fullscreen(hwnd, s_set.fullscreen);
            null_settings_save();
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {   /* Alt+Enter */
            s_set.fullscreen = !s_is_fullscreen;
            null_apply_fullscreen(hwnd, s_set.fullscreen);
            null_settings_save();
            return 0;
        }
        break;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------------------
 * Backend callbacks
 * -----------------------------------------------------------------------*/

static int null_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[RSX null] init(%ux%u)\n", width, height);
    return 0;
}

static void null_shutdown(void* ud)
{
    (void)ud;
    printf("[RSX null] shutdown\n");
}

static void null_begin_frame(void* ud)
{
    (void)ud;
}

static void null_end_frame(void* ud)
{
    (void)ud;
    s_state.frame_count++;

    /* Compute FPS every second */
    ULONGLONG now = GetTickCount64();
    if (now - s_state.last_fps_time >= 1000) {
        /* Simple: just use frame_count delta. For first second, estimate. */
        s_state.fps = (u32)(s_state.frame_count -
                           (s_state.frame_count > 60 ? s_state.frame_count - 60 : 0));
        s_state.last_fps_time = now;
    }
}

static void null_present(void* ud, u32 buffer_id)
{
    /* The live draw engine presents through its own swap chain on this HWND;
     * leaving the GDI blit running underneath would fight it for the window. */
    if (s_present_suppressed) return;
    (void)ud;
    (void)buffer_id;

    /* Trigger a repaint */
    if (s_state.hwnd)
        InvalidateRect(s_state.hwnd, NULL, FALSE);
}

static void null_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB to Win32 COLORREF (BGR) */
    u8 r = (color >> 16) & 0xFF;
    u8 g = (color >> 8) & 0xFF;
    u8 b = color & 0xFF;
    s_state.clear_color = RGB(r, g, b);
}

static void null_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    if (ps3_log_verbose()) printf("[RSX null] set_render_target(format=0x%X, %ux%u)\n",
           state->surface_format, state->surface_clip_w, state->surface_clip_h);
}

static void null_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    if (ps3_log_verbose()) printf("[RSX null] set_viewport(%u,%u %ux%u)\n",
           state->viewport_x, state->viewport_y,
           state->viewport_w, state->viewport_h);
}

static void null_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_arrays(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        s_draw_log_count++;
    }
}

static void null_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_indexed(prim=%u, offset=%u, count=%u)\n",
               primitive, offset, count);
        s_draw_log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_null_backend = {
    .userdata          = &s_state,
    .init              = null_init,
    .shutdown          = null_shutdown,
    .begin_frame       = null_begin_frame,
    .end_frame         = null_end_frame,
    .present           = null_present,
    .set_render_target = null_set_render_target,
    .set_viewport      = null_set_viewport,
    .set_blend         = NULL,
    .set_depth_stencil = NULL,
    .clear             = null_clear,
    .draw_arrays       = null_draw_arrays,
    .draw_indexed      = null_draw_indexed,
    .bind_texture      = NULL,
};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.width = width;
    s_state.height = height;
    s_state.clear_color = RGB(0, 0, 64); /* dark blue default */

    /* Register window class */
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = null_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ps3recomp_null";
    RegisterClassExA(&wc);

    /* Compute window size from client area */
    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    s_state.hwnd = CreateWindowExA(
        0, "ps3recomp_null",
        title && strcmp(title, "ps3recomp") ? title : "macross30",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    if (!s_state.hwnd) {
        printf("[RSX null] ERROR: CreateWindow failed (%lu)\n", GetLastError());
        return -1;
    }

    /* Saved options: fullscreen and bloom now; vsync once the renderer
     * reads its present interval (the setter only stores it). */
    {   /* Menu bar: 옵션 > 그래픽 옵션 / 전체화면 (hidden in fullscreen). */
        HMENU bar = CreateMenu(), sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, 0x1F90u, macross_ui_string("options.open"));
        AppendMenuW(sub, MF_STRING, 0x1FA0u, macross_ui_string("options.fullscreen.menu"));
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)sub, macross_ui_string("options.menu"));
        s_menu_bar = bar;
        SetMenu(s_state.hwnd, bar);
        /* Keep the client area at the requested size with the menu added. */
        RECT mr = {0, 0, (LONG)width, (LONG)height};
        AdjustWindowRect(&mr, WS_OVERLAPPEDWINDOW, TRUE);
        SetWindowPos(s_state.hwnd, NULL, 0, 0, mr.right - mr.left, mr.bottom - mr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    { HMENU sm = GetSystemMenu(s_state.hwnd, FALSE);   /* title-bar menu entry */
      if (sm) { AppendMenuW(sm, MF_SEPARATOR, 0, NULL); AppendMenuW(sm, MF_STRING, 0x1F90u, macross_ui_string("options.system_menu")); } }
    null_settings_load();
    if (s_fps_apply) s_set.fps_unlock = s_fps_apply(s_set.fps_unlock);
    null_apply_bloom();
    rsx_live_draw_set_vsync(s_set.vsync);
    if (s_set.fullscreen) null_apply_fullscreen(s_state.hwnd, 1);

    s_state.hdc = GetDC(s_state.hwnd);
    s_state.last_fps_time = GetTickCount64();
    snprintf(s_state.title, sizeof(s_state.title), "%s", title && strcmp(title, "ps3recomp") ? title : "macross30");
    s_state.title_time = s_state.last_fps_time;

    /* Keep the display awake, as any PC game does. With the monitor asleep a
     * vsync'd Present crawls (~4 fps); guest frames stretch to 250 ms and GH3's
     * Havok step takes huge timesteps, which is how an unattended run stalled
     * in the intro movie while an attended one did not. */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    /* Register as the active RSX backend */
    rsx_set_backend(&s_null_backend);

    printf("[RSX null] Window created: %ux%u\n", width, height);
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    if (s_state.hwnd) {
        ReleaseDC(s_state.hwnd, s_state.hdc);
        DestroyWindow(s_state.hwnd);
        s_state.hwnd = NULL;
    }
    rsx_set_backend(NULL);
    printf("[RSX null] Backend shut down after %llu frames\n",
           (unsigned long long)s_state.frame_count);
}

int rsx_null_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    /* RPCS3-style caption, once a second: guest FPS (flips), draws in the last
     * live-draw frame, window size. Here on the window's own thread, so the
     * WM_SETTEXT never blocks the renderer. */
    ULONGLONG now = GetTickCount64();
    if (s_state.hwnd && now - s_state.title_time >= 1000) {
        extern u32 rsx_live_draw_get_last_draws(void);
        extern double rsx_live_draw_get_present_fps(void);
        /* Live draw owns presentation (and end_frame never runs) when enabled. */
        double fps = rsx_live_draw_get_present_fps();
        if (fps <= 0.0)
            fps = (s_state.frame_count - s_state.title_frames) * 1000.0 /
                  (double)(now - s_state.title_time);
        wchar_t tb[256];
        swprintf(tb, 256, macross_ui_string("window.title"), s_state.title, fps,
                 rsx_live_draw_get_last_draws(), s_state.width, s_state.height);
        SetWindowTextW(s_state.hwnd, tb);
        s_state.title_frames = s_state.frame_count;
        s_state.title_time = now;
    }
    return s_state.window_closed ? -1 : 0;
}

/* Ported from caner (canersaka) Yakuza-Dead-Souls-EX: the live NV4097->D3D12
 * engine binds its swap chain to this window and then owns presentation, so it
 * needs the HWND and a way to silence the GDI present underneath it. */
void* rsx_null_backend_get_hwnd(void)
{
    return (void*)s_state.hwnd;
}

void rsx_null_backend_suppress_present(int on)
{
    s_present_suppressed = on;
}

#else /* !_WIN32 */

/* ---------------------------------------------------------------------------
 * Headless software backend
 *
 * No window and no GPU: the RSX target is a plain u32 framebuffer in host
 * memory, and triangles are filled on the CPU. That is enough to answer the
 * one question this backend exists to answer -- did the guest's command
 * stream actually reach a backend and produce the pixels it asked for -- on
 * any platform, including a CI runner with no display and no GPU driver.
 *
 * Linux has no real backend yet (Metal is Apple-only, D3D12 Windows-only), so
 * without this ps3recomp_host cannot be built or run there at all and the
 * whole cellGcm -> RSX -> backend bridge goes unexercised on the platform.
 *
 * ponytail: flat-shaded triangles from the first vertex's colour, no blending,
 * no stencil, no clipping beyond the framebuffer bounds, and the viewport is
 * the whole target. It is a correctness probe, not a renderer. Anything more
 * belongs in a real backend.
 *
 * It does keep a depth buffer, because draw ORDER is one of the things the
 * probe has to be able to answer: without one it draws in submission order and
 * cannot tell a working depth test from a missing one.
 * -----------------------------------------------------------------------*/

#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdlib.h>

typedef struct {
    u32* fb;                /* width * height, 0x00RRGGBB          */
    float* depth;           /* width * height, window z in [0,1]   */
    u32  width, height;
    u32  clear_argb;        /* last NV4097_SET_COLOR_CLEAR_VALUE   */
    u32  last_present;      /* centre pixel of the last presented frame */
    int  presented;
    const rsx_state* state; /* live state, for the draw path       */

    /* One decoded texture unit. The point is not to be a sampler but to
     * prove the shared RSX texture path end to end -- layout, deswizzle,
     * channel order -- on a platform with no GPU at all. */
    u8*  tex;               /* decoded RGBA rows, tex_pitch * tex_h  */
    u32  tex_w, tex_h, tex_pitch;
    int  tex_ready;
} NullSoftState;

static NullSoftState s_soft;

static void nullsw_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                      /* no stencil buffer here */
    s_soft.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the four colour
     * channels. rsx_commands.c decoded `depth` out of ZSTENCIL_CLEAR_VALUE's
     * top 24 bits, so it is already the [0,1] value to store. */
    if (s_soft.depth && (flags & 0x01u))
        for (u32 i = 0; i < s_soft.width * s_soft.height; i++) s_soft.depth[i] = depth;
    if (!s_soft.fb || !(flags & 0xF0u)) return;   /* colour bits only */
    u32 rgb = color & 0x00FFFFFFu;
    for (u32 i = 0; i < s_soft.width * s_soft.height; i++) s_soft.fb[i] = rgb;
}

static void nullsw_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud; s_soft.state = state;
}

static void nullsw_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud; s_soft.state = state;
}

/* NDC -> framebuffer pixels, plus the window z the depth test compares. The
 * guest's clip-space position is divided by w (1.0 for the identity-transform
 * case) and mapped over the whole target; see the ponytail note above about
 * the viewport. z/w is already the [0,1] depth on this hardware, as it is in
 * D3D and Metal, so it needs no remapping. */
static void ndc_to_px(const float p[4], float* sx, float* sy, float* sz)
{
    float w = (p[3] != 0.0f) ? p[3] : 1.0f;
    *sx = ( p[0] / w * 0.5f + 0.5f) * (float)s_soft.width;
    *sy = (-p[1] / w * 0.5f + 0.5f) * (float)s_soft.height;
    *sz = p[2] / w;
}

/* The NV4097 depth function is the GL enum, 0x200 NEVER .. 0x207 ALWAYS.
 * Anything unrecognised passes, as it does in the D3D12 backend's gcm_cmp --
 * including the 1 rsx_state_init seeds the register with. */
static int depth_passes(u32 func, float src, float dst)
{
    switch (func) {
    case 0x0200: return 0;              /* NEVER    */
    case 0x0201: return src <  dst;     /* LESS     */
    case 0x0202: return src == dst;     /* EQUAL    */
    case 0x0203: return src <= dst;     /* LEQUAL   */
    case 0x0204: return src >  dst;     /* GREATER  */
    case 0x0205: return src != dst;     /* NOTEQUAL */
    case 0x0206: return src >= dst;     /* GEQUAL   */
    default:     return 1;              /* ALWAYS   */
    }
}

static u32 colour_of(const float c[4])
{
    float r = c[0] < 0 ? 0 : (c[0] > 1 ? 1 : c[0]);
    float g = c[1] < 0 ? 0 : (c[1] > 1 ? 1 : c[1]);
    float b = c[2] < 0 ? 0 : (c[2] > 1 ? 1 : c[2]);
    return ((u32)(r * 255.0f + 0.5f) << 16) |
           ((u32)(g * 255.0f + 0.5f) <<  8) |
            (u32)(b * 255.0f + 0.5f);
}

/* Decode the bound guest texture once, through the same rsx_texture_layout /
 * rsx_texture_decode the D3D12 backend uses. Only unit 0 is kept: this exists
 * to check that path produces the right pixels, not to be a texture cache. */
static void nullsw_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit != 0 || !t) return;

    /* CONTROL0 bit 31 is the unit enable. A disabled unit unbinds. */
    if (!(t->control0 & 0x80000000u) || !t->offset) { s_soft.tex_ready = 0; return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { s_soft.tex_ready = 0; return; }

    /* SET_TEXTURE_FORMAT as a title writes it: bits [1:0] are the location
     * (1 = local, 2 = main), [15:8] the format byte with its LN/UN flags.
     * The offset is a plain RSX offset. This used to read the format byte
     * from the low bits and a location flag from bit 31 of the offset --
     * a convention only this backend and the host harness shared; the
     * D3D12 backend, which has met real titles, decodes the register. */
    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { s_soft.tex_ready = 0; return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    if (tl.compressed) { s_soft.tex_ready = 0; return; }   /* no BC decode here */

    u32 pitch = w * 4u;
    u8* buf = (u8*)realloc(s_soft.tex, (size_t)pitch * h);
    if (!buf) { s_soft.tex_ready = 0; return; }
    s_soft.tex = buf;

    if (tl.fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(buf, pitch, vm_base + ea, w, h, &tl,
                           rsx_texture_argb_is_rgba());
    } else {
        /* Narrower formats decode to their own width; splay them to RGBA so
         * sampling below has one layout to read. */
        u32 srcp = tl.row_bytes;
        u8* tmp = (u8*)malloc((size_t)srcp * h);
        if (!tmp) { s_soft.tex_ready = 0; return; }
        rsx_texture_decode(tmp, srcp, vm_base + ea, w, h, &tl, 0);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl.bytes_per_texel;
                u8* dp = buf + (size_t)y * pitch + (size_t)x * 4u;
                dp[0] = sp[0];
                dp[1] = tl.bytes_per_texel > 1 ? sp[1] : sp[0];
                dp[2] = tl.bytes_per_texel > 2 ? sp[2] : sp[0];
                dp[3] = 255;
            }
        free(tmp);
    }

    s_soft.tex_w = w; s_soft.tex_h = h; s_soft.tex_pitch = pitch;
    s_soft.tex_ready = 1;
}

/* Nearest-neighbour point sample, wrapping. No filtering, no mip levels, no
 * addressing modes: the guest UVs in the harness land inside [0,1). */
static u32 sample_tex(float u, float v)
{
    if (!s_soft.tex_ready) return 0xFF00FFu;         /* magenta = not bound */
    long sx = (long)(u * (float)s_soft.tex_w);
    long sy = (long)(v * (float)s_soft.tex_h);
    sx %= (long)s_soft.tex_w; if (sx < 0) sx += (long)s_soft.tex_w;
    sy %= (long)s_soft.tex_h; if (sy < 0) sy += (long)s_soft.tex_h;
    const u8* px = s_soft.tex + (size_t)sy * s_soft.tex_pitch + (size_t)sx * 4u;
    return ((u32)px[0] << 16) | ((u32)px[1] << 8) | (u32)px[2];
}

/* Bounding-box scan with the three edge functions; fills when a pixel centre
 * is on the same side of all three edges, so winding does not matter. */
static void fill_triangle(const float a[4], const float b[4], const float c[4],
                          u32 rgb, const float uv[3][4], int textured)
{
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    ndc_to_px(a, &ax, &ay, &az);
    ndc_to_px(b, &bx, &by, &bz);
    ndc_to_px(c, &cx, &cy, &cz);

    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.0f) return;                       /* degenerate */
    const float inv = 1.0f / area;                  /* barycentric scale */

    /* Depth writes need the test enabled, exactly as on the hardware: with
     * SET_DEPTH_TEST_ENABLE off the RSX writes no depth whatever the mask
     * says, and both real backends express that the same way. */
    const rsx_state* dst = s_soft.state;
    const int ztest  = s_soft.depth && dst && dst->depth_test_enable;
    const int zwrite = ztest && dst->depth_mask;
    const u32 zfunc  = dst ? dst->depth_func : 0u;

    /* Bounding box. Plain casts truncate toward zero rather than flooring,
     * which only differs for negative coordinates -- and those clamp to 0 on
     * the next line anyway. +1 on the max side covers the truncation. Keeps
     * this file off libm. */
    float lox = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    float hix = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    float loy = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    float hiy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);

    long x0 = (long)lox,       y0 = (long)loy;
    long x1 = (long)hix + 1,   y1 = (long)hiy + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > (long)s_soft.width)  x1 = (long)s_soft.width;
    if (y1 > (long)s_soft.height) y1 = (long)s_soft.height;

    for (long y = y0; y < y1; y++) {
        for (long x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            float w1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            float w2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            int neg = (w0 < 0) || (w1 < 0) || (w2 < 0);
            int pos = (w0 > 0) || (w1 > 0) || (w2 > 0);
            if (neg && pos) continue;               /* outside */

            /* Barycentric weights fall out of the same edge functions, so
             * interpolating costs nothing extra. Screen-space rather than
             * perspective-correct: the probe's geometry is drawn in a plane,
             * where the two agree. */
            float la = w1 * inv, lb = w2 * inv, lc = w0 * inv;

            if (ztest) {
                float z = la * az + lb * bz + lc * cz;
                float* zp = &s_soft.depth[(u32)y * s_soft.width + (u32)x];
                if (!depth_passes(zfunc, z, *zp)) continue;
                if (zwrite) *zp = z;
            }

            u32 out = rgb;
            if (textured) {
                float u = la * uv[0][0] + lb * uv[1][0] + lc * uv[2][0];
                float v = la * uv[0][1] + lb * uv[1][1] + lc * uv[2][1];
                out = sample_tex(u, v);
            }
            s_soft.fb[(u32)y * s_soft.width + (u32)x] = out;
        }
    }
}

/* Map sequence position -> guest vertex index, expanding the primitives the
 * hardware has and a triangle list does not. Returns the triangle count and
 * writes 3 indices per triangle through `emit`. */
static void draw_prim(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_soft.state;
    if (!st || !s_soft.fb || count < 3) return;

    /* Position is attribute 0 and diffuse colour attribute 3 -- the same slots
     * the D3D12 and Metal fallback paths assume. */
    /* Texture coordinates are attribute 8 (texcoord0), the slot the D3D12 and
     * Metal fetch paths also read. A draw is textured when a texture is bound
     * AND that attribute is actually supplied. */
    const int textured = s_soft.tex_ready && st->vertex_attribs[8].enabled;

    #define TRI(i0, i1, i2) do {                                        \
        float p0[4], p1[4], p2[4], col[4], uv[3][4];                    \
        rsx_fetch_attrib(st, 0, (i0), p0);                              \
        rsx_fetch_attrib(st, 0, (i1), p1);                              \
        rsx_fetch_attrib(st, 0, (i2), p2);                              \
        rsx_fetch_attrib(st, 3, (i0), col);                             \
        rsx_fetch_attrib(st, 8, (i0), uv[0]);                           \
        rsx_fetch_attrib(st, 8, (i1), uv[1]);                           \
        rsx_fetch_attrib(st, 8, (i2), uv[2]);                           \
        fill_triangle(p0, p1, p2, colour_of(col), uv, textured);        \
    } while (0)

    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3)
            TRI(first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            TRI(first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++)
            TRI(first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            TRI(first + i, first + i + 1, first + i + 2);
            TRI(first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            TRI(first + i,     first + i + 1, first + i + 3);
            TRI(first + i,     first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines contribute no coverage here */
    }
    #undef TRI
}

static void nullsw_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    draw_prim(primitive, first, count);
}

static void nullsw_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* ponytail: index buffers are not read; the probe's geometry is
     * non-indexed. A backend that needs them reads them for real. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

static void nullsw_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_soft.fb) return;
    s_soft.last_present =
        s_soft.fb[(s_soft.height / 2) * s_soft.width + s_soft.width / 2];
    s_soft.presented++;
}

static rsx_backend s_nullsw_backend = {
    .userdata          = &s_soft,
    .set_render_target = nullsw_set_render_target,
    .set_vertex_attribs= nullsw_set_vertex_attribs,
    .clear             = nullsw_clear,
    .bind_texture      = nullsw_bind_texture,
    .draw_arrays       = nullsw_draw_arrays,
    .draw_indexed      = nullsw_draw_indexed,
    .present           = nullsw_present,
};

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_soft, 0, sizeof(s_soft));
    s_soft.width  = width  ? width  : 1280;
    s_soft.height = height ? height : 720;
    s_soft.fb = (u32*)calloc((size_t)s_soft.width * s_soft.height, sizeof(u32));
    if (!s_soft.fb) return -1;
    /* The far plane, not calloc's zero: zero is the NEAR plane, so a guest
     * that enables the depth test before its first depth clear would draw
     * nothing at all under LESS. */
    s_soft.depth = (float*)malloc((size_t)s_soft.width * s_soft.height * sizeof(float));
    if (!s_soft.depth) { free(s_soft.fb); s_soft.fb = NULL; return -1; }
    for (u32 i = 0; i < s_soft.width * s_soft.height; i++) s_soft.depth[i] = 1.0f;
    rsx_set_backend(&s_nullsw_backend);
    printf("[RSX null] headless software backend %ux%u (%s)\n",
           s_soft.width, s_soft.height, title ? title : "");
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    rsx_set_backend(NULL);
    free(s_soft.fb);    s_soft.fb    = NULL;
    free(s_soft.depth); s_soft.depth = NULL;
    free(s_soft.tex);   s_soft.tex   = NULL;
    s_soft.tex_ready = 0;
}

/* Headless has no event queue and no window to close. */
void rsx_null_backend_set_fps_callback(int (*apply)(int)) { (void)apply; }
int rsx_null_backend_pump_messages(void) { return 0; }

void rsx_null_backend_present(void) { nullsw_present(NULL, 0); }
void* rsx_null_backend_get_hwnd(void) { return 0; }
void  rsx_null_backend_suppress_present(int on) { (void)on; }
int rsx_null_backend_movie_subtitles_enabled(u32* epoch) { if (epoch) *epoch = 0; return 1; }
int rsx_null_backend_subtitles_enabled(u32* epoch) { return rsx_null_backend_movie_subtitles_enabled(epoch); }

u32 rsx_null_backend_debug_color(void) { return s_soft.clear_argb; }

u32 rsx_null_backend_readback_center(void)
{
    /* 0 means "nothing presented yet", so give a presented black pixel a
     * non-zero alpha the way a real drawable readback would. */
    return s_soft.presented ? (0xFF000000u | s_soft.last_present) : 0u;
}

u32 rsx_null_backend_guest_draws(void) { return 0; }

#endif /* _WIN32 */
