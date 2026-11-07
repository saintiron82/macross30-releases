"""Redraw the Korean screen titles in ttl_menutext_01.dds so they fit the original boxes (menu-title-v10).

F031 part 2: on the controller-type screen the title is two sprites of ttl_topmenu00.ark,
TXOS 31 (操作タイプ, cell 0,55 256x55) and TXOS 35 (選択, cell 2,146 96x55) drawn on one row,
the second one 205 texels to the right of the first (fitted on the user's capture
docs/captures/controller-type-screen-20261028.png: scale 1.86 px/texel, both at y=23). The
English sub-title TXOS 33 (CONTROLLER TYPE) sits right under the title's ink (local y >= ~53).

The Japanese ink keeps to x 8..210, y 63..105 (操作タイプ) and x 7..86, y 153..196 (選択), so
選 starts exactly where プ ends. menu-ui-v7 drew 조작 방식 over x 23..232, y 56..108 and 선택 from
x 4: 선택 covered 방식 and the glyphs reached the sub-title. TXOS 30 (難易度選択, its own title on
the difficulty screen) had the same oversize (x 2..253, y 1..52) and is redrawn the same way.

This script re-renders the three cells (and only those cells) from scratch: ink kept inside the
Japanese ink boxes, a word gap before 선택, and the original's look (white RGB, alpha face with a
top-to-bottom gradient, a dark scanline every third row, a 3-texel down-right extrusion at 1/4
alpha and a faint halo; all measured from the ISO texture). Every pixel outside the three cells,
including the English sub-titles (TXOS 32/33) and the rule (TXOS 34), stays byte-identical.

Offline only: reads the installed menu-ui-v7 replacement and the ISO original, writes a fresh
candidate directory. Never touches active packs, saves or the running game.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
W = H = 256
SOURCE = ROOT / 'localization/image_assets/menu-ui-v7/gallery-title-replacements/ttl_menutext_01.dds'
SOURCE_SHA = 'f159dded56999d144ccc407367ad5a509f916d7ae1aa3febcc56b18236fa61f3'
ORIGINAL = ROOT / 'analysis/localization/menu-atlas-audit/iso/ttl_menutext_01-9e9a243c7e.dds'
ORIGINAL_SHA = '9e9a243c7e34dc29b8bd194faebb74cef4e8c297413719993818ec1d938d32ac'
FONT = Path('C:/Windows/Fonts/NotoSansKR-VF.ttf')
SS = 8
EXTRUDE, EXTRUDE_A = 3, 0.25          # original: 3 px down-right at alpha 64
GRAD_TOP, GRAD_BOTTOM = 0.68, 1.00    # original ~0.55..0.95; Hangul strokes are thinner than the kanji
SCAN_EVERY, SCAN_K = 3, 0.70          # a darker row every third row
HALO_SIGMA, HALO_K = 2.0, 0.12

# cell = TXOS rect (x, y, w, h); face = box the new face ink must fill (abs texel ltrb, inclusive);
# jp_ink = alpha>32 ink box of the ISO original (checked at build time).
LABELS = [
    dict(id='difficulty', txos=30, jp='難易度選択', ko='난이도 선택', cell=(0, 0, 256, 55),
         face=(8, 8, 206, 47), jp_ink=(8, 8, 209, 50)),
    dict(id='controller', txos=31, jp='操作タイプ', ko='조작 방식', cell=(0, 55, 256, 55),
         face=(8, 63, 196, 102), jp_ink=(8, 63, 210, 105)),
    dict(id='select', txos=35, jp='選択', ko='선택', cell=(2, 146, 96, 55),
         face=(11, 153, 84, 193), jp_ink=(7, 153, 87, 196)),
]
# Placement of TXOS 35 relative to TXOS 31 on the controller screen, and of the sub-title TXOS 33.
SELECT_DX = 205
SUBTITLE_AT = (140, 45)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def pixels(raw):
    return np.frombuffer(raw, np.uint8, W * H * 4, 128).reshape(H, W, 4)


def ink_box(alpha, cell, thr=32):
    x, y, w, h = cell
    ys, xs = np.nonzero(alpha[y:y + h, x:x + w] > thr)
    return (int(xs.min() + x), int(ys.min() + y), int(xs.max() + x), int(ys.max() + y))


EM = 48 * SS


def line_image(font, text, track):
    """`text` drawn glyph by glyph with `track` extra font units after each character, cropped to ink."""
    width = int(sum(font.getlength(c) + track for c in text)) + EM
    im = Image.new('L', (width, EM * 2), 0)
    d = ImageDraw.Draw(im)
    x = EM // 2
    for c in text:
        d.text((x, EM // 2), c, font=font, fill=255)
        x += font.getlength(c) + track
    return im.crop(im.getbbox())


def glyphs(text, box, weight):
    """Coverage (float, texel grid of the box) of `text` in box, left-aligned, vertically centred.
    Height fills the box. Too wide: condense to 85%, then shrink. Too narrow (the Japanese title
    was wider): stretch to 106%, then letter-space up to 0.08 em, so the line ends near the box's
    right edge like the original instead of leaving a hole before the next sprite."""
    l, t, r, b = box
    bw, bh = r - l + 1, b - t + 1
    font = ImageFont.truetype(str(FONT), EM)
    font.set_variation_by_axes([weight])
    track = 0
    im = line_image(font, text, track)
    sy = bh * SS / im.height
    sx = sy
    if im.width * sx > bw * SS:
        sx = max(sy * 0.85, bw * SS / im.width)
        if im.width * sx > bw * SS:
            sx = bw * SS / im.width
            sy = sx / 0.85
    else:
        sx = min(sy * 1.06, bw * SS / im.width)
        while im.width * sx < bw * SS and track < EM * 0.08:
            nxt = line_image(font, text, track + EM // 100)
            if nxt.width * sx > bw * SS:
                break
            track += EM // 100
            im = nxt
    gw, gh = max(1, round(im.width * sx)), max(1, round(im.height * sy))
    g = np.asarray(im.resize((gw, gh), Image.LANCZOS), np.float64) / 255
    canvas = np.zeros((bh * SS, bw * SS))
    oy = (bh * SS - gh) // 2
    canvas[oy:oy + gh, 0:gw] = g[:, :min(gw, bw * SS)]
    cov = canvas.reshape(bh, SS, bw, SS).mean(axis=(1, 3))
    return np.clip((cov - 0.5) * 1.15 + 0.5, 0, 1), dict(scale_x=sx / SS, scale_y=sy / SS,
                                                        condense=round(sx / sy, 3),
                                                        tracking_em=round(track / EM, 3))


def render(label, weight):
    """Alpha of the whole cell (cell-sized float array) and a fit report."""
    cx, cy, cw, ch = label['cell']
    l, t, r, b = label['face']
    cov_box, fit = glyphs(label['ko'], label['face'], weight)
    cov = np.zeros((ch, cw))
    cov[t - cy:b - cy + 1, l - cx:r - cx + 1] = cov_box
    ext = np.zeros_like(cov)
    for k in range(1, EXTRUDE + 1):
        ext[k:, k:] = np.maximum(ext[k:, k:], cov[:-k, :-k])
    rows = np.nonzero(cov.max(axis=1) > 0.5)[0]
    top, bottom = rows.min(), rows.max()
    yy = np.arange(ch)[:, None]
    grad = GRAD_TOP + (GRAD_BOTTOM - GRAD_TOP) * np.clip((yy - top) / max(1, bottom - top), 0, 1)
    scan = np.where(((yy - top) % SCAN_EVERY) == SCAN_EVERY - 1, SCAN_K, 1.0)
    face = cov * grad * scan
    body = face + (1 - cov) * EXTRUDE_A * ext
    shape = np.maximum(cov, ext)
    halo = np.asarray(Image.fromarray((shape * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(HALO_SIGMA)),
                      np.float64) / 255 * HALO_K
    a = np.maximum(body, halo)
    # Nothing may touch the cell border (bilinear sampling would bleed into the neighbour cell).
    a[0, :] = a[-1, :] = 0
    a[:, 0] = a[:, -1] = 0
    return a, fit


def build(out, weight):
    src = SOURCE.read_bytes()
    org = ORIGINAL.read_bytes()
    if sha(src) != SOURCE_SHA or sha(org) != ORIGINAL_SHA:
        raise ValueError('Source/original hash changed')
    if src[:128] != org[:128] or len(src) != 128 + W * H * 4:
        raise ValueError('Unexpected DDS layout')
    px_src, px_org = pixels(src), pixels(org)
    result = px_src.copy()
    mask = np.zeros((H, W), np.uint8)
    report = []
    for label in LABELS:
        if ink_box(px_org[:, :, 3], label['cell']) != label['jp_ink']:
            raise ValueError('Japanese ink box changed: ' + label['id'])
        a, fit = render(label, weight)
        cx, cy, cw, ch = label['cell']
        a8 = np.round(a * 255).astype(np.uint8)
        region = result[cy:cy + ch, cx:cx + cw]
        region[:, :, 0:3] = 255
        region[:, :, 3] = a8
        mask[cy:cy + ch, cx:cx + cw] = 1
        new_ink = ink_box(result[:, :, 3], label['cell'])
        jl, jt, jr, jb = label['jp_ink']
        if new_ink[0] < jl or new_ink[1] < jt or new_ink[2] > jr or new_ink[3] > jb:
            raise ValueError(f"Korean ink {new_ink} leaves the Japanese ink box {label['jp_ink']}: {label['id']}")
        report.append(dict(id=label['id'], TXOS_index=label['txos'], source_jp=label['jp'], ko=label['ko'],
                           cell_xywh=list(label['cell']), face_box_ltrb=list(label['face']),
                           jp_ink_ltrb=list(label['jp_ink']), v7_ink_ltrb=list(ink_box(px_src[:, :, 3], label['cell'])),
                           new_ink_ltrb=list(new_ink), fit=fit,
                           changed_pixels=int(np.any(region != px_src[cy:cy + ch, cx:cx + cw], axis=2).sum())))
    outside = int((np.any(result != px_src, axis=2) & (mask == 0)).sum())
    if outside:
        raise ValueError('Pixels changed outside the three cells')
    layout = layout_check(result)
    data = src[:128] + result.tobytes()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'ttl_menutext_01.dds').write_bytes(data)
    (out / 'allowed-pixel-mask.bin').write_bytes(mask.tobytes())
    previews(out, px_org, px_src, result)
    protected = [[0, 110, 103, 27], [103, 110, 153, 27], [0, 137, 256, 2], [98, 146, 158, 110], [0, 201, 98, 55]]
    authority = dict(
        name='ttl_menutext_01.dds',
        source_DDS=str(SOURCE), source_sha256=sha(src),
        ISO_original_DDS=str(ORIGINAL), ISO_original_sha256=sha(org),
        replacement=str((out / 'ttl_menutext_01.dds').resolve()), replacement_sha256=sha(data),
        allowed_pixel_mask=str((out / 'allowed-pixel-mask.bin').resolve()), allowed_mask_sha256=sha(mask.tobytes()),
        ROI_xywh=[list(lb['cell']) for lb in LABELS], protected_xywh=protected, labels=report,
        layout_check=layout, header_changed_bytes=0, outside_mask_RGBA_changed_pixels=outside,
        glyph_source=dict(font=str(FONT), font_sha256=sha(FONT.read_bytes()), weight=weight, supersample=SS,
                          extrude=EXTRUDE, extrude_alpha=EXTRUDE_A, gradient=[GRAD_TOP, GRAD_BOTTOM],
                          scanline=[SCAN_EVERY, SCAN_K], halo=[HALO_SIGMA, HALO_K],
                          note='System Noto Sans KR (OFL) rendered offline; style measured from the ISO texture'),
        layout_refs=['ttl_topmenu00.ark LAYO cont_type_text: TXOS 31 + TXOS 35',
                     'ttl_topmenu00.ark LAYO controller_menu00: cont_title_01 (TXOS 35), cont_title_02 (TXOS 33)',
                     'ttl_topmenu00.ark LAYO difficulty_menu00: diff_title01 (TXOS 30), diff_title02 (TXOS 32)'],
        illustration_modified=False, game_active_modified=False)
    (out / 'authority.json').write_text(json.dumps(authority, ensure_ascii=False, indent=2), encoding='utf-8')
    return authority


def layout_check(px):
    """The controller row as the game composes it (texel units): 선택 must not touch 방식 and no title
    ink may reach the sub-title's ink rows. Returns the measured gaps."""
    a = px[:, :, 3]
    title = a[55:110, 0:256] > 32
    select = a[146:201, 2:98] > 32
    sub = a[110:137, 103:256] > 32
    row = np.zeros((90, 320), bool)
    row[0:55, 0:256] |= title
    t_right = int(np.nonzero(title.any(axis=0))[0].max())
    s_left = SELECT_DX + int(np.nonzero(select.any(axis=0))[0].min())
    sub_top = SUBTITLE_AT[1] + int(np.nonzero(sub.any(axis=1))[0].min())
    t_bottom = int(np.nonzero(title.any(axis=1))[0].max())
    s_bottom = int(np.nonzero(select.any(axis=1))[0].max())
    gap = s_left - t_right - 1
    if gap < 6:
        raise ValueError(f'선택 too close to 방식: gap {gap}')
    if max(t_bottom, s_bottom) >= sub_top - 2:
        raise ValueError('Title ink reaches the sub-title rows')
    return dict(select_dx=SELECT_DX, title_ink_right=t_right, select_ink_left=s_left, word_gap_texels=gap,
                title_ink_bottom=t_bottom, select_ink_bottom=s_bottom, subtitle_ink_top=sub_top,
                note='placement fitted on docs/captures/controller-type-screen-20261028.png (1.86 px/texel)')


def compose(px, bg):
    """Controller title row over a flat background, RGBA texture with straight alpha."""
    canvas = np.zeros((80, 330, 3)) + np.array(bg, float)

    def put(cell, dx, dy):
        x, y, w, h = cell
        c = px[y:y + h, x:x + w].astype(float)
        al = c[:, :, 3:4] / 255
        rgb = c[:, :, [2, 1, 0]]
        tgt = canvas[dy:dy + h, dx:dx + w]
        tgt[:] = tgt * (1 - al) + rgb * al
    put((103, 110, 153, 27), SUBTITLE_AT[0] + 4, SUBTITLE_AT[1] + 4)
    put((0, 55, 256, 55), 4, 4)
    put((2, 146, 96, 55), SELECT_DX + 4, 4)
    return Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8))


def previews(out, org, v7, new):
    """Resource previews (not game captures)."""
    for name, bg in (('sky', (90, 190, 250)), ('dark', (20, 28, 40))):
        tiles = [compose(p, bg).resize((330 * 3, 80 * 3), Image.LANCZOS) for p in (org, v7, new)]
        sheet = Image.new('RGB', (tiles[0].width, tiles[0].height * 3 + 24), (60, 60, 60))
        for i, t in enumerate(tiles):
            sheet.paste(t, (0, i * (t.height + 12)))
        sheet.save(out / f'controller-title-row-jp-v7-v10-{name}3x.png')
    atlas = []
    for p in (org, v7, new):
        im = Image.fromarray(p[0:205, :, [2, 1, 0, 3]].copy(), 'RGBA')
        bg = Image.new('RGBA', im.size, (0, 0, 0, 255))
        bg.alpha_composite(im)
        atlas.append(bg.convert('RGB').resize((im.width * 3, im.height * 3), Image.NEAREST))
    sheet = Image.new('RGB', (atlas[0].width * 3 + 32, atlas[0].height), (60, 60, 60))
    for i, t in enumerate(atlas):
        sheet.paste(t, (i * (t.width + 16), 0))
    sheet.save(out / 'atlas-jp-v7-v10-black3x.png')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=ROOT / 'localization/image_assets/menu-title-v10/candidate')
    p.add_argument('--weight', type=int, default=900)
    a = p.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    auth = build(a.out, a.weight)
    print(json.dumps(dict(replacement_sha256=auth['replacement_sha256'], labels=auth['labels'],
                          layout=auth['layout_check']), ensure_ascii=False))


if __name__ == '__main__':
    main()
