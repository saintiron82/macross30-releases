"""Apply the 30/60/unlimited selection to the isolated merged release sources."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(text, old, new):
    assert text.count(old) == 1, old[:100]
    return text.replace(old, new, 1)


def options_source(text):
    text = replace_once(text, '#include "gameplay_rumble.h"', '#include "gameplay_rumble.h"\n#include "macross_fps_limit.h"')
    text = text.replace('vsync, fps_unlock, bloom', 'vsync, fps_limit, bloom')
    text = replace_once(text, 's_set = { 2, 0, 1, 0, 1, 1, 1, 0, 1, 100 }', 's_set = { 2, 0, 1, 30, 1, 1, 1, 0, 1, 100 }')
    text = text.replace('s_set.fps_unlock', 's_set.fps_limit')
    text = replace_once(text, 'void rsx_null_backend_set_fps_callback(int (*apply)(int)) { s_fps_apply = apply; }', '''void rsx_null_backend_set_fps_callback(int (*apply)(int)) { s_fps_apply = apply; }
static const int k_fps_limits[] = {30, 60, 0};
static int null_fps_limit_index(int limit) { return limit == 60 ? 1 : limit == 0 ? 2 : 0; }
static int null_apply_fps_limit(int limit)
{
    return s_fps_apply ? macross_fps_mode_to_limit(s_fps_apply(macross_fps_limit_to_mode(limit))) : limit;
}''')
    text = replace_once(text, '    if (gate) s_set.fps_limit = !strcmp(gate, "1");', '''    s_set.fps_limit = gate && !strcmp(gate, "1") ? 0 : 30;
    macross_fps_limit_parse(getenv("MACROSS_FPS_LIMIT"), &s_set.fps_limit);
    int fps_limit_seen = 0;''')
    text = replace_once(text, 'else if (!strcmp(key, "fps_unlock")) s_set.fps_limit = atoi(val) != 0;', '''else if (!strcmp(key, "fps_limit")) {
            int limit;
            if (macross_fps_limit_parse(val, &limit)) { s_set.fps_limit = limit; fps_limit_seen = 1; }
        }
        else if (!strcmp(key, "fps_unlock") && !fps_limit_seen && (!strcmp(val,"0") || !strcmp(val,"1")))
            s_set.fps_limit = val[0] == '1' ? 0 : 30;''')
    text = replace_once(text, 'vsync=%d\\nfps_unlock=%d\\nbloom=%d', 'vsync=%d\\nfps_limit=%d\\nfps_unlock=%d\\nbloom=%d')
    text = replace_once(text, 's_set.vsync, s_set.fps_limit, s_set.bloom,', 's_set.vsync, s_set.fps_limit, s_set.fps_limit == 0, s_set.bloom,')
    text = text.replace('id <= ID_OPT_UNLOCK)', 'id <= ID_OPT_BLOOM)')
    text = replace_once(text, '''else if (id == ID_OPT_UNLOCK && code == BN_CLICKED) {
            s_set.fps_limit = s_fps_apply ? s_fps_apply(checked) : checked;
            SendMessageW((HWND)lp, BM_SETCHECK, s_set.fps_limit ? BST_CHECKED : BST_UNCHECKED, 0);
        }''', '''else if (id == ID_OPT_UNLOCK && code == CBN_SELCHANGE) {
            const LRESULT selected = SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected < 3) s_set.fps_limit = null_apply_fps_limit(k_fps_limits[selected]);
            SendMessageW((HWND)lp, CB_SETCURSEL, null_fps_limit_index(s_set.fps_limit), 0);
        }''')
    start = text.index('#if defined(MACROSS_RELEASE_FPS_LOCKED)', text.index('static void null_open_options'))
    end = text.index('#endif', start) + len('#endif')
    text = text[:start] + '''    OPT_CTL(L"STATIC", macross_ui_string("options.fps_limit"), 0, 20, 208, 200, 24, 0)
    OPT_CTL(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 230, 204, 200, 120, ID_OPT_UNLOCK)
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"30 FPS");
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"60 FPS");
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)macross_ui_string("options.fps_limit.unlimited"));
    SendMessageW(c, CB_SETCURSEL, null_fps_limit_index(s_set.fps_limit), 0);''' + text[end:]
    text = replace_once(text, 'if (s_fps_apply) s_set.fps_limit = s_fps_apply(s_set.fps_limit);', 's_set.fps_limit = null_apply_fps_limit(s_set.fps_limit);')
    assert 's_set.fps_unlock' not in text
    return text


def stage(out, parent):
    path = out/'rsx_null_backend.c'
    path.write_text(options_source(path.read_text('utf8')), encoding='utf8')
    original = (parent/'frame_pacing.cpp').read_bytes()
    assert hashlib.sha256(original).hexdigest() == '7093130164eb7e51f775912594ebb23490401baf33615af7a3ab0946090447eb'
    text = original.decode('utf8')
    text = replace_once(text, '#include <atomic>', '#include <atomic>\n#include "macross_fps_limit.h"')
    text = text.replace('std::atomic<bool> fps_unlocked{false};', 'std::atomic<int> fps_mode{MACROSS_FPS_30};')
    text = text.replace('return fps_unlocked.load(std::memory_order_acquire) ? 0u : 2u;', 'return macross_fps_mode_vblanks(fps_mode.load(std::memory_order_acquire));')
    text = replace_once(text, '    const char* dt =', '''    const int requested_mode = unlocked == MACROSS_FPS_UNLIMITED || unlocked == MACROSS_FPS_60 ? unlocked : MACROSS_FPS_30;
    const char* dt =''')
    text = text.replace('if (unlocked && !original)', 'if (requested_mode != MACROSS_FPS_30 && !original)')
    start = text.index('    const bool enabled =')
    text = text[:start] + '''    const int enabled = !original && compatible ? requested_mode : MACROSS_FPS_30;
    fps_mode.store(enabled, std::memory_order_release);
    std::fprintf(stderr, "[runtime-options] fps_limit=%d effective_vblanks=%u measured=%d compatible=%d\\n",
                 macross_fps_mode_to_limit(enabled), macross_fps_mode_vblanks(enabled), int(!original), int(compatible));
    return enabled;
}
'''
    (out/'frame_pacing.cpp').write_text(text,encoding='utf8')
    (out/'macross_fps_limit.h').write_bytes((ROOT/'vendor/ps3recomp-master/libs/video/macross_fps_limit.h').read_bytes())
