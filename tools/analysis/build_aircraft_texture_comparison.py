"""Read original paired MB/GB models and export a local, read-only comparison."""
import hashlib
import io
import json
import re
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/localization'))
from stage_gop_primary_mirrors import pidx, read_payload

OUT = ROOT / 'analysis/aircraft-texture-comparison'


def main():
    pack = ROOT / 'native/vfs/PS3_GAME/USRDIR/DATA/PACK'
    _, rows, _ = pidx(pack / 'pack.idx')
    index = {r['name'].lower(): r for r in rows}
    packs = {p.name.lower(): p for p in pack.iterdir() if p.is_file()}
    def read(name):
        row = index[name]
        return read_payload(packs[Path(row['pack']).name.lower()], row)[0]
    elf = (ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    catalog = json.loads((ROOT / 'localization/image_assets/data-ui-v6/catalog-source-inventory.json').read_text(encoding='utf8'))
    names = {}
    for e in catalog['entries']:
        if any(r.get('role') == 'short_display_name' for r in e['refs']):
            offset = int(e['file_offset'], 16)
            # Model key is an adjacent literal; retain this evidence, not a guessed name.
            match = re.search(rb'\x00(MB_[A-Z0-9]+)\x00', elf[offset:offset+240])
            if match:
                names.setdefault(match[1].decode().lower(), []).append(e['source'])
    assets = OUT / 'assets'
    assets.mkdir(parents=True, exist_ok=True)
    cache = {}
    failures = []
    def export(name):
        if name in cache:
            return cache[name]
        raw = read(name)
        im = Image.open(io.BytesIO(raw))
        target = assets / (name + '.png')
        im.convert('RGBA').save(target)
        item = dict(name=name, image='assets/'+target.name, width=im.width, height=im.height,
                    sha256=hashlib.sha256(raw).hexdigest(), bytes=len(raw))
        cache[name] = item
        return item
    results = []
    for mb in sorted(n for n in index if n.startswith('mb_') and n.endswith('.mdl')):
        gb = 'g' + mb[1:]
        if gb not in index:
            continue
        sides = []
        for model in (mb, gb):
            prefix = model[:-4]
            refs = sorted(set(s.decode().lower() for s in re.findall(rb'[A-Za-z0-9_]+\.dds', read(model))))
            # Export every referenced texture, including shared maps, to expose real layout differences.
            textures = []
            for ref in refs:
                if ref not in index:
                    failures.append(dict(model=model, texture=ref, reason='not in primary index'))
                    continue
                try:
                    textures.append(export(ref))
                except (OSError, ValueError) as exc:
                    failures.append(dict(model=model, texture=ref, reason=str(exc)))
            textures.sort(key=lambda t: (not t['name'].replace('_','').startswith(prefix.replace('_','')), bool(re.search(r'_[nso]\.dds$', t['name'])), -t['width']*t['height'], t['name']))
            sides.append(dict(model=model, textures=textures))
        if all(s['textures'] for s in sides):
            results.append(dict(key=mb[:-4], names=names.get(mb[:-4], []), battle=sides[0], gallery=sides[1]))
    doc = dict(models=results, failures=failures, unique_textures=len(cache),
               note='Original MB/GB model references. Corresponding model IDs do not prove UV compatibility. No game files modified.')
    (OUT / 'manifest.json').write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding='utf8')
    html = TEMPLATE.replace('__DATA__', json.dumps(doc, ensure_ascii=False).replace('</', '<\\/'))
    (OUT / 'index.html').write_text(html, encoding='utf8')
    print(json.dumps(dict(models=len(results), textures=len(cache), unresolved=len(failures), html=str(OUT/'index.html'))))


TEMPLATE = '''<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>기체 텍스처 원본 비교</title><style>
*{box-sizing:border-box}body{margin:0;background:#10151c;color:#e1e8f0;font:15px system-ui}header{padding:24px;position:sticky;top:0;background:#10151cf5;z-index:2;border-bottom:1px solid #364052}h1{font-size:22px;margin:0 0 8px}p{color:#a9b7c9;margin:8px 0}input,select,button{background:#202c3a;color:inherit;border:1px solid #536277;border-radius:5px;padding:8px}input{width:min(440px,100%)}main{max-width:1600px;margin:auto;padding:20px}.card{background:#19222e;border:1px solid #364052;border-radius:10px;padding:18px;margin-bottom:24px}h2{margin:0 0 12px;font-size:19px}.pair{display:grid;grid-template-columns:1fr 1fr;gap:18px}.pane{min-width:0}.viewport{background:repeating-conic-gradient(#242b34 0% 25%,#303945 0% 50%) 0/24px 24px;overflow:auto;margin-top:10px;text-align:center;max-height:850px}.viewport img{display:block;width:100%;height:auto;image-rendering:auto}.native .viewport img{width:auto;max-width:none;margin:auto}.pixel img{image-rendering:pixelated}.info{font:12px ui-monospace,monospace;overflow-wrap:anywhere;padding-top:8px;color:#b9c9da}select{max-width:100%;width:100%}.bar{display:flex;gap:16px;align-items:center;flex-wrap:wrap;margin-top:14px}label{cursor:pointer}small{color:#acbbce}@media(max-width:700px){.pair{grid-template-columns:1fr}header{position:static}}</style>
<header><h1>미리보기 · 전투 텍스처 원본 비교</h1><p>왼쪽: 미리보기(GB) / 오른쪽: 전투(MB). 각 모델이 실제 참조하는 맵을 선택할 수 있습니다.</p><small>같은 기체 코드의 원본 리소스 비교이며, UV 호환성이나 교체 성공을 뜻하지 않습니다. 기본값은 화면 맞춤이므로 아래 원본 해상도를 함께 확인하세요.</small><div class="bar"><input id="search" placeholder="기체명 또는 코드 검색: VF-19E, VF-0, mb_00f"><label><input type="checkbox" id="native" style="width:auto"> 원본 크기(1:1)</label><label><input type="checkbox" id="pixel" style="width:auto"> 픽셀 선명하게</label><span id="count"></span></div></header><main id="list"></main>
<script>const data=__DATA__;const list=document.querySelector('#list');
function pane(side,title){const div=document.createElement('div');div.className='pane';const h=document.createElement('h3');h.textContent=title+' · '+side.model;div.append(h);const select=document.createElement('select');side.textures.forEach((t,i)=>{const o=document.createElement('option');o.value=i;o.textContent=t.name+' — '+t.width+' × '+t.height;select.append(o)});const viewport=document.createElement('div');viewport.className='viewport';const img=document.createElement('img');img.loading='lazy';viewport.append(img);const info=document.createElement('div');info.className='info';function update(){const t=side.textures[select.value];img.src=t.image;img.alt=t.name;info.textContent=t.width+' × '+t.height+' | '+t.bytes.toLocaleString()+' bytes | SHA256 '+t.sha256}select.onchange=update;div.append(select,viewport,info);update();return div}
function render(){const q=document.querySelector('#search').value.toLowerCase().trim();list.replaceChildren();const rows=data.models.filter(m=>(m.key+' '+m.names.join(' ')).toLowerCase().includes(q));document.querySelector('#count').textContent=rows.length+' / '+data.models.length+' 기체 · '+data.unique_textures+' 텍스처';rows.forEach(m=>{const card=document.createElement('section');card.className='card';const h=document.createElement('h2');h.textContent=(m.names.join(' / ')||m.key)+' ['+m.key+']';const pair=document.createElement('div');pair.className='pair';pair.append(pane(m.gallery,'미리보기'),pane(m.battle,'전투'));card.append(h,pair);list.append(card)})}document.querySelector('#search').oninput=render;for(const id of ['native','pixel'])document.querySelector('#'+id).onchange=e=>document.body.classList.toggle(id,e.target.checked);render();</script></html>'''

if __name__ == '__main__':
    main()
