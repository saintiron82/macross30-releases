/*
 * ps3recomp - save-data list dialog (Win32)
 *
 * The window cellSaveDataList{Save,Load}2 show in place of the PS3's OS-drawn
 * list (cellSaveData.c says when and why). Header-only, like ps3_psf.h, so
 * cellSaveData.c includes it and tools/tests/savedlg_preview.c can open the
 * very same window against a save folder without the HLE around it.
 *
 * One card per slot: the slot's ICON0.PNG on the left (decoded with WIC once
 * per run, aspect kept), slot name, save time, PARAM.SFO subtitle and detail
 * text; the "New Data" row is a card of its own. Everything is painted by hand
 * -- no LISTBOX -- so the window can look like a dark game overlay. When WIC is
 * unavailable the cards are text-only.
 *
 * The window itself only READS: ICON0.PNG and file times. Slot text comes from
 * the caller, and the one question it asks while open ("does this slot hold a
 * save already?") goes through the caller's callback.
 * -----------------------------------------------------------------------*/
#ifndef PS3RECOMP_SAVEDLG_WIN32_H
#define PS3RECOMP_SAVEDLG_WIN32_H
#ifdef _WIN32

#ifndef COBJMACROS
#  define COBJMACROS
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <xinput.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")        /* AlphaBlend, GradientFill */
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "xinput.lib")

/* External tools (tools/play/auto_load.ps1, tools/diag/live/cancel_save.ps1)
 * find the window by this class and post WM_KEYDOWN Enter/Esc to it. */
#define SAVEDLG_CLASS     L"Ps3SaveDataListDlg"

/* One row as the caller hands it in. Strings stay owned by the caller for the
 * whole savedlg_ui_run call. */
typedef struct savedlg_row {
    const char* dir;      /* directory name, e.g. "BLJS10184_SAVEDATA_00" */
    const char* path;     /* host directory of the slot (ICON0.PNG, file times) */
    const char* sub;      /* PARAM.SFO SUB_TITLE, UTF-8, "" when none */
    const char* detail;   /* PARAM.SFO DETAIL, UTF-8, '\n'-separated, "" when none */
} savedlg_row;

/* Palette: a dark overlay, one accent per action. */
#define SAVEDLG_C_BG        RGB(0x11, 0x14, 0x19)
#define SAVEDLG_C_BAND      RGB(0x17, 0x1B, 0x22)   /* header / footer */
#define SAVEDLG_C_CARD      RGB(0x1C, 0x21, 0x29)
#define SAVEDLG_C_CARD_HOT  RGB(0x22, 0x28, 0x31)
#define SAVEDLG_C_EDGE      RGB(0x2A, 0x31, 0x3B)
#define SAVEDLG_C_EDGE_HOT  RGB(0x3C, 0x45, 0x52)
#define SAVEDLG_C_TEXT      RGB(0xF0, 0xF3, 0xF7)
#define SAVEDLG_C_SUB       RGB(0xCF, 0xD5, 0xDD)
#define SAVEDLG_C_MUTED     RGB(0x8A, 0x94, 0xA2)
#define SAVEDLG_C_DIM       RGB(0x55, 0x5F, 0x6C)
#define SAVEDLG_C_THUMB_TOP RGB(0x1B, 0x26, 0x38)
#define SAVEDLG_C_THUMB_BOT RGB(0x09, 0x0C, 0x11)
#define SAVEDLG_C_INK       RGB(0x10, 0x13, 0x17)   /* text on an accent fill */
#define SAVEDLG_C_SAVE      RGB(0xFF, 0xAA, 0x3C)
#define SAVEDLG_C_LOAD      RGB(0x4D, 0xB6, 0xFF)
#define SAVEDLG_C_WARN      RGB(0xFF, 0x5C, 0x4D)

#define SAVEDLG_DETAIL_ROWS 3   /* detail lines after the first go two per row */

/* Controller input comes from cellPad's SDL2/XInput snapshot. The preview harness
 * sets this to 0 so a pad in use by the running game cannot answer it. */
#ifndef SAVEDLG_POLL_PAD
#  define SAVEDLG_POLL_PAD 1
#endif
#include "savedlg_input.h"

/* Card text, prepared once when the window opens. */
typedef struct {
    wchar_t slot[48];         /* "SAVEDATA_00" */
    wchar_t stamp[32];        /* savedlg_stamp */
    wchar_t sub[136];         /* SUB_TITLE */
    wchar_t head[200];        /* first DETAIL line, else the subtitle */
    wchar_t line[SAVEDLG_DETAIL_ROWS * 2][160];
    int     nline, sub_in_head;
    HBITMAP thumb;            /* premultiplied BGRA, already at its drawn size */
    int     tw, th, tried;
} savedlg_card;

/* Only one savedata operation is in flight at a time (see the scratch
 * allocator in cellSaveData.c), so the dialog state can be a plain static. */
static struct {
    HWND  wnd;
    const savedlg_row* rows;
    savedlg_card* cards;
    int (*occupied)(const char* path);
    int   nrows, new_row, is_save, done, choice;
    int   pending;            /* row shown the overwrite prompt, -1 none */
    int   sel, top, wheel;    /* selected row; list scroll offset in px */
    int   hot, hot_btn, press_btn, tracking;
    /* layout, client pixels */
    int   em, cw, ch, pad, gap, vpad, card_h, list_y, list_h, foot_y, head_h;
    int   thumb_w, thumb_h, h_small, h_text, h_head, vis;
    RECT  btn[2];             /* 0 OK, 1 Cancel */
    HFONT f_title, f_head, f_text, f_small, f_bold, f_glyph;
    IWICImagingFactory* wic;
} s_savedlg;

/* WIC GUIDs, spelt out so neither the runtime nor the preview harness depends
 * on which import library happens to carry their definitions. */
static const GUID savedlg_clsid_wic = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID savedlg_iid_wic   = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID savedlg_fmt_pbgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

static COLORREF savedlg_accent(void) { return s_savedlg.is_save ? SAVEDLG_C_SAVE : SAVEDLG_C_LOAD; }

/* a + (b - a) * t/256, per channel */
static COLORREF savedlg_mix(COLORREF a, COLORREF b, int t)
{
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / 256,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / 256,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / 256);
}

static const wchar_t* savedlg_title(void)
{
    return s_savedlg.is_save ? L"저장 - 세이브 데이터 선택 / Save - Select Save Data"
                             : L"불러오기 - 세이브 데이터 선택 / Load - Select Save Data";
}

static void savedlg_redraw(void)
{
    if (s_savedlg.wnd) InvalidateRect(s_savedlg.wnd, NULL, FALSE);
}

/* ---------------------------------------------------------------------------
 * Selection, confirm / cancel
 * -----------------------------------------------------------------------*/

static int savedlg_stride(void) { return s_savedlg.card_h + s_savedlg.gap; }

static int savedlg_max_top(void)
{
    int c = s_savedlg.nrows * savedlg_stride() - s_savedlg.gap - s_savedlg.list_h;
    return c > 0 ? c : 0;
}

static void savedlg_set_top(int t)
{
    int m = savedlg_max_top();
    if (t > m) t = m;
    if (t < 0) t = 0;
    if (t != s_savedlg.top) { s_savedlg.top = t; savedlg_redraw(); }
}

/* Scroll just far enough that row i is wholly in view. */
static void savedlg_scroll_to(int i)
{
    int y0 = i * savedlg_stride(), y1 = y0 + s_savedlg.card_h;
    if (y0 < s_savedlg.top) savedlg_set_top(y0);
    else if (y1 > s_savedlg.top + s_savedlg.list_h) savedlg_set_top(y1 - s_savedlg.list_h);
}

/* Move the cursor (clamped). Returns whether it changed rows. */
static int savedlg_select(int i)
{
    if (i >= s_savedlg.nrows) i = s_savedlg.nrows - 1;
    if (i < 0) i = 0;
    int changed = i != s_savedlg.sel;
    s_savedlg.sel = i;
    savedlg_scroll_to(i);
    savedlg_redraw();
    return changed;
}

/* Enter / double-click / OK. A save onto a slot that already holds a save asks
 * first, like the PS3's overwrite prompt; "No" goes back to the list rather
 * than cancelling the whole call. (backup_save_dir still copies the old slot
 * away before savedata_execute touches it.) */
static void savedlg_confirm(void)
{
    int i = s_savedlg.sel;
    if (i < 0 || i >= s_savedlg.nrows) return;
    if (s_savedlg.is_save && s_savedlg.occupied) {
        if (s_savedlg.occupied(s_savedlg.rows[i].path) && s_savedlg.pending != i) {
            /* Ask in the window itself instead of a MessageBox: its own modal
             * loop never reaches the gamepad poll in savedlg_ui_run. The title
             * bar says it too, as it always has. */
            s_savedlg.pending = i;
            SetWindowTextW(s_savedlg.wnd, L"덮어쓸까요? 한 번 더 ○/Enter = 덮어쓰기, ×/Esc = 돌아가기 "
                                          L"/ Overwrite? Confirm again");
            savedlg_redraw();
            return;
        }
    }
    s_savedlg.choice = i;
    s_savedlg.done = 1;
}

static void savedlg_clear_pending(void)
{
    if (s_savedlg.pending < 0) return;
    s_savedlg.pending = -1;
    SetWindowTextW(s_savedlg.wnd, savedlg_title());
    savedlg_redraw();
}

/* Esc / × / Cancel first backs out of a pending overwrite prompt. */
static void savedlg_cancel(void)
{
    if (s_savedlg.pending >= 0) { savedlg_clear_pending(); return; }
    s_savedlg.choice = -1; s_savedlg.done = 1;
}

/* ---------------------------------------------------------------------------
 * Slot data: time stamp, text, thumbnail
 * -----------------------------------------------------------------------*/

/* "YYYY-MM-DD HH:MM" (local time) of the slot's SAVEDATA.BIN -- the payload the
 * game writes every save -- falling back to PARAM.SFO for other titles. */
static void savedlg_stamp(const char* save_path, wchar_t* out, size_t n)
{
    static const char* const probe[] = { "SAVEDATA.BIN", "PARAM.SFO" };
    for (int k = 0; k < 2; k++) {
        char file[1100];
        WIN32_FILE_ATTRIBUTE_DATA a;
        FILETIME lt;
        SYSTEMTIME s;
        snprintf(file, sizeof file, "%s\\%s", save_path, probe[k]);
        if (!GetFileAttributesExA(file, GetFileExInfoStandard, &a)) continue;
        if (!FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt) || !FileTimeToSystemTime(&lt, &s)) continue;
        swprintf(out, n, L"%04u-%02u-%02u %02u:%02u",
                 s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute);
        return;
    }
    swprintf(out, n, L"----------------");
}

static void savedlg_wcopy(wchar_t* dst, size_t n, const wchar_t* src, size_t len)
{
    if (len >= n) len = n - 1;
    memcpy(dst, src, len * sizeof *dst);
    dst[len] = 0;
}

/* UTF-8 -> UTF-16; "" when it does not fit or is not UTF-8. */
static void savedlg_utf8(const char* s, wchar_t* out, int n)
{
    if (!s || !MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) out[0] = 0;
}

static void savedlg_card_text(int i)
{
    savedlg_card* c = &s_savedlg.cards[i];
    const savedlg_row* r = &s_savedlg.rows[i];
    /* "BLJS10184_SAVEDATA_00" -> "SAVEDATA_00": drop the 9-char title id. */
    const char* n = r->dir ? r->dir : "";
    const char* sfx = (strlen(n) > 10 && n[9] == '_') ? n + 10 : n;
    savedlg_utf8(sfx, c->slot, 48);
    if (i == s_savedlg.new_row) {
        swprintf(c->head, 200, L"새 데이터 / New Data");
        swprintf(c->line[0], 160, L"빈 슬롯에 새로 저장합니다 / Save to a new slot");
        c->nline = 1;
        return;
    }
    savedlg_stamp(r->path, c->stamp, 32);
    savedlg_utf8(r->sub, c->sub, 136);

    /* DETAIL is the game's own summary, one fact per line (Macross 30:
     * "●1-1 chapter", "장소: ...", level, money, play time, difficulty). The
     * first line is the card's headline, the rest a two-column grid. */
    wchar_t d[1100];
    savedlg_utf8(r->detail, d, 1100);
    for (wchar_t* p = d; p && *p; ) {
        wchar_t* nl = wcschr(p, L'\n');
        if (nl) *nl = 0;
        size_t len = wcslen(p);
        while (len && (p[len - 1] == L'\r' || p[len - 1] == L' ')) p[--len] = 0;
        if (len) {
            if (!c->head[0]) savedlg_wcopy(c->head, 200, p, len);
            else if (c->nline < SAVEDLG_DETAIL_ROWS * 2) savedlg_wcopy(c->line[c->nline++], 160, p, len);
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!c->head[0] && c->sub[0]) { savedlg_wcopy(c->head, 200, c->sub, wcslen(c->sub)); c->sub_in_head = 1; }
    if (!c->head[0]) savedlg_wcopy(c->head, 200, c->slot, wcslen(c->slot));
}

/* ICON0.PNG of the slot directory, scaled to fit bw x bh (aspect kept) as a
 * premultiplied 32-bit DIB for AlphaBlend. Tried once per card per run; a
 * missing or undecodable icon leaves the placeholder. */
static void savedlg_load_thumb(savedlg_card* c, const char* dir, int bw, int bh)
{
    c->tried = 1;
    IWICImagingFactory* f = s_savedlg.wic;
    if (!f || !dir || bw <= 0 || bh <= 0) return;

    char file[1100];
    wchar_t wfile[1100];
    snprintf(file, sizeof file, "%s\\ICON0.PNG", dir);
    if (GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES) return;
    if (!MultiByteToWideChar(CP_ACP, 0, file, -1, wfile, 1100)) return;

    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* frm = NULL;
    IWICFormatConverter* cvt = NULL;
    IWICBitmapScaler* scl = NULL;
    UINT w = 0, h = 0;
    /* Convert to premultiplied first, then scale: scaling straight alpha
     * bleeds the transparent pixels' colour into the edges. */
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, wfile, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frm)) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize(frm, &w, &h)) && w && h &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cvt)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cvt, (IWICBitmapSource*)frm, &savedlg_fmt_pbgra,
                                                 WICBitmapDitherTypeNone, NULL, 0.0,
                                                 WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &scl))) {
        int tw = bw, th = (int)((long long)h * bw / w);
        if (th > bh) { th = bh; tw = (int)((long long)w * bh / h); }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = tw;
        bi.bmiHeader.biHeight = -th;              /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && bits &&
            SUCCEEDED(IWICBitmapScaler_Initialize(scl, (IWICBitmapSource*)cvt, (UINT)tw, (UINT)th,
                                                  WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(IWICBitmapScaler_CopyPixels(scl, NULL, (UINT)tw * 4, (UINT)(tw * 4 * th), (BYTE*)bits))) {
            c->thumb = bmp; c->tw = tw; c->th = th;
            bmp = NULL;
        }
        if (bmp) DeleteObject(bmp);
    }
    if (scl) IWICBitmapScaler_Release(scl);
    if (cvt) IWICFormatConverter_Release(cvt);
    if (frm) IWICBitmapFrameDecode_Release(frm);
    if (dec) IWICBitmapDecoder_Release(dec);
}

/* ---------------------------------------------------------------------------
 * Painting
 * -----------------------------------------------------------------------*/

static void savedlg_fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void savedlg_frame(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

static void savedlg_round(HDC dc, int x0, int y0, int x1, int y1, int rad,
                          COLORREF fill, COLORREF edge, int pw)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_INSIDEFRAME, pw, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, x0, y0, x1, y1, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void savedlg_vgrad(HDC dc, int x0, int y0, int x1, int y1, COLORREF top, COLORREF bot)
{
    TRIVERTEX v[2];
    memset(v, 0, sizeof v);
    v[0].x = x0; v[0].y = y0;
    v[0].Red = (COLOR16)(GetRValue(top) << 8); v[0].Green = (COLOR16)(GetGValue(top) << 8);
    v[0].Blue = (COLOR16)(GetBValue(top) << 8); v[0].Alpha = 0xFF00;
    v[1].x = x1; v[1].y = y1;
    v[1].Red = (COLOR16)(GetRValue(bot) << 8); v[1].Green = (COLOR16)(GetGValue(bot) << 8);
    v[1].Blue = (COLOR16)(GetBValue(bot) << 8); v[1].Alpha = 0xFF00;
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

static int savedlg_text_w(HDC dc, HFONT f, const wchar_t* s)
{
    SIZE sz = { 0, 0 };
    HGDIOBJ o = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

/* One line of text in [x0,x1) x [y0,y1), vertically centred, ellipsised. */
static void savedlg_text(HDC dc, HFONT f, COLORREF ink, const wchar_t* s,
                         int x0, int y0, int x1, int y1, UINT align)
{
    if (x1 <= x0) return;
    RECT r = { x0, y0, x1, y1 };
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, ink);
    DrawTextW(dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, o);
}

/* Rounded tag; x is its left edge, or its right edge when right != 0.
 * Returns its width. */
static int savedlg_pill(HDC dc, HFONT f, const wchar_t* s, int x, int yc, int right,
                        COLORREF fill, COLORREF ink)
{
    const int em = s_savedlg.em;
    int w = savedlg_text_w(dc, f, s) + em, h = s_savedlg.h_small + em / 8;
    int x0 = right ? x - w : x, y0 = yc - h / 2;
    savedlg_round(dc, x0, y0, x0 + w, y0 + h, h, fill, fill, 1);
    savedlg_text(dc, f, ink, s, x0, y0, x0 + w, y0 + h, DT_CENTER);
    return w;
}

/* "장소: 유리아 군도" -> key muted, value bright. Lines without a short
 * "key:" prefix are drawn as they are. */
static void savedlg_kv(HDC dc, const wchar_t* line, int x0, int y0, int x1, int y1)
{
    const wchar_t* c = wcschr(line, L':');
    if (!c) c = wcschr(line, 0xFF1A);                 /* fullwidth colon */
    if (c && c > line && c - line < 24) {
        wchar_t key[32];
        savedlg_wcopy(key, 32, line, (size_t)(c - line + 1));
        const wchar_t* v = c + 1;
        while (*v == L' ') v++;
        int kw = savedlg_text_w(dc, s_savedlg.f_text, key);
        savedlg_text(dc, s_savedlg.f_text, SAVEDLG_C_MUTED, key, x0, y0, x1, y1, DT_LEFT);
        savedlg_text(dc, s_savedlg.f_text, SAVEDLG_C_SUB, v, x0 + kw + s_savedlg.em / 3, y0, x1, y1, DT_LEFT);
    } else {
        savedlg_text(dc, s_savedlg.f_text, SAVEDLG_C_SUB, line, x0, y0, x1, y1, DT_LEFT);
    }
}

static void savedlg_paint_card(HDC dc, int i, int x0, int y0, int x1, int y1)
{
    savedlg_card* c = &s_savedlg.cards[i];
    const int em = s_savedlg.em;
    const int sel = i == s_savedlg.sel, pend = i == s_savedlg.pending, hot = i == s_savedlg.hot;
    const int isnew = i == s_savedlg.new_row;
    const COLORREF acc = savedlg_accent();

    /* Card body: the selected one is tinted with the accent and outlined in
     * it (red while it waits on the overwrite prompt). */
    COLORREF edge = pend ? SAVEDLG_C_WARN : sel ? acc : hot ? SAVEDLG_C_EDGE_HOT : SAVEDLG_C_EDGE;
    COLORREF fill = sel ? savedlg_mix(SAVEDLG_C_CARD, pend ? SAVEDLG_C_WARN : acc, 30)
                        : hot ? SAVEDLG_C_CARD_HOT : SAVEDLG_C_CARD;
    int bw = (sel || pend) ? (em / 8 > 2 ? em / 8 : 2) : 1;
    savedlg_round(dc, x0, y0, x1, y1, em * 3 / 4, fill, edge, bw);
    if (sel) savedlg_fill(dc, x0 + bw + 2, y0 + em * 3 / 4, x0 + bw + 2 + (em / 5 > 3 ? em / 5 : 3),
                          y1 - em * 3 / 4, pend ? SAVEDLG_C_WARN : acc);

    const int hp = em * 9 / 10, vp = s_savedlg.vpad;
    int tx = x0 + hp, ty = y0 + vp;

    /* Thumbnail panel (absent when WIC is not available). */
    if (s_savedlg.thumb_w > 0) {
        const int tw = s_savedlg.thumb_w, th = s_savedlg.thumb_h, in = em / 6;
        savedlg_vgrad(dc, tx, ty, tx + tw, ty + th, SAVEDLG_C_THUMB_TOP, SAVEDLG_C_THUMB_BOT);
        savedlg_frame(dc, tx, ty, tx + tw, ty + th, sel ? savedlg_mix(SAVEDLG_C_EDGE, acc, 110) : SAVEDLG_C_EDGE);
        if (!isnew && !c->tried) savedlg_load_thumb(c, s_savedlg.rows[i].path, tw - 2 * in, th - 2 * in);
        if (c->thumb) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ o = SelectObject(mem, c->thumb);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, tx + (tw - c->tw) / 2, ty + (th - c->th) / 2, c->tw, c->th,
                       mem, 0, 0, c->tw, c->th, bf);
            SelectObject(mem, o);
            DeleteDC(mem);
        } else {
            /* Placeholder: "+" for New Data, else the slot number. */
            const wchar_t* big = isnew ? L"+" : (wcslen(c->slot) >= 2 ? c->slot + wcslen(c->slot) - 2 : c->slot);
            savedlg_text(dc, s_savedlg.f_glyph, isnew ? acc : SAVEDLG_C_DIM, big,
                         tx, ty, tx + tw, ty + th - s_savedlg.h_small / 2, DT_CENTER);
            savedlg_text(dc, s_savedlg.f_small, SAVEDLG_C_DIM, isnew ? L"NEW DATA" : L"NO IMAGE",
                         tx, ty + th - s_savedlg.h_small - em / 4, tx + tw, ty + th - em / 4, DT_CENTER);
        }
        tx += tw + em;
    }

    /* Text column. */
    const int xr = x1 - hp;
    int y = y0 + vp;
    const int hs = s_savedlg.h_small;

    /* Line 1: slot  ·  subtitle ............ time / tag */
    int right_w = 0;
    if (pend)
        right_w = savedlg_pill(dc, s_savedlg.f_bold, L"덮어쓰기?  Overwrite?", xr, y + hs / 2, 1,
                               SAVEDLG_C_WARN, SAVEDLG_C_INK);
    else if (isnew)
        right_w = savedlg_pill(dc, s_savedlg.f_bold, L"NEW", xr, y + hs / 2, 1, acc, SAVEDLG_C_INK);
    else {
        right_w = savedlg_text_w(dc, s_savedlg.f_small, c->stamp);
        savedlg_text(dc, s_savedlg.f_small, sel ? SAVEDLG_C_SUB : SAVEDLG_C_MUTED, c->stamp,
                     xr - right_w, y, xr, y + hs, DT_RIGHT);
    }
    const int lr = xr - right_w - em / 2;
    int lw = savedlg_text_w(dc, s_savedlg.f_bold, c->slot);
    savedlg_text(dc, s_savedlg.f_bold, sel ? acc : SAVEDLG_C_MUTED, c->slot, tx, y, lr, y + hs, DT_LEFT);
    if (c->sub[0] && !c->sub_in_head) {
        wchar_t s[160];
        swprintf(s, 160, L"·  %ls", c->sub);
        savedlg_text(dc, s_savedlg.f_small, SAVEDLG_C_MUTED, s, tx + lw + em / 2, y, lr, y + hs, DT_LEFT);
    }
    y += hs + em / 5;

    /* Line 2: headline. */
    savedlg_text(dc, s_savedlg.f_head, SAVEDLG_C_TEXT, c->head, tx, y, xr, y + s_savedlg.h_head, DT_LEFT);
    y += s_savedlg.h_head + em / 4;

    /* Detail grid, two facts per row. */
    const int ht = s_savedlg.h_text, colw = (xr - tx - em) / 2;
    for (int k = 0; k < c->nline; k++) {
        int row = c->nline == 1 ? 0 : k / 2, col = c->nline == 1 ? 0 : k % 2;
        int cx0 = tx + col * (colw + em), cx1 = c->nline == 1 ? xr : cx0 + colw;
        int cy = y + row * (ht + em / 8);
        if (isnew) savedlg_text(dc, s_savedlg.f_text, SAVEDLG_C_MUTED, c->line[k], cx0, cy, cx1, cy + ht, DT_LEFT);
        else savedlg_kv(dc, c->line[k], cx0, cy, cx1, cy + ht);
    }
}

static void savedlg_paint(HDC dc)
{
    const int em = s_savedlg.em, cw = s_savedlg.cw, ch = s_savedlg.ch, pad = s_savedlg.pad;
    const COLORREF acc = savedlg_accent();
    const int pend = s_savedlg.pending >= 0;
    SetBkMode(dc, TRANSPARENT);
    savedlg_fill(dc, 0, 0, cw, ch, SAVEDLG_C_BG);

    /* Cards, clipped to the list area. */
    int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, s_savedlg.list_y, cw, s_savedlg.list_y + s_savedlg.list_h);
    const int stride = savedlg_stride();
    for (int i = s_savedlg.top / stride; i < s_savedlg.nrows; i++) {
        int y = s_savedlg.list_y + i * stride - s_savedlg.top;
        if (y >= s_savedlg.list_y + s_savedlg.list_h) break;
        savedlg_paint_card(dc, i, pad, y, cw - pad, y + s_savedlg.card_h);
    }
    RestoreDC(dc, saved);

    /* Scroll indicator in the right margin. */
    const int maxt = savedlg_max_top();
    if (maxt > 0) {
        int w = em / 6 > 3 ? em / 6 : 3, x = cw - pad / 2 - w / 2;
        int content = s_savedlg.nrows * stride - s_savedlg.gap;
        int th = s_savedlg.list_h * s_savedlg.list_h / content;
        if (th < em) th = em;
        int ty = s_savedlg.list_y + (s_savedlg.list_h - th) * s_savedlg.top / maxt;
        savedlg_round(dc, x, s_savedlg.list_y, x + w, s_savedlg.list_y + s_savedlg.list_h, w,
                      SAVEDLG_C_CARD, SAVEDLG_C_CARD, 1);
        savedlg_round(dc, x, ty, x + w, ty + th, w, SAVEDLG_C_EDGE_HOT, SAVEDLG_C_EDGE_HOT, 1);
    }

    /* Header: accent strip, action badge, title, position. */
    const int hh = s_savedlg.head_h;
    savedlg_fill(dc, 0, 0, cw, hh, SAVEDLG_C_BAND);
    savedlg_fill(dc, 0, 0, cw, em / 8 > 2 ? em / 8 : 2, acc);
    savedlg_fill(dc, 0, hh - 1, cw, hh, SAVEDLG_C_EDGE);
    int x = pad, yc = hh / 2 + em / 16;
    x += savedlg_pill(dc, s_savedlg.f_bold, s_savedlg.is_save ? L"저장  SAVE" : L"불러오기  LOAD",
                      x, yc, 0, acc, SAVEDLG_C_INK) + em * 2 / 3;
    const wchar_t* title = L"세이브 데이터 선택";
    int tw = savedlg_text_w(dc, s_savedlg.f_title, title);
    savedlg_text(dc, s_savedlg.f_title, SAVEDLG_C_TEXT, title, x, 0, x + tw + 2, hh, DT_LEFT);
    x += tw + em / 2;
    wchar_t pos[32];
    swprintf(pos, 32, L"%d / %d", s_savedlg.sel + 1, s_savedlg.nrows);
    int pw = savedlg_text_w(dc, s_savedlg.f_small, pos);
    savedlg_text(dc, s_savedlg.f_small, SAVEDLG_C_MUTED, pos, cw - pad - pw, 0, cw - pad, hh, DT_RIGHT);
    savedlg_text(dc, s_savedlg.f_small, SAVEDLG_C_MUTED, L"Select Save Data", x, em / 8, cw - pad - pw - em, hh,
                 DT_LEFT);

    /* Footer: controls (or the overwrite prompt) and the two buttons. */
    const int fy = s_savedlg.foot_y;
    savedlg_fill(dc, 0, fy, cw, ch, SAVEDLG_C_BAND);
    savedlg_fill(dc, 0, fy, cw, fy + 1, SAVEDLG_C_EDGE);
    const int hs = s_savedlg.h_small, mid = (fy + ch) / 2, hx1 = s_savedlg.btn[0].left - em;
    if (pend) {
        savedlg_text(dc, s_savedlg.f_bold, SAVEDLG_C_WARN,
                     L"덮어쓸까요? 한 번 더 ○/Enter = 덮어쓰기, ×/Esc = 돌아가기",
                     pad, mid - hs, hx1, mid, DT_LEFT);
        savedlg_text(dc, s_savedlg.f_small, SAVEDLG_C_MUTED, L"Overwrite? Confirm again",
                     pad, mid, hx1, mid + hs, DT_LEFT);
    } else {
        savedlg_text(dc, s_savedlg.f_small, SAVEDLG_C_SUB,
                     L"↑↓ 선택     ○ / Enter  확인     × / Esc  취소",
                     pad, mid - hs, hx1, mid, DT_LEFT);
        savedlg_text(dc, s_savedlg.f_small, SAVEDLG_C_DIM,
                     L"↑↓ Select     ○ / Enter  OK     × / Esc  Cancel",
                     pad, mid, hx1, mid + hs, DT_LEFT);
    }
    for (int b = 0; b < 2; b++) {
        const RECT* r = &s_savedlg.btn[b];
        const int hotb = s_savedlg.hot_btn == b;
        if (b == 0) {
            COLORREF f = pend ? SAVEDLG_C_WARN : acc;
            if (hotb) f = savedlg_mix(f, RGB(255, 255, 255), 40);
            savedlg_round(dc, r->left, r->top, r->right, r->bottom, em * 2 / 3, f, f, 1);
            savedlg_text(dc, s_savedlg.f_bold, SAVEDLG_C_INK, pend ? L"덮어쓰기 / Overwrite" : L"확인 / OK",
                         r->left, r->top, r->right, r->bottom, DT_CENTER);
        } else {
            savedlg_round(dc, r->left, r->top, r->right, r->bottom, em * 2 / 3,
                          hotb ? SAVEDLG_C_CARD_HOT : SAVEDLG_C_CARD,
                          hotb ? SAVEDLG_C_EDGE_HOT : SAVEDLG_C_EDGE, 1);
            savedlg_text(dc, s_savedlg.f_bold, SAVEDLG_C_TEXT, pend ? L"돌아가기 / Back" : L"취소 / Cancel",
                         r->left, r->top, r->right, r->bottom, DT_CENTER);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Layout
 * -----------------------------------------------------------------------*/

static int savedlg_font_h(HDC dc, HFONT f)
{
    TEXTMETRICW tm;
    HGDIOBJ o = SelectObject(dc, f);
    int h = GetTextMetricsW(dc, &tm) ? tm.tmHeight : 0;
    SelectObject(dc, o);
    return h > 0 ? h : s_savedlg.em;
}

/* Everything is sized off em (1.6x the system message font, as before), and
 * the list shows up to four cards -- fewer if the monitor's work area is too
 * short -- plus a sliver of the next one when there are more to scroll to. */
static void savedlg_layout(HDC dc, int max_cw, int max_ch)
{
    const int em = s_savedlg.em;
    s_savedlg.pad = em;
    s_savedlg.gap = em * 45 / 100;
    s_savedlg.vpad = em * 6 / 10;
    s_savedlg.h_small = savedlg_font_h(dc, s_savedlg.f_small);
    s_savedlg.h_text = savedlg_font_h(dc, s_savedlg.f_text);
    s_savedlg.h_head = savedlg_font_h(dc, s_savedlg.f_head);
    const int content = s_savedlg.h_small + em / 5 + s_savedlg.h_head + em / 4 +
                        SAVEDLG_DETAIL_ROWS * (s_savedlg.h_text + em / 8);
    s_savedlg.card_h = content + 2 * s_savedlg.vpad;
    s_savedlg.thumb_h = content;
    s_savedlg.thumb_w = s_savedlg.wic ? content * 320 / 176 : 0;   /* PS3 ICON0 is 320x176 */
    s_savedlg.head_h = em * 3;
    const int foot_h = em * 3 + em / 4;

    s_savedlg.cw = em * 48;
    if (s_savedlg.cw > max_cw) s_savedlg.cw = max_cw;
    const int stride = savedlg_stride(), n = s_savedlg.nrows;
    int vis = n < 4 ? n : 4, peek = 0;
    for (;;) {
        peek = n > vis ? s_savedlg.card_h / 3 + s_savedlg.gap : 0;
        s_savedlg.list_h = vis * stride - s_savedlg.gap + peek;
        s_savedlg.ch = s_savedlg.head_h + em * 6 / 10 + s_savedlg.list_h + em * 6 / 10 + foot_h;
        if (s_savedlg.ch <= max_ch || vis <= 1) break;
        vis--;
    }
    s_savedlg.vis = vis;
    s_savedlg.list_y = s_savedlg.head_h + em * 6 / 10;
    s_savedlg.foot_y = s_savedlg.ch - foot_h;

    /* Buttons sized for their widest label (the overwrite state's). */
    int bw = savedlg_text_w(dc, s_savedlg.f_bold, L"덮어쓰기 / Overwrite") + em * 2;
    if (bw < em * 7) bw = em * 7;
    const int bh = em * 2, by = s_savedlg.foot_y + (foot_h - bh) / 2;
    const int cx = s_savedlg.cw - s_savedlg.pad;
    SetRect(&s_savedlg.btn[1], cx - bw, by, cx, by + bh);
    SetRect(&s_savedlg.btn[0], cx - 2 * bw - em / 2, by, cx - bw - em / 2, by + bh);
}

/* ---------------------------------------------------------------------------
 * Window
 * -----------------------------------------------------------------------*/

static int savedlg_card_at(int x, int y)
{
    if (y < s_savedlg.list_y || y >= s_savedlg.list_y + s_savedlg.list_h) return -1;
    if (x < s_savedlg.pad || x >= s_savedlg.cw - s_savedlg.pad) return -1;
    const int stride = savedlg_stride(), off = y - s_savedlg.list_y + s_savedlg.top, i = off / stride;
    if (off % stride >= s_savedlg.card_h || i >= s_savedlg.nrows) return -1;
    return i;
}

static int savedlg_btn_at(int x, int y)
{
    POINT p = { x, y };
    for (int b = 0; b < 2; b++)
        if (PtInRect(&s_savedlg.btn[b], p)) return b;
    return -1;
}

static void savedlg_click_btn(int b)
{
    if (b == 0) savedlg_confirm();
    else if (b == 1) savedlg_cancel();
}

static LRESULT CALLBACK savedlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:   /* anything that still sends IDOK / IDCANCEL */
        if (LOWORD(wp) == IDOK) savedlg_confirm();
        else if (LOWORD(wp) == IDCANCEL) savedlg_cancel();
        return 0;
    case WM_CLOSE:
        savedlg_cancel();
        return 0;
    case WM_ACTIVATE:
        /* Keep the keys coming here after any re-activation. */
        if (LOWORD(wp) != WA_INACTIVE) SetFocus(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bb = CreateCompatibleBitmap(dc, s_savedlg.cw, s_savedlg.ch);
        if (mem && bb) {
            HGDIOBJ o = SelectObject(mem, bb);
            savedlg_paint(mem);
            BitBlt(dc, 0, 0, s_savedlg.cw, s_savedlg.ch, mem, 0, 0, SRCCOPY);
            SelectObject(mem, o);
        } else {
            savedlg_paint(dc);
        }
        if (bb) DeleteObject(bb);
        if (mem) DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:   /* PrintWindow (the preview harness) */
        savedlg_paint((HDC)wp);
        return 0;
    case WM_KEYDOWN: {
        /* Enter/Esc normally never get here -- savedlg_ui_run takes them out of
         * the queue first -- but a SendMessage'd key would. */
        int to = -1;
        switch (wp) {
        case VK_RETURN: savedlg_confirm(); return 0;
        case VK_ESCAPE: savedlg_cancel(); return 0;
        case VK_UP:     to = s_savedlg.sel - 1; break;
        case VK_DOWN:   to = s_savedlg.sel + 1; break;
        case VK_PRIOR:  to = s_savedlg.sel - s_savedlg.vis; break;
        case VK_NEXT:   to = s_savedlg.sel + s_savedlg.vis; break;
        case VK_HOME:   to = 0; break;
        case VK_END:    to = s_savedlg.nrows - 1; break;
        default: return 0;
        }
        if (to < 0) to = 0;
        if (savedlg_select(to)) savedlg_clear_pending();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        /* One card per notch; the cursor stays where it is, as in a list box. */
        s_savedlg.wheel += GET_WHEEL_DELTA_WPARAM(wp);
        int steps = s_savedlg.wheel / WHEEL_DELTA;
        s_savedlg.wheel -= steps * WHEEL_DELTA;
        if (steps) savedlg_set_top(s_savedlg.top - steps * savedlg_stride());
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int hot = savedlg_card_at(x, y), hb = savedlg_btn_at(x, y);
        if (hot != s_savedlg.hot || hb != s_savedlg.hot_btn) {
            s_savedlg.hot = hot; s_savedlg.hot_btn = hb;
            savedlg_redraw();
        }
        if (!s_savedlg.tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            s_savedlg.tracking = TrackMouseEvent(&t) ? 1 : 0;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_savedlg.tracking = 0;
        if (s_savedlg.hot >= 0 || s_savedlg.hot_btn >= 0) {
            s_savedlg.hot = s_savedlg.hot_btn = -1;
            savedlg_redraw();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int i = savedlg_card_at(x, y);
        SetFocus(h);
        if (i >= 0) {
            if (savedlg_select(i)) savedlg_clear_pending();
            if (msg == WM_LBUTTONDBLCLK) savedlg_confirm();
        } else if ((s_savedlg.press_btn = savedlg_btn_at(x, y)) >= 0) {
            SetCapture(h);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int b = s_savedlg.press_btn;
        s_savedlg.press_btn = -1;
        if (b >= 0) {
            ReleaseCapture();
            if (savedlg_btn_at((short)LOWORD(lp), (short)HIWORD(lp)) == b) savedlg_click_btn(b);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HFONT savedlg_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
}

static void savedlg_free(void)
{
    HFONT* fonts[] = { &s_savedlg.f_title, &s_savedlg.f_head, &s_savedlg.f_text,
                       &s_savedlg.f_small, &s_savedlg.f_bold, &s_savedlg.f_glyph };
    for (size_t k = 0; k < sizeof fonts / sizeof fonts[0]; k++)
        if (*fonts[k]) { DeleteObject(*fonts[k]); *fonts[k] = NULL; }
    if (s_savedlg.cards) {
        for (int i = 0; i < s_savedlg.nrows; i++)
            if (s_savedlg.cards[i].thumb) DeleteObject(s_savedlg.cards[i].thumb);
        free(s_savedlg.cards);
        s_savedlg.cards = NULL;
    }
    if (s_savedlg.wic) { IWICImagingFactory_Release(s_savedlg.wic); s_savedlg.wic = NULL; }
}

/* Shows the list and returns the chosen row, -1 when the user cancelled, -2
 * when no window could be created (the caller keeps its auto-pick). `def` is
 * the initial cursor. occupied(path) answers "is there a save in this slot?"
 * for the overwrite prompt on a save. */
static int savedlg_ui_run(const savedlg_row* rows, int nrows, int new_row, int is_save, int def,
                          int (*occupied)(const char* path))
{
    static ATOM s_cls;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!rows || nrows <= 0) return -2;
    if (!s_cls) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = savedlg_proc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));   /* IDC_ARROW */
        wc.hbrBackground = NULL;                  /* painted in full by WM_PAINT */
        wc.lpszClassName = SAVEDLG_CLASS;
        s_cls = RegisterClassW(&wc);
        if (!s_cls) return -2;
    }

    HWND prev = GetForegroundWindow();

    /* Size everything off the system message font so it stays legible under
     * DPI scaling -- 1.6x it: the plain one was too small to read comfortably
     * next to a 1080p game window. The face is Malgun Gothic for the Korean. */
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    int em = 20;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0) && ncm.lfMessageFont.lfHeight)
        em = (ncm.lfMessageFont.lfHeight < 0 ? -ncm.lfMessageFont.lfHeight : ncm.lfMessageFont.lfHeight) * 16 / 10;

    memset(&s_savedlg, 0, sizeof s_savedlg);
    s_savedlg.rows = rows; s_savedlg.nrows = nrows; s_savedlg.new_row = new_row;
    s_savedlg.is_save = is_save; s_savedlg.occupied = occupied;
    s_savedlg.choice = -1; s_savedlg.pending = -1;
    s_savedlg.hot = s_savedlg.hot_btn = s_savedlg.press_btn = -1;
    s_savedlg.sel = (def >= 0 && def < nrows) ? def : 0;
    s_savedlg.em = em;
    s_savedlg.cards = (savedlg_card*)calloc((size_t)nrows, sizeof *s_savedlg.cards);
    if (!s_savedlg.cards) { memset(&s_savedlg, 0, sizeof s_savedlg); return -2; }
    s_savedlg.f_title = savedlg_font(em * 115 / 100, FW_BOLD);
    s_savedlg.f_head  = savedlg_font(em * 95 / 100, FW_BOLD);
    s_savedlg.f_text  = savedlg_font(em * 78 / 100, FW_NORMAL);
    s_savedlg.f_small = savedlg_font(em * 70 / 100, FW_NORMAL);
    s_savedlg.f_bold  = savedlg_font(em * 70 / 100, FW_BOLD);
    s_savedlg.f_glyph = savedlg_font(em * 2, FW_BOLD);
    for (int i = 0; i < nrows; i++) savedlg_card_text(i);

    /* WIC for the thumbnails. This is the guest thread: it may already be in
     * an apartment (then use it as is), else join the MTA for the dialog's
     * lifetime. No factory -> text-only cards. */
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(co) || co == RPC_E_CHANGED_MODE) {
        if (FAILED(CoCreateInstance(&savedlg_clsid_wic, NULL, CLSCTX_INPROC_SERVER, &savedlg_iid_wic,
                                    (void**)&s_savedlg.wic)))
            s_savedlg.wic = NULL;
    }

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex    = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    HWND w = CreateWindowExW(ex, SAVEDLG_CLASS, savedlg_title(),
                             style, 0, 0, 100, 100, NULL, NULL, inst, NULL);
    if (!w) {
        savedlg_free();
        if (SUCCEEDED(co)) CoUninitialize();
        memset(&s_savedlg, 0, sizeof s_savedlg);
        return -2;
    }
    s_savedlg.wnd = w;
    {
        /* Dark caption to go with the body (Windows 10 20H1+ / 11; ignored
         * elsewhere): DWMWA_USE_IMMERSIVE_DARK_MODE, DWMWA_CAPTION_COLOR. */
        BOOL dark = TRUE;
        COLORREF cap = SAVEDLG_C_BAND;
        DwmSetWindowAttribute(w, 20, &dark, sizeof dark);
        DwmSetWindowAttribute(w, 35, &cap, sizeof cap);
    }

    /* Centre on the game's monitor, not the primary one, and keep the whole
     * window inside its work area. */
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    HMONITOR mon = MonitorFromWindow(prev, MONITOR_DEFAULTTOPRIMARY);
    int have_mon = mon && GetMonitorInfoW(mon, &mi);
    RECT nc = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&nc, style, FALSE, ex);
    int max_cw = 1 << 20, max_ch = 1 << 20;
    if (have_mon) {
        max_cw = (mi.rcWork.right - mi.rcWork.left) - (nc.right - nc.left) - em;
        max_ch = (mi.rcWork.bottom - mi.rcWork.top) - (nc.bottom - nc.top) - em;
    }
    HDC dc = GetDC(w);
    savedlg_layout(dc, max_cw, max_ch);
    ReleaseDC(w, dc);
    savedlg_scroll_to(s_savedlg.sel);

    RECT r = { 0, 0, s_savedlg.cw, s_savedlg.ch };
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = 100, y = 100;
    if (have_mon) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    }
    SetWindowPos(w, HWND_TOPMOST, x, y, ww, wh, SWP_SHOWWINDOW);
    SetForegroundWindow(w);
    SetFocus(w);

    /* Local modal loop. Enter/Esc are taken here, ahead of dispatch, for the
     * window and anything posted to it (the tools above post WM_KEYDOWN).
     * Arrows, Home/End, PgUp/PgDn reach savedlg_proc through the normal
     * dispatch. */
    /* Use the same mapped controller snapshot as the game (including SDL2
     * DualSense), without the keyboard merge that belongs to the game window.
     * Only a focused dialog accepts pads. Held buttons must first release. */
    MSG m;
    savedlg_input pad_input = {0};
    int quit = 0;
    while (!s_savedlg.done && !quit) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT);
        while (!s_savedlg.done && PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); quit = 1; break; }
            if (m.message == WM_KEYDOWN && (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE) &&
                (m.hwnd == w || IsChild(w, m.hwnd))) {
                if (m.wParam == VK_RETURN) savedlg_confirm(); else savedlg_cancel();
                continue;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (s_savedlg.done || quit) break;

#if SAVEDLG_POLL_PAD
        PadTestInfo pads;
        cellPad_test_info(&pads);
        int devices;
        unsigned btn = savedlg_pad_buttons(&pads, &devices);
        HWND foreground = GetForegroundWindow();
        savedlg_action action = savedlg_input_step(&pad_input, btn, devices,
            foreground == w || IsChild(w, foreground), GetTickCount());
        if (action.move) {
            savedlg_select(s_savedlg.sel + action.move);
            savedlg_clear_pending();
        }
        if (action.confirm) savedlg_confirm();
        else if (action.cancel) savedlg_cancel();
#endif
    }
    int choice = s_savedlg.done ? s_savedlg.choice : -1;
    DestroyWindow(w);
    savedlg_free();
    if (SUCCEEDED(co)) CoUninitialize();
    memset(&s_savedlg, 0, sizeof s_savedlg);
    /* Hand focus back, or the game keeps running with no keyboard. */
    if (prev && IsWindow(prev)) SetForegroundWindow(prev);
    return choice;
}

#endif /* _WIN32 */
#endif /* PS3RECOMP_SAVEDLG_WIN32_H */
