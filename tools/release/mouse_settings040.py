"""Move the complete mouse-control group into key/mouse bindings."""
from frame_limit040 import replace_once


def move_sensitivity(text):
    start=text.index('        else if (id == ID_OPT_MOUSE_X || id == ID_OPT_MOUSE_Y) {')
    end=text.index('        else if (id == ID_OPT_CLOSE)',start)
    block=text[start:end]
    text=text[:start]+text[end:]
    block=block.replace('        else if ', '        if ',1)
    block=block.replace('ID_OPT_MOUSE_X','ID_KEY_MOUSE_X').replace('ID_OPT_MOUSE_Y','ID_KEY_MOUSE_Y')
    block=replace_once(block,'            int *axis =', '            if (s_key_capture >= 0) null_key_finish(-1);\n            int *axis =')
    block=replace_once(block,'            null_mouse_apply();', '            null_mouse_apply();\n            null_settings_save();\n            return 0;')
    start=text.index('static LRESULT CALLBACK null_key_wndproc')
    pos=text.index('        const int id = LOWORD(wp);',start)
    text=text[:pos]+text[pos:].replace('        const int id = LOWORD(wp);',
        '        const int id = LOWORD(wp), code = HIWORD(wp);\n'+block,1)
    text=replace_once(text,'ID_KEY_CLOSE = 291, ID_KEY_TIMER = 1',
        'ID_KEY_CLOSE = 291, ID_KEY_MOUSE_X = 292, ID_KEY_MOUSE_Y = 293, ID_KEY_TIMER = 1')
    text=replace_once(text,', ID_OPT_MOUSE_X, ID_OPT_MOUSE_Y };',' };')
    text=replace_once(text,'const int kw = 860, kh = 40 + rows * rh + 110;',
        'const int kw = 860, kh = 40 + rows * rh + 180;')
    start=text.index('    OPT_CTL(L"STATIC", macross_ui_string("mouse.sensitivity_x")')
    end=text.index('    OPT_CTL(L"STATIC", macross_ui_string("mouse.capture_help")',start)
    controls=text[start:end]
    text=text[:start]+text[end:]
    controls=controls.replace('OPT_CTL','KEY_CTL').replace('ID_OPT_MOUSE_X','ID_KEY_MOUSE_X').replace('ID_OPT_MOUSE_Y','ID_KEY_MOUSE_Y')
    controls=controls.replace(', 20, 550, 195, 22,', ', 16, by + 52, 200, 22,')
    controls=controls.replace(', 230, 550, 195, 22,', ', 436, by + 52, 200, 22,')
    controls=controls.replace(', 20, 573, 190, 200,', ', 226, by + 48, 180, 200,')
    controls=controls.replace(', 230, 573, 190, 200,', ', 646, by + 48, 178, 200,')
    controls=controls.replace(', 20, 607, 420, 23,', ', 16, by + 84, 820, 23,')
    marker='    KEY_CTL(L"BUTTON", macross_ui_string("common.defaults")'
    text=replace_once(text,marker,controls+marker)
    text=text.replace('by + 50, 140, 30, ID_KEY_DEFAULTS','by + 120, 140, 30, ID_KEY_DEFAULTS')
    text=text.replace('by + 50, 100, 30, ID_KEY_CLOSE','by + 120, 100, 30, ID_KEY_CLOSE')
    text=replace_once(text,'const int ow = 470, oh = 700;', 'const int ow = 470, oh = 620;')
    text=text.replace('"mouse.capture_help"), 0, 20, 630,', '"mouse.capture_help"), 0, 20, 550,')
    marker='        if (s_opt_hwnd && IsDialogMessageW(s_opt_hwnd, &msg)) continue;'
    text=replace_once(text,marker,'        if (s_key_hwnd && IsDialogMessageW(s_key_hwnd, &msg)) continue;\n'+marker)
    # Keep enable/inversion and the cursor hint with the sensitivity controls.
    start=text.index('        else if (id == ID_OPT_MOUSE && code == BN_CLICKED)')
    end=text.index('        else if (id == ID_OPT_CLOSE)',start)
    handler=text[start:end].replace('        else if ', '            if ',1)
    text=text[:start]+text[end:]
    handler=handler.replace('ID_OPT_MOUSE_INVERT','ID_KEY_MOUSE_INVERT').replace('ID_OPT_MOUSE','ID_KEY_MOUSE')
    wrapper='''        if (id == ID_KEY_MOUSE || id == ID_KEY_MOUSE_INVERT) {
            if (code != BN_CLICKED) return 0;
            if (s_key_capture >= 0) null_key_finish(-1);
            const int checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
'''+handler+'''            null_settings_save();
            return 0;
        }
'''
    marker='        if (id >= ID_KEY_BASE && id < ID_KEY_BASE + PAD_KM_COUNT)'
    text=replace_once(text,marker,wrapper+marker)
    text=replace_once(text,'ID_KEY_MOUSE_Y = 293, ID_KEY_TIMER',
        'ID_KEY_MOUSE_Y = 293, ID_KEY_MOUSE = 294, ID_KEY_MOUSE_INVERT = 295, ID_KEY_TIMER')
    text=replace_once(text,', ID_OPT_MOUSE, ID_OPT_MOUSE_INVERT };',' };')
    text=text.replace(' || id == ID_OPT_MOUSE || id == ID_OPT_MOUSE_INVERT','')
    start=text.index('    OPT_CTL(L"BUTTON", macross_ui_string("mouse.enable")')
    end=text.index('#undef OPT_CTL',start)
    group=text[start:end].replace('OPT_CTL','KEY_CTL')
    text=text[:start]+text[end:]
    group=group.replace('ID_OPT_MOUSE_INVERT','ID_KEY_MOUSE_INVERT').replace('ID_OPT_MOUSE','ID_KEY_MOUSE')
    group=group.replace(', 20, 520, 230, 24,', ', 16, by + 50, 300, 24,')
    group=group.replace(', 245, 520, 180, 24,', ', 436, by + 50, 300, 24,')
    group=group.replace(', 20, 550, 420, 23,', ', 16, by + 140, 820, 23,')
    hint=group.index('    KEY_CTL(L"STATIC", macross_ui_string("mouse.capture_help")')
    toggles,hint=group[:hint],group[hint:]
    # Move existing sensitivity rows down by one checkbox row.
    text=text.replace('by + 52, 200, 22,','by + 84, 200, 22,')
    text=text.replace('by + 48, 180, 200,','by + 80, 180, 200,').replace('by + 48, 178, 200,','by + 80, 178, 200,')
    text=text.replace('by + 84, 820, 23,','by + 116, 820, 23,')
    marker='    KEY_CTL(L"STATIC", macross_ui_string("mouse.sensitivity_x")'
    text=replace_once(text,marker,toggles+marker)
    marker='    KEY_CTL(L"BUTTON", macross_ui_string("common.defaults")'
    text=replace_once(text,marker,hint+marker)
    text=text.replace('by + 120, 140, 30,','by + 174, 140, 30,').replace('by + 120, 100, 30,','by + 174, 100, 30,')
    text=replace_once(text,'kh = 40 + rows * rh + 180;', 'kh = 40 + rows * rh + 240;')
    text=replace_once(text,'const int ow = 470, oh = 620;', 'const int ow = 470, oh = 560;')
    assert 'ID_OPT_MOUSE' not in text
    assert 'ID_OPT_MOUSE_X' not in text and 'ID_OPT_MOUSE_Y' not in text
    return text
