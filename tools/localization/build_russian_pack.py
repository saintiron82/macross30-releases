"""Assemble source-checked Russian text while preserving English image payloads."""
import argparse,hashlib,json,os,re,shutil,struct,textwrap,zipfile,unicodedata
from functools import lru_cache
from pathlib import Path
from russian_pack import ROOT,BASE,read,write,sha,validate
from gop_strings import parse,TOKEN
from stage_gop_primary_mirrors import pidx,read_payload,fsts_entries
from english_font import original_font
from audit_runtime_font import read_font
from build_english_voice_sidecar import wrap_caption

OUTPUT=ROOT/'final/russian/russian-preview-v1'
STATE=ROOT/'final/russian/state-ru-preview-v1'
SUBS=ROOT/'final/russian/subtitles-v1'

def rebuild_gop(source,translations):
    """Reflow GENESTRT; all other sections and string identities are unchanged."""
    rows=parse(source);pos=16
    while source[pos:pos+8]!=b'GENESTRT':pos+=16+struct.unpack_from('>I',source,pos+8)[0]
    n=struct.unpack_from('>I',source,pos+8)[0];base=pos+16
    count,table,values,limit=struct.unpack_from('>4I',source,base)
    assert limit==n and len(rows)==count
    body=bytearray(source[base:base+values]);strings=bytearray()
    for row in rows:
        text=translations.get(row['index'],row['text'])
        if row['index'] in translations:
            assert not row['metadata'] and TOKEN.findall(text)==TOKEN.findall(row['text'])
        struct.pack_into('>I',body,table+row['index']*4,len(strings));strings.extend(text.encode()+b'\0')
    body.extend(strings);body.extend(bytes(-len(body)%16));struct.pack_into('>I',body,12,len(body))
    header=source[pos:pos+8]+struct.pack('>I',len(body))+source[pos+12:base]
    out=source[:pos]+header+body+source[base+n:]
    assert out[:pos]==source[:pos] and out[pos+16+len(body):]==source[base+n:]
    after=parse(out);assert len(after)==len(rows)
    assert all(b['text']==translations.get(a['index'],a['text']) and a['metadata']==b['metadata'] for a,b in zip(rows,after))
    return bytes(out)

def normalize(text):
    text=unicodedata.normalize('NFC',text)
    text=''.join(c for c in text if not unicodedata.combining(c))
    return text.translate(str.maketrans({'—':'-','–':'-','…':'...','⋯':'...','“':'"','”':'"','„':'"','«':'"','»':'"','「':'"','」':'"','『':'"','』':'"','‘':"'",'’':"'",'\u00a0':' ','\u3000':' ','～':'~'}))
def cyrillic_font(font):
    """Use existing Cyrillic ink proportionally; never change atlas pixels."""
    common,rows,atlas=read_font(font);out=bytearray(font);allowed=set()
    alphabet=set(range(0x410,0x450))|{0x401,0x451};count=0
    for g in rows:
        if g['codepoint'] not in alphabet:continue
        at=g['record_offset'];struct.pack_into('>h',out,at+12,min(0,g['bearing_x']))
        struct.pack_into('>h',out,at+16,max(1,g['width']+1));allowed.update(range(at+12,at+14));allowed.update(range(at+16,at+18));count+=1
    assert count==66 and all(i in allowed for i,(a,b) in enumerate(zip(font,out)) if a!=b)
    after,new_rows,new_atlas=read_font(out);assert common==after and atlas==new_atlas
    for a,b in zip(rows,new_rows):
        assert all(a[k]==b[k] for k in a if k not in ('bearing_x','advance'))
        if a['codepoint'] not in alphabet:assert a==b
    return bytes(out),{g['codepoint']:g for g in new_rows}
@lru_cache(maxsize=1)
def russian_glyphs():return cyrillic_font(original_font()[0])[1]
def pages(text,native=False):
    text=normalize(text).replace('\\f',' ').replace('\\n',' ').replace('\\\\','\\')
    if native:return wrap_caption(text,russian_glyphs()).split('\f')
    lines=textwrap.wrap(' '.join(text.split()),width=42,break_long_words=False,break_on_hyphens=False)
    result=['\n'.join(lines[i:i+2]) for i in range(0,len(lines),2)] or ['...']
    if any(len(s.encode())>254 for s in result):raise ValueError('Russian caption page exceeds native byte limit')
    if len(result)>16:raise ValueError('Russian caption page limit')
    return result
def timestamp(ms):
    h,ms=divmod(ms,3600000);m,ms=divmod(ms,60000);s,ms=divmod(ms,1000)
    return f'{h:02}:{m:02}:{s:02},{ms:03}'
def milliseconds(text):
    h,m,s,ms=map(int,re.split('[:,]',text));return ((h*60+m)*60+s)*1000+ms
def clone(source,target):
    # Immutable VFS files may share disk data. Modified files are copied first.
    shutil.copytree(source,target,copy_function=os.link)
def private_file(path):
    temp=path.with_name(path.name+'.ru-private');shutil.copyfile(path,temp);os.replace(temp,path)
def packfile(folder,name):
    p=folder/Path(name).name
    if p.exists():return p
    return next(p for p in folder.iterdir() if p.name.lower()==Path(name).name.lower())
def scaffold():
    scope=read(BASE/'scope.json');english=scope['english_ready'];source=ROOT/english['vfs']
    marker=BASE/'build/scaffold.json'
    if marker.exists():
        proof=read(marker);assert proof['english_elf_sha256']==scope['english_elf_sha256']
        assert OUTPUT.is_dir() and STATE.is_dir();return
    assert not OUTPUT.exists() and not STATE.exists()
    OUTPUT.parent.mkdir(parents=True,exist_ok=True);clone(source,OUTPUT)
    dest=OUTPUT/'PS3_GAME/USRDIR/DATA/PACK'
    for name in ('DATA.DAT','PACK.IDX'):private_file(packfile(dest,name))
    shutil.copytree(ROOT/english['state'],STATE)
    write(marker,dict(english_elf_sha256=scope['english_elf_sha256'],image_locale='en-US',status='English base cloned; Russian text not installed yet'))
    print('Russian VFS and independent HDD prepared; English images preserved',flush=True)
def patch_pack(folder,replacements,expected,*,font_payload=None,font_atlases=None,proof_directory=None):
    font_atlases={} if font_atlases is None else font_atlases
    if not isinstance(font_atlases,dict) or set(font_atlases)-{'data/font/font_j24x24_0.dds'}:
        raise ValueError('Only the dedicated font_j24x24_0.dds atlas can be replaced')
    if font_atlases and font_payload is None:raise ValueError('A font atlas requires an explicit matching ABF')
    index=packfile(folder,'pack.idx');_,master,archives=pidx(index)
    names={Path(r['pack']).name.lower() for r in master if r['name'] in replacements}
    assert names<= {'data.dat'}
    pack=packfile(folder,'data.dat');_,embedded,embedded_archives=pidx(pack)
    def append(blob):
        with pack.open('ab') as f:f.write(bytes(-f.tell()%2048));off=f.tell();f.write(blob)
        return off
    def update(path,at,values):
        with path.open('r+b') as f:f.seek(at);f.write(struct.pack('<'+'I'*len(values),*values))
    for name,payload in replacements.items():
        for rows in (master,embedded):
            hits=[r for r in rows if r['name']==name];assert hits
            assert all(sha(read_payload(pack,r)[0])==expected[name] for r in hits)
        off=append(payload)
        for path,rows in ((index,master),(pack,embedded)):
            for r in rows:
                if r['name']==name:update(path,r['record']+12,(off,len(payload),0))
    arc=next(r for r in archives if r['name']=='gop');earc=next(r for r in embedded_archives if r['name']=='gop')
    raw=read_payload(pack,arc)[0];rebuilt=bytearray(raw);table=struct.unpack_from('<I',raw,8)[0];found=set()
    for e in fsts_entries(raw):
        name=Path(e['name']).name
        if name not in replacements:continue
        assert sha(e['payload'])==expected[name];found.add(name)
        rebuilt.extend(bytes(-len(rebuilt)%16));off=len(rebuilt);rebuilt.extend(replacements[name])
        struct.pack_into('<III',rebuilt,table+e['entry']*16+4,off,len(replacements[name]),0)
    assert found==set(replacements);off=append(rebuilt)
    for path,row in ((index,arc),(pack,earc)):update(path,row['record']+8,(off,len(rebuilt)))
    resident=next(r for r in archives if r['name']=='resident');eresident=next(r for r in embedded_archives if r['name']=='resident')
    resident_raw=read_payload(pack,resident)[0];resident_entries=fsts_entries(resident_raw)
    font=next(e for e in resident_entries if e['name']=='data/font/font_j24x24.abf')
    if font_payload is None:assert font['payload']==original_font()[0]
    russian_font=font_payload if font_payload is not None else cyrillic_font(font['payload'])[0]
    extension_proof=None
    if font_atlases:
        from pack_font import validate_extension
        atlas=next(e for e in resident_entries if e['name']=='data/font/font_j24x24_0.dds')
        extension_proof=validate_extension(font['payload'],atlas['payload'],russian_font,font_atlases[atlas['name']])
    atlas_pixels_unchanged=all(entry['payload']==font_atlases.get(entry['name'],entry['payload']) for entry in resident_entries if entry['name'].lower().endswith('.dds'))
    resident_out=bytearray(resident_raw)
    proof_directory=Path(proof_directory) if proof_directory is not None else BASE/'build'
    proof_directory.mkdir(parents=True,exist_ok=True)
    (proof_directory/'font_j24x24.abf').write_bytes(russian_font)
    write(proof_directory/'font-metrics.json',dict(source_sha256=sha(font['payload']),replacement_sha256=sha(russian_font),
                                            cyrillic_records=66 if font_payload is None else None,atlas_pixels_unchanged=atlas_pixels_unchanged,
                                            atlas_extension_verified=extension_proof is not None))
    font_table=struct.unpack_from('<I',resident_raw,8)[0]
    resident_replacements={font['name']:russian_font,**font_atlases}
    for entry in resident_entries:
        if entry['name'] not in resident_replacements:continue
        payload=resident_replacements[entry['name']]
        resident_out.extend(bytes(-len(resident_out)%16));offset=len(resident_out);resident_out.extend(payload)
        struct.pack_into('<III',resident_out,font_table+entry['entry']*16+4,offset,len(payload),0)
    updated=fsts_entries(resident_out)
    for before,after in zip(resident_entries,updated):
        assert before['name']==after['name']
        assert after['payload']==resident_replacements.get(before['name'],before['payload'])
    off=append(resident_out)
    for destination,row in ((index,resident),(pack,eresident)):update(destination,row['record']+8,(off,len(resident_out)))
    for path in (index,pack):
        _,rows,arcs=pidx(path)
        for r in rows:
            if r['name'] in replacements:assert read_payload(pack,r)[0]==replacements[r['name']]
        assert read_payload(pack,next(r for r in arcs if r['name']=='gop'))[0]==rebuilt
        assert read_payload(pack,next(r for r in arcs if r['name']=='resident'))[0]==resident_out
    # Every primary DDS route and all non-GOP archive descriptors stay exact.
    _,after,arcs=pidx(index)
    assert [r for r in master if r['name'].lower().endswith('.dds')]==[r for r in after if r['name'].lower().endswith('.dds')]
    assert [r for r in archives if r['name'] not in ('gop','resident')]==[r for r in arcs if r['name'] not in ('gop','resident')]
    _,old_glyphs,_=read_font(font['payload']);_,new_glyphs,_=read_font(russian_font)
    new_by_codepoint={row['codepoint']:row for row in new_glyphs}
    changed_metrics=sum(a['bearing_x']!=new_by_codepoint[a['codepoint']]['bearing_x'] or a['advance']!=new_by_codepoint[a['codepoint']]['advance'] for a in old_glyphs)
    return dict(primary_gops=len(replacements),nested_gops=len(found),image_records_unchanged=True,
                cyrillic_metric_records=66 if font_payload is None else None,font_metric_records_changed=changed_metrics,
                added_font_glyphs=len(new_glyphs)-len(old_glyphs),font_atlas_pixels_unchanged=atlas_pixels_unchanged,
                font_atlas_extension_verified=extension_proof is not None,font_extension=extension_proof,
                all_other_resident_payloads_unchanged=True)

def assemble():
    scope=read(BASE/'scope.json');draft=read(BASE/'translations.json');jobs={j['id']:j for j in scope['jobs']}
    if scope.get('source_language')!='ja-JP' or draft.get('translation_method')!='direct_assistant_Japanese_source':
        raise ValueError('English-pivot/Ollama Russian drafts are excluded. Complete the Japanese-source bilingual review and import its admitted records first.')
    missing=set(jobs)-set(draft['entries'])
    if missing:raise ValueError(f'{len(missing)} Russian translations remain; pack admission refused')
    values={}
    for k,j in jobs.items():
        value=normalize(draft['entries'][k]['translation']);masked,_=__import__('russian_pack').mask(j['source'])
        # Re-mask protected tokens for validation, then require exact restored text.
        tokenized,_=__import__('russian_pack').mask(value)
        checked,reason=validate(j,tokenized)
        if reason or checked!=value:raise ValueError(f'Translation validation failed {k}: {reason}')
        values[k]=value
    _,_,glyphs,_=original_font();missing_glyphs=sorted({ord(c) for v in values.values() for c in normalize(v) if c not in '\n\r\t' and ord(c) not in glyphs})
    if missing_glyphs:raise ValueError('Missing glyphs '+str(missing_glyphs))
    english=scope['english_ready'];source=ROOT/english['vfs'];source_pack=source/'PS3_GAME/USRDIR/DATA/PACK'
    elf=(ROOT/english['elf']).read_bytes();assert sha(elf)==scope['english_elf_sha256']
    assert not SUBS.exists()
    scaffold()
    dest_pack=OUTPUT/'PS3_GAME/USRDIR/DATA/PACK'
    _,rows,_=pidx(source_pack/'PACK.IDX');replacements={};expected={}
    gop_dir=BASE/'build/gop';gop_dir.mkdir(parents=True,exist_ok=True)
    for g in scope['gops']:
        r=next(r for r in rows if r['name']==g['name']);raw=read_payload(packfile(source_pack,r['pack']),r)[0];assert sha(raw)==g['source_sha256']
        targets={e['index']:values[e['job']] for e in g['entries']}
        replacement=rebuild_gop(raw,targets);replacements[g['name']]=replacement;expected[g['name']]=sha(raw)
        (gop_dir/g['name']).write_bytes(replacement)
    resource_proof=patch_pack(dest_pack,replacements,expected)
    out=bytearray(elf);allowed=set()
    intervals=[]
    for slot in scope['elf_slots']:
        off=slot['offset'];cap=slot['capacity'];text=values[slot['job']];raw=text.encode()+b'\0'
        assert len(raw)<=cap and elf[off:off+len(slot['source'].encode())+1]==slot['source'].encode()+b'\0'
        assert not allowed.intersection(range(off,off+cap));allowed.update(range(off,off+cap));intervals.append((off,cap))
        out[off:off+cap]=raw.ljust(cap,b'\0')
    assert all(i in allowed for i,(a,b) in enumerate(zip(elf,out)) if a!=b)
    ph=struct.unpack_from('>Q',elf,32)[0];ps,pn=struct.unpack_from('>HH',elf,54)
    assert out[:ph+ps*pn]==elf[:ph+ps*pn]
    sh=struct.unpack_from('>Q',elf,40)[0];stride,count=struct.unpack_from('>HH',elf,58)
    for i in range(count):
        at=sh+i*stride;flags=struct.unpack_from('>Q',elf,at+8)[0];off,size=struct.unpack_from('>QQ',elf,at+24)
        if flags&4:assert out[off:off+size]==elf[off:off+size]
    ru_elf=OUTPUT/'EBOOT.russian.elf';ru_elf.write_bytes(out)
    # State is independently writable; no hardlinks to the English HDD.
    hdd=STATE/'hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
    for name in ('data.dat','pack.idx'):shutil.copyfile(packfile(dest_pack,name),packfile(hdd,name))
    SUBS.mkdir(parents=True)
    tracks=(ROOT/english['subtitles']/'tracks.tsv').read_text(encoding='utf8').splitlines();track_rows=[tracks[0]];movie_files={}
    for movie in scope['movies']:
        blocks=[];number=0
        for cue in movie['cues']:
            parts=pages(values[cue['job']]);start,end=map(milliseconds,[s.strip() for s in cue['timing'].split('-->')])
            assert end-start>=len(parts)
            for i,text in enumerate(parts):
                number+=1;a=start+(end-start)*i//len(parts);b=start+(end-start)*(i+1)//len(parts)
                blocks.append(f'{number}\n{timestamp(a)} --> {timestamp(b)}\n{text}')
        payload=('\n\n'.join(blocks)+'\n').encode();(SUBS/movie['name']).write_bytes(payload);movie_files[movie['english_name']]=(movie['name'],sha(payload))
    for row in tracks[1:]:
        fields=row.split('\t');name,digest=movie_files[fields[1]];fields[1]=name;fields[5]=digest;fields[-1]='draft';track_rows.append('\t'.join(fields))
    (SUBS/'tracks.tsv').write_text('\n'.join(track_rows)+'\n',encoding='utf8')
    voice=['MACROSS_BATTLE_CAPTIONS_V2\tru-RU']
    for row in scope['voice']:
        parts=pages(values[row['job']],native=True);text='\f'.join(parts).replace('\\','\\\\').replace('\n','\\n').replace('\f','\\f')
        voice.append(row['identity']+'\t'+row['duration']+'\t'+text)
    (SUBS/'battle.ru.tsv').write_text('\n'.join(voice)+'\n',encoding='utf8')
    native=['/* Source-checked Russian native UI draft. */']
    for row in scope['native_ui']:
        native.append('MACROSS_RU_STRING('+json.dumps(row['key'])+', L'+json.dumps(normalize(values[row['job']]),ensure_ascii=False)+')')
    (ROOT/'vendor/ps3recomp-master/libs/video/macross_ui_ru.def').write_text('\n'.join(native)+'\n',encoding='utf8')
    proof=dict(status='PASS',locale='ru-RU',image_locale='en-US',english_source_ready_sha256=sha(json.dumps(english,sort_keys=True).encode()),
               translated_unique_strings=len(values),gop_entries=sum(len(g['entries']) for g in scope['gops']),elf_slots=len(scope['elf_slots']),
               movie_tracks=len(scope['movies']),voice_stems=len(scope['voice']),native_ui_keys=len(scope['native_ui']),font_missing=0,
               music_titles_preserved=True,image_records_unchanged=True,elf_code_and_headers_unchanged=True,resource_readback=resource_proof,
               source_elf_sha256=sha(elf),russian_elf_sha256=sha(out),game_render_verified=False,human_translation_review_complete=False)
    write(BASE/'build/verification.json',proof);print(json.dumps(proof),flush=True)
    return proof

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--scaffold',action='store_true');args=parser.parse_args()
    if args.scaffold:scaffold()
    else:assemble()
