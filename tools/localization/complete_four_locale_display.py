"""Guarded supplemental Korean ELF build and four-locale display readback audit."""
import argparse
import hashlib
import json
import struct
import numpy as np
from pathlib import Path
from korean_remaining_text import translate
from gop_strings import TOKEN
from audit_runtime_font import read_font
from stage_gop_primary_mirrors import pidx, read_payload, fsts_entries

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'build/tests/four-locale-display-20261115'
def read(p): return json.loads(p.read_text(encoding='utf8'))
def sha(b): return hashlib.sha256(b).hexdigest()
def write(p,d): p.write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n',encoding='utf8')

def main(publish=False):
    active_path=ROOT/'localization/image_assets/active-ui.json'
    active=read(active_path)
    if active.get('elf_override',{}).get('path')=='EBOOT.korean-display-v23.elf':
        active=read(OUT/'previous-active.json')
    bundle=ROOT/'localization/image_assets'/active['bundle']
    selected=active.get('elf_override',read(bundle/'ready.json')['elf'])
    base=bundle/selected['path'];before=base.read_bytes();assert sha(before)==selected['sha256']
    original=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    candidates=read(ROOT/'analysis/localization/elf_japanese_candidates.json');assert sha(original)==candidates['source_sha256']
    audit=read(ROOT/'analysis/localization/korean-remaining-20261115/audit.json')
    remaining={int(r['offset'],0):r for r in audit['other_literal_candidates'] if r['unchanged']}
    scope=read(ROOT/'localization/ja-en-ru-review/elf-scope.json')
    rows=[r for r in scope['entries'] if r['offset'] in remaining and r['source_ja']==remaining[r['offset']]['current'] and not 0x925918<=r['offset']<0x928cc8]
    assert len(rows)==154
    # Read the actual selected Korean VFS primary font, not OS font coverage.
    pack=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
    _,entries,archives=pidx(pack/'pack.idx');resident=next(r for r in archives if r['name']=='resident')
    fonts=fsts_entries(read_payload(pack/Path(resident['pack']).name,resident)[0])
    font=next(r['payload'] for r in fonts if r['name']=='data/font/font_j24x24.abf')
    glyphs={g['codepoint'] for g in read_font(font)[1]}
    after=bytearray(before);allowed=set();authored=[];problems=[]
    words=np.frombuffer(original[:len(original)//4*4],dtype='>u4')
    for r in rows:
        offset=r['offset'];source=r['source_ja'];target=translate(offset,source)
        raw=source.encode()+b'\0';replacement=target.encode()+b'\0'
        assert original[offset:offset+len(raw)]==before[offset:offset+len(raw)]==raw
        # A pointer into the middle of a literal would be invalidated by an
        # inline replacement even if the slot length itself stays unchanged.
        assert not np.any((words>offset+0x10000)&(words<offset+0x10000+len(raw)-1)),hex(offset)
        cap=len(raw)
        while cap<len(raw)+7 and (offset+cap)%8 and original[offset+cap]==before[offset+cap]==0:cap+=1
        assert TOKEN.findall(source)==TOKEN.findall(target) and source.count('\n')==target.count('\n')
        missing=''.join(sorted({c for c in target if ord(c)>=32 and ord(c) not in glyphs}))
        if missing:problems.append(dict(offset=hex(offset),missing_font=missing))
        if len(replacement)>cap:problems.append(dict(offset=hex(offset),required=len(replacement),capacity=cap))
        positions=set(range(offset,offset+cap));assert not allowed&positions
        allowed|=positions
        after[offset:offset+cap]=replacement.ljust(cap,b'\0')
        authored.append(dict(offset=hex(offset),source=source,source_sha256=sha(source.encode()),translation=target,capacity=cap,target_bytes=len(replacement)))
    OUT.mkdir(parents=True,exist_ok=True)
    write(OUT/'korean-authored.json',dict(source_elf_sha256=sha(before),entries=authored,problems=problems))
    if problems:
        print(json.dumps(dict(problems=problems),ensure_ascii=False));return
    assert len(before)==len(after) and all(i in allowed for i,(a,b) in enumerate(zip(before,after)) if a!=b)
    sh=struct.unpack_from('>Q',before,40)[0];stride,num=struct.unpack_from('>HH',before,58)
    ph=struct.unpack_from('>Q',before,32)[0];ps,pn=struct.unpack_from('>HH',before,54)
    assert after[:ph+ps*pn]==before[:ph+ps*pn] and after[sh:sh+stride*num]==before[sh:sh+stride*num]
    for i in range(num):
        q=sh+i*stride;flags=struct.unpack_from('>Q',before,q+8)[0];f,n=struct.unpack_from('>QQ',before,q+24)
        if flags&4:assert before[f:f+n]==after[f:f+n]
    english_ready=read(ROOT/'localization/en-US/pack-ready.json');en=(ROOT/english_ready['elf']).read_bytes()
    assert sha(en)==scope['english_elf_sha256']
    locale_checks={}
    for locale in ('ru-RU','es-ES'):
        folder=ROOT/'localization/installed-packs'/locale
        manifest=read(folder/'pack.json');texts=read(folder/'elf-authored.json');payload=(folder/'elf-text.pack').read_bytes()
        font_path=folder/'gop/font_j24x24.abf'
        assert sha(font_path.read_bytes())==next(p['sha256'] for p in manifest['proofs'] if p['path']=='gop/font_j24x24.abf')
        locale_glyphs={g['codepoint'] for g in read_font(font_path.read_bytes())[1]}
        for name in ('elf-authored.json','elf-text.pack'):
            assert sha((folder/name).read_bytes())==next(p['sha256'] for p in manifest['proofs'] if p['path']==name)
        assert payload[:8]==b'M30TXP1\0' and payload[8:40]==bytes.fromhex(sha(en))
        count=struct.unpack_from('<I',payload,40)[0];pos=48;records={}
        for _ in range(count):
            ea,cap,length,refs=struct.unpack_from('<IIII',payload,pos);pos+=16
            expected=payload[pos:pos+cap];pos+=cap
            text=payload[pos:pos+length].decode();pos+=length+refs*4
            records[ea]=(expected,text)
        assert pos==len(payload)
        verified=[]
        for r in rows:
            off=r['offset'];expected,text=records[off+0x10000]
            assert expected==en[off:off+len(expected)] and text==texts[str(off)]
            assert TOKEN.findall(text)==TOKEN.findall(r['source_ja'])
            assert text.count('\n')==r['source_ja'].count('\n')
            assert all(ord(c) in locale_glyphs for c in text if ord(c)>=32)
            verified.append(dict(offset=hex(off),text=text))
        locale_checks[locale]=dict(verified=len(verified),entries=verified,pack_sha256=sha(payload),changed=False)
    en_checks=[]
    for r in rows:
        o=r['offset'];text=en[o:en.index(0,o)].decode();assert text==r['installed_en']
        en_checks.append(dict(offset=hex(o),text=text))
    locale_checks['en-US']=dict(verified=len(en_checks),entries=en_checks,changed=False)
    target=OUT/'EBOOT.korean-display-v23.elf';target.write_bytes(after)
    for row in authored:
        o=int(row['offset'],0);assert after[o:after.index(0,o)].decode()==row['translation']
    proof=dict(status='PASS',source=str(base.relative_to(ROOT)),source_sha256=sha(before),output_sha256=sha(after),korean_strings=len(authored),font_missing=0,outside_slots_changed=0,code_headers_unchanged=True,locales=locale_checks,music_titles_credits_preserved=True,game_render_verified=False,published=False)
    if publish:
        # Publish a new independent file; no live ELF or hardlinked file is edited.
        published=bundle/target.name
        if published.exists():assert published.read_bytes()==after
        else:published.write_bytes(after)
        assert published.read_bytes()==after
        write(OUT/'previous-active.json',active)
        active['elf_override']=dict(path=published.name,sha256=sha(after),base_elf_sha256=read(bundle/'ready.json')['elf']['sha256'])
        write(active_path,active)
        write(ROOT/'final/korean/active-ui.json',active)
        write(bundle/'display-v23.json',dict(path=published.name,sha256=sha(after),base_elf_sha256=sha(before),strings=len(authored),game_render_verified=False))
        proof['published']=True
    write(OUT/'verification.json',proof)
    print(json.dumps(dict(status='PASS',korean_strings=len(authored),other_locales={k:v['verified'] for k,v in locale_checks.items()},published=proof['published'],output=str(target)),ensure_ascii=False))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--publish',action='store_true');main(p.parse_args().publish)
