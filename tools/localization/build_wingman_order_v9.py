"""Build the Korean wingman-order HUD labels into hud_cockpit_01.dds (battle-hud-v9).

Offline only: reads the installed v8 replacement (which already carries the
'특수' fix) plus the ISO original, writes a fresh candidate directory. Never
touches active packs, saves or the running game.

Sprites (hud_wingman_order.ark TXOS 9..15, same rects in hud.ark 162..168 and
hud_minimap_obj.ark 221..227):
  9  通常 / NORMAL    -> 일반   (precedent: 通常行動 -> 일반 행동)
  10 集合 / ASSEMBLY  -> 집합   (precedent: 集合 -> 집합)
  11 スキル / SKILL   -> 스킬   (precedent: オートスキル -> 자동 스킬)
  12 SPA / SPECIAL    unchanged (English; earlier bundles keep SPA)
  13 僚機            -> 편대기 (precedent: 僚機 -> 편대기 everywhere)
  14/15 digits 1/2    unchanged
English sub-labels (NORMAL/ASSEMBLY/SKILL), the frame lines and every pixel
outside the reviewed ROIs stay byte-identical. The texture is pure white RGB
with the shape in alpha; the game tints it at draw time.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
W, H = 1280, 720
SOURCE = ROOT / 'localization/image_assets/battle-hud-v8/candidate-v2/hud_cockpit_01.dds'
SOURCE_SHA = '992a392990ef21095539704cb4fc4ebfc3766f323cbc7f3d0e1b773ddd0f9c43'
ORIGINAL = ROOT / 'analysis/localization/battle-special-audit/disc-hud_cockpit_01.dds'
ORIGINAL_SHA = '9ed1c3cd54185b6c089c3cfb96ff48aaf5355b3845648e4e1446ede9f2a1d8a1'
FONT = Path('C:/Windows/Fonts/NotoSansKR-VF.ttf')
SS = 8                      # supersampling factor
GLOW_SIGMA, GLOW_K = 2.0, 0.32   # fitted to the original kanji halo (fit_glow.py)

# roi = (x0, y0, x1, y1) exclusive; ref = column whose alpha is the frame-line glow
# for that row (None: background alpha is 0); box = (cx, cy, max_w, max_h) of the
# new glyph ink in texture pixels (the Japanese ink bbox is measured from the original).
LABELS = [
    dict(id='normal', sprite=9, jp='通常', ko='일반', roi=(1168, 487, 1267, 505), ref=1170,
         box=(1220.5, 495.5, 38, 17)),
    dict(id='assembly', sprite=10, jp='集合', ko='집합', roi=(1168, 527, 1267, 547), ref=1170,
         box=(1220.5, 536.5, 38, 18)),
    dict(id='skill', sprite=11, jp='スキル', ko='스킬', roi=(1168, 567, 1267, 586), ref=1170,
         box=(1220.5, 576.0, 38, 18)),
    dict(id='wingman', sprite=13, jp='僚機', ko='편대기', roi=(1159, 641, 1198, 664), ref=None,
         box=(1178.0, 652.5, 35, 14)),
]
# Never touched (verified byte-identical): SPA sprite, digits, ROIs of the v8 特殊 fix.
PROTECTED = [(1157, 600, 122, 39), (1200, 640, 22, 26), (1225, 640, 22, 26), (858, 358, 25, 52)]


def sha(b):
    return hashlib.sha256(b).hexdigest()


def alpha(raw):
    return np.frombuffer(raw, np.uint8, W * H * 4, 128).reshape(H, W, 4)[:, :, 3].astype(np.float64) / 255


def ink(text, max_w, max_h, weight):
    """Supersampled coverage of `text`, scaled to fit max_w x max_h (aspect kept,
    horizontal condensing up to 12% allowed). Returns float array at SS scale."""
    font = ImageFont.truetype(str(FONT), 40 * SS)
    font.set_variation_by_axes([weight])
    l, t, r, b = font.getbbox(text)
    im = Image.new('L', (r - l + 8, b - t + 8), 0)
    ImageDraw.Draw(im).text((4 - l, 4 - t), text, font=font, fill=255)
    bb = im.getbbox()
    im = im.crop(bb)
    sh = max_h * SS / im.height
    sw = min(sh, max_w * SS / im.width)
    if sw < sh * 0.88:
        sh = max_w * SS / im.width / 0.88
        sw = max_w * SS / im.width
    size = (max(1, round(im.width * sw)), max(1, round(im.height * sh)))
    return np.asarray(im.resize(size, Image.LANCZOS), np.float64) / 255


def render_label(label, base_alpha, weight):
    x0, y0, x1, y1 = label['roi']
    cx, cy, mw, mh = label['box']
    glyph = ink(label['ko'], mw, mh, weight)
    canvas = np.zeros(((y1 - y0) * SS, (x1 - x0) * SS))
    gx = int(round((cx - x0) * SS - glyph.shape[1] / 2))
    gy = int(round((cy - y0) * SS - glyph.shape[0] / 2))
    if gx < 0 or gy < 0 or gx + glyph.shape[1] > canvas.shape[1] or gy + glyph.shape[0] > canvas.shape[0]:
        raise ValueError('Glyph does not fit ROI: ' + label['id'])
    canvas[gy:gy + glyph.shape[0], gx:gx + glyph.shape[1]] = glyph
    cov = canvas.reshape(y1 - y0, SS, x1 - x0, SS).mean(axis=(1, 3))
    cov = np.clip((cov - 0.5) * 1.15 + 0.5, 0, 1)  # crisper edges like the original
    blur = np.asarray(Image.fromarray((cov * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(GLOW_SIGMA)),
                      np.float64) / 255
    text = 1 - (1 - cov) * (1 - GLOW_K * blur)
    # Background under the removed Japanese ink: the frame-line glow, which is
    # constant along a row (the reference column). Pixels well away from the
    # Japanese ink keep their original alpha exactly, so the line gap that holds
    # the English sub-label stays as it was.
    if label['ref'] is None:
        rowconst = np.zeros_like(text)
    else:
        rowconst = np.repeat(base_alpha[y0:y1, label['ref']:label['ref'] + 1], x1 - x0, axis=1)
    ys, xs = np.nonzero(base_alpha[y0:y1, x0:x1] - rowconst > 0.25)
    jx0, jy0, jx1, jy1 = xs.min() + x0, ys.min() + y0, xs.max() + x0, ys.max() + y0
    label['jp_ink_measured'] = [int(jx0), int(jy0), int(jx1), int(jy1)]
    near = np.zeros_like(text, bool)
    near[max(0, jy0 - 8 - y0):jy1 + 9 - y0, max(0, jx0 - 8 - x0):jx1 + 9 - x0] = True
    bg = np.where(near, rowconst, base_alpha[y0:y1, x0:x1])
    return np.maximum(bg, text), rowconst, near


def build(out, weight):
    src = SOURCE.read_bytes()
    org = ORIGINAL.read_bytes()
    if sha(src) != SOURCE_SHA or sha(org) != ORIGINAL_SHA:
        raise ValueError('Source/original hash changed')
    if src[:128] != org[:128] or len(src) != 128 + W * H * 4:
        raise ValueError('Unexpected DDS layout')
    a_src = alpha(src)
    a_org = alpha(org)
    px_src = np.frombuffer(src, np.uint8, W * H * 4, 128).reshape(H, W, 4)
    px_org = np.frombuffer(org, np.uint8, W * H * 4, 128).reshape(H, W, 4)
    result = px_src.copy()
    mask = np.zeros((H, W), np.uint8)
    report = []
    for label in LABELS:
        x0, y0, x1, y1 = label['roi']
        if not np.array_equal(px_src[y0:y1, x0:x1], px_org[y0:y1, x0:x1]):
            raise ValueError('v8 source differs from ISO original inside ROI ' + label['id'])
        new, rowconst, near = render_label(label, a_src, weight)
        # Model check: away from the Japanese ink the original equals the row
        # background except where the bottom line has its English gap (<= 8/255).
        orig = np.round(a_org[y0:y1, x0:x1] * 255)
        dev = np.abs(orig - np.round(rowconst * 255))[~near]
        if dev.size and dev.max() > 8:
            raise ValueError('Background model mismatch in ' + label['id'])
        a8 = np.round(new * 255).astype(np.uint8)
        region = result[y0:y1, x0:x1]
        changed = a8 != region[:, :, 3]
        region[changed, 0:3] = 255
        region[changed, 3] = a8[changed]
        mask[y0:y1, x0:x1] = 1
        report.append(dict(id=label['id'], sprite=label['sprite'], source_jp=label['jp'], ko=label['ko'],
                           ROI_xywh=[x0, y0, x1 - x0, y1 - y0], jp_ink_ltrb=label['jp_ink_measured'], changed_pixels=int(changed.sum())))
    for x, y, w, h in PROTECTED:
        if not np.array_equal(result[y:y + h, x:x + w], px_src[y:y + h, x:x + w]):
            raise ValueError('Protected region changed')
    outside = (np.any(result != px_src, axis=2) & (mask == 0)).sum()
    if outside:
        raise ValueError('Pixels changed outside mask')
    data = src[:128] + result.tobytes()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'hud_cockpit_01.dds').write_bytes(data)
    (out / 'allowed-pixel-mask.bin').write_bytes(mask.tobytes())
    previews(out, px_org, px_src, result)
    authority = dict(
        name='hud_cockpit_01.dds',
        source_DDS=str(SOURCE), source_sha256=sha(src),
        ISO_original_DDS=str(ORIGINAL), ISO_original_sha256=sha(org),
        replacement=str((out / 'hud_cockpit_01.dds').resolve()), replacement_sha256=sha(data),
        allowed_pixel_mask=str((out / 'allowed-pixel-mask.bin').resolve()), allowed_mask_sha256=sha(mask.tobytes()),
        ROI_xywh=[r['ROI_xywh'] for r in report], protected_xywh=PROTECTED, labels=report,
        header_changed_bytes=0, outside_mask_RGBA_changed_pixels=int(outside),
        glyph_source=dict(font=str(FONT), font_sha256=sha(FONT.read_bytes()), weight=weight, supersample=SS,
                          glow_sigma=GLOW_SIGMA, glow_k=GLOW_K,
                          note='System Noto Sans KR (OFL) rendered offline; halo fitted to the original kanji'),
        layout_refs=['hud_wingman_order.ark TXOS 9-15', 'hud.ark TXOS 162-168', 'hud_minimap_obj.ark TXOS 221-227'],
        illustration_modified=False, game_active_modified=False)
    (out / 'authority.json').write_text(json.dumps(authority, ensure_ascii=False, indent=2), encoding='utf-8')
    (out / 'extra-manifest.json').write_text(json.dumps(dict(assets=[authority], nested_entries=[]),
                                                        ensure_ascii=False, indent=2), encoding='utf-8')
    return authority


def previews(out, org, src, res):
    """Resource previews (not game captures): crops on black, white and HUD-yellow tint."""
    crop = (1150, 470, 1280, 672)

    def tile(px, tint):
        im = Image.fromarray(px[crop[1]:crop[3], crop[0]:crop[2]][:, :, [2, 1, 0, 3]].copy(), 'RGBA')
        r, g, b, a = im.split()
        im = Image.merge('RGBA', (r.point(lambda v: v * tint[0] // 255), g.point(lambda v: v * tint[1] // 255),
                                  b.point(lambda v: v * tint[2] // 255), a))
        bg = Image.new('RGBA', im.size, (12, 16, 20, 255))
        bg.alpha_composite(im)
        return bg.convert('RGB').resize((im.width * 4, im.height * 4), Image.NEAREST)

    for tint, name in (((255, 255, 255), 'white'), ((236, 244, 40), 'yellow')):
        a, b = tile(org, tint), tile(res, tint)
        sheet = Image.new('RGB', (a.width * 2 + 16, a.height), (60, 60, 60))
        sheet.paste(a, (0, 0))
        sheet.paste(b, (a.width + 16, 0))
        sheet.save(out / f'wingman-order-before-after-{name}4x.png')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=ROOT / 'localization/image_assets/battle-hud-v9/candidate')
    p.add_argument('--weight', type=int, default=900)
    a = p.parse_args()
    auth = build(a.out, a.weight)
    print(json.dumps(dict(replacement_sha256=auth['replacement_sha256'], labels=auth['labels']), ensure_ascii=False))


if __name__ == '__main__':
    main()
