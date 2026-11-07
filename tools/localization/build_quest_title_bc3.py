"""DDS codec only: recompress GDI-composited title within reviewed BC3 blocks."""
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from build_korean_font import alpha_palette, decode_alpha


def rgb565(n):
    return ((n >> 11) * 255 // 31, ((n >> 5) & 63) * 255 // 63, (n & 31) * 255 // 31)


def pack565(c):
    return (round(c[0] * 31 / 255) << 11) | (round(c[1] * 63 / 255) << 5) | round(c[2] * 31 / 255)


def colors(a, b):
    x, y = rgb565(a), rgb565(b)
    return [x, y, tuple((2 * u + v) // 3 for u, v in zip(x, y)),
            tuple((u + 2 * v) // 3 for u, v in zip(x, y))]


def decode(block):
    a = decode_alpha(block)
    c0, c1, bits = struct.unpack_from('<HHI', block, 8)
    palette = colors(c0, c1)
    return [(*palette[(bits >> (2 * i)) & 3], a[i]) for i in range(16)]


def encode(pixels):
    alphas = [c[3] for c in pixels]
    hi, lo = max(alphas), min(alphas)
    palette = alpha_palette(hi, lo)
    indices = [min(range(8), key=lambda i: abs(palette[i] - v)) for v in alphas]
    abits = sum(v << (3 * i) for i, v in enumerate(indices))
    visible = [p[:3] for p in pixels if p[3]]
    # Fit visible colors only; hidden RGB must not darken the glyph edges.
    c0 = pack565(tuple(max(p[i] for p in visible) for i in range(3))) if visible else 0
    c1 = pack565(tuple(min(p[i] for p in visible) for i in range(3))) if visible else 0
    palette = colors(c0, c1)
    ci = [min(range(4), key=lambda i: sum((palette[i][k] - p[k]) ** 2 for k in range(3)))
          if p[3] else 0 for p in pixels]
    return bytes([hi, lo]) + abits.to_bytes(6, 'little') + struct.pack('<HHI', c0, c1, sum(v << (2 * i) for i, v in enumerate(ci)))


def png(path, pixels, w, h):
    def chunk(k, data):
        return struct.pack('>I', len(data)) + k + data + struct.pack('>I', zlib.crc32(k + data) & 0xffffffff)
    raw = bytes(v for p in pixels for v in p)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(b''.join(b'\0' + raw[y*w*4:(y+1)*w*4] for y in range(h)))) + chunk(b'IEND', b''))


def main():
    for color in [(255, 255, 255, 255), (255, 190, 0, 255), (0, 0, 0, 0)]:
        restored = decode(encode([color] * 16))
        assert all(p[3] == color[3] and (not color[3] or max(abs(p[k]-color[k]) for k in range(3)) <= 4) for p in restored)
    ramp = [(255, 190, 0, i * 17) for i in range(16)]
    assert max(abs(a[3]-b[3]) for a,b in zip(ramp,decode(encode(ramp)))) <= 18
    source = Path('native/analysis/quest-title-sources/quest_title_01_02.dds')
    dest = Path('localization/image_assets/menu-ui-v2/replacements')
    original = source.read_bytes()
    assert hashlib.sha256(original).hexdigest() == '7d328ba39c42e93bd75797e324b7ea7e8cfac4c2fe80f1f574172dfa1d6894fd'
    assert original[84:88] == b'DXT5' and struct.unpack_from('<II', original, 12) == (96,640) and len(original) == 61568
    raw = (dest/'quest_title_01_02.rgba').read_bytes()
    assert len(raw) == 640*96*4
    result = bytearray(original); changed = set()
    for by in range(8//4,88//4):
        for bx in range(176//4,472//4):
            pixels = [tuple(raw[((by*4+y)*640+bx*4+x)*4:((by*4+y)*640+bx*4+x)*4+4]) for y in range(4) for x in range(4)]
            pos = 128+(by*160+bx)*16; result[pos:pos+16] = encode(pixels); changed.add(pos)
    assert original[:128] == result[:128]
    assert all(original[p:p+16] == result[p:p+16] for p in range(128,len(original),16) if p not in changed)
    pixels = [(0,0,0,0)]*(640*96)
    for by in range(24):
        for bx in range(160):
            block = decode(result[128+(by*160+bx)*16:128+(by*160+bx+1)*16])
            for i,p in enumerate(block): pixels[(by*4+i//4)*640+bx*4+i%4] = p
    target=dest/'quest_title_01_02.dds';target.write_bytes(result)
    png(dest/'quest_title_01_02.png',pixels,640,96)
    subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',
                    'tools/localization/preview_quest_title.ps1','-Source',str(dest/'quest_title_01_02.png'),
                    '-Output',str(dest/'quest_title_01_02-blackpreview.png')],check=True)
    doc=dict(source=str(source),output=str(target),source_sha256=hashlib.sha256(original).hexdigest(),sha256=hashlib.sha256(result).hexdigest(),width=640,height=96,format='BC3/DXT5 single mip',text='밴디트',source_roi_xyxy=[176,10,469,85],changed_block_roi_xyxy=[176,8,472,88],recompressed_blocks=len(changed),outside_block_changed_bytes=0,header_changed_bytes=0,codec_checks=['constant white','constant orange RGB565 error <=4','constant transparent','alpha ramp error<=18'],gdi=json.loads((dest/'gdi-title-roi.json').read_text()),active_modified=False,in_game_verified=False)
    provenance = json.loads((source.parent/'manifest.json').read_text(encoding='utf8'))
    doc['original_pidxtuple'] = next({k:r[k] for k in ('path','pack','offset','size','compressed_size')} for r in provenance['titles'] if r['quest_id']=='1-2')
    doc['glyph_source'] = 'localization/image_assets/menu-ui-v2/source-glyphs.png'
    doc['glyph_source_sha256'] = hashlib.sha256(Path(doc['glyph_source']).read_bytes()).hexdigest()
    (dest/'quest_title_01_02.manifest.json').write_text(json.dumps(doc,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(doc,ensure_ascii=True))


if __name__ == '__main__':main()
