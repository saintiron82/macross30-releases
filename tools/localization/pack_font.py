"""Data-only BFNT/BC3 extension; optional exact Russian proportional metrics."""
import argparse
import hashlib
import json
import struct
import unicodedata
from pathlib import Path

from audit_runtime_font import chunks, read_font, occupied_rows, place_cells
from build_korean_font import decode_alpha, encode_white_block, alpha_palette


def sha(data):
    return hashlib.sha256(data).hexdigest()


RUSSIAN_ALPHABET=frozenset(range(0x410,0x450))|{0x401,0x451}


def proportional_cyrillic(abf):
    """Normalize only the 66 Russian bearings/advances; preserve all ink and IDs."""
    _,glyphs,_=read_font(abf);rows=[g for g in glyphs if g['codepoint'] in RUSSIAN_ALPHABET]
    if len(rows)!=66 or {g['codepoint'] for g in rows}!=RUSSIAN_ALPHABET:
        raise ValueError('Expected all 66 original Russian alphabet glyphs')
    result=bytearray(abf)
    for g in rows:
        struct.pack_into('>h',result,g['record_offset']+12,min(0,g['bearing_x']))
        struct.pack_into('>h',result,g['record_offset']+16,max(1,g['width']+1))
    return bytes(result)


def cyrillic_metric_report(before,after):
    old={g['codepoint']:g for g in read_font(before)[1]};new={g['codepoint']:g for g in read_font(after)[1]}
    samples={}
    for text in ['Словарь','Сохранение','Миссия','Плазменная пушка','ЖЩШМИиюь']:
        samples[text]=dict(before_advance=sum(old[ord(c)]['advance'] for c in text),
                           after_advance=sum(new[ord(c)]['advance'] for c in text))
    changed=sum(any(old[cp][k]!=new[cp][k] for k in ('bearing_x','advance')) for cp in RUSSIAN_ALPHABET)
    return dict(policy='66 Russian letters: advance=width+1, bearing_x=min(0,original)',
                changed_glyphs=changed,samples_at_24px=samples,
                glyphs={chr(cp):dict(width=old[cp]['width'],before_advance=old[cp]['advance'],after_advance=new[cp]['advance'],
                    before_bearing_x=old[cp]['bearing_x'],after_bearing_x=new[cp]['bearing_x']) for cp in sorted(RUSSIAN_ALPHABET)})


def _scale80(value):
    return (value*4+2)//5 if value>=0 else -((-value*4+2)//5)


def _compact_metrics(abf,added_codepoints=()):
    result=bytearray(proportional_cyrillic(abf));_,glyphs,_=read_font(result)
    selected=set(range(32,127))|RUSSIAN_ALPHABET|set(added_codepoints)
    for g in glyphs:
        if g['codepoint'] not in selected:continue
        width=max(1,_scale80(g['width']));height=max(1,_scale80(g['height']))
        bx=_scale80(g['bearing_x']);by=20-_scale80(20-g['bearing_y'])
        advance=max(1,_scale80(g['advance']),width+max(0,bx))
        struct.pack_into('>2H3h',result,g['record_offset']+8,width,height,bx,by,advance)
    return bytes(result)


def compact_russian_profile(abf,dds,added_codepoints=()):
    """Exact 4/5 raster profile; shared blocks preserve all unrelated pixels."""
    from PIL import Image
    common,glyphs,_=read_font(abf);adjusted=_compact_metrics(abf,added_codepoints)
    by_code={g['codepoint']:g for g in read_font(adjusted)[1]}
    selected=set(range(32,127))|RUSSIAN_ALPHABET|set(added_codepoints)
    targets=[g for g in glyphs if g['codepoint'] in selected]
    unselected=occupied_rows(common,[g for g in glyphs if g['codepoint'] not in selected],1)
    w,h=common['width'],common['height']
    if common['page_count']!=1 or len(dds)!=128+w*h or dds[84:88]!=b'DXT5':raise ValueError('Expected single-page BC3 compact profile')
    blocks={};masks={};original_blocks={}
    def pixels(index):
        if index not in blocks:
            original_blocks[index]=dds[128+index*16:144+index*16]
            blocks[index]=decode_alpha(original_blocks[index])
        return blocks[index]
    for g in targets:
        if any(unselected[0][y]&(((1<<g['width'])-1)<<g['x']) for y in range(g['y'],g['y']+g['height'])):
            raise ValueError('Selected compact glyph shares texture rectangle with an unrelated glyph')
        crop=[]
        for y in range(g['height']):
            for x in range(g['width']):
                ax,ay=g['x']+x,g['y']+y;index=(ay//4)*(w//4)+ax//4
                crop.append(decode_alpha(dds[128+index*16:144+index*16])[(ay%4)*4+ax%4])
        new=by_code[g['codepoint']]
        image=Image.frombytes('L',(g['width'],g['height']),bytes(crop))
        scaled=image.resize((new['width'],new['height']),Image.Resampling.LANCZOS).tobytes()
        for y in range(g['height']):
            for x in range(g['width']):
                ax,ay=g['x']+x,g['y']+y;index=(ay//4)*(w//4)+ax//4;pixel=(ay%4)*4+ax%4
                masks[index]=masks.get(index,0)|(1<<pixel)
                pixels(index)[pixel]=scaled[y*new['width']+x] if x<new['width'] and y<new['height'] else 0
    result=bytearray(dds);shared=0
    for index,values in blocks.items():
        original=original_blocks[index];mask=masks[index]
        if mask==65535:
            encoded=encode_white_block(values)[:8]+original[8:]
        else:
            shared+=1;palette=alpha_palette(original[0],original[1])
            if 0 not in palette:raise ValueError('Shared compact glyph block has no transparent palette entry')
            bits=int.from_bytes(original[2:8],'little')
            for pixel,value in enumerate(values):
                if mask&(1<<pixel):
                    chosen=min(range(8),key=lambda i:abs(palette[i]-value))
                    bits=(bits&~(7<<(pixel*3)))|(chosen<<(pixel*3))
            encoded=original[:2]+bits.to_bytes(6,'little')+original[8:]
            decoded=decode_alpha(encoded);before=decode_alpha(original)
            if any(decoded[i]!=before[i] for i in range(16) if not mask&(1<<i)):raise AssertionError('Unrelated alpha changed')
        result[128+index*16:144+index*16]=encoded
    old={g['codepoint']:g for g in glyphs};samples={}
    for text in ['Словарь','Сохранение','VF-0S 12345','Миссия','Плазменная пушка']:
        samples[text]=dict(before_advance=sum(old[ord(c)]['advance'] for c in text),after_advance=sum(by_code[ord(c)]['advance'] for c in text))
    return adjusted,bytes(result),dict(scale_numerator=4,scale_denominator=5,baseline=20,global_lineheight_changed=False,
        existing_ascii_and_russian=161,scaled_glyphs=len(targets),shared_blocks_preserving_other_pixels=shared,
        samples_at_24px=samples,unrelated_glyph_pixels_identical=True,raster_filter='Pillow LANCZOS',
        metrics='width,height,bearings,advance scaled4/5 after Russian proportional normalization; baseline20 preserved; advance covers ink')


def extract_resident_font(pack_directory, output):
    """Extract actual resident originals into isolated scratch; return (abf_path, dds_path)."""
    from stage_gop_primary_mirrors import pidx, read_payload, fsts_entries
    pack_directory,output=Path(pack_directory),Path(output)
    index=next((p for p in pack_directory.iterdir() if p.name.lower()=='pack.idx'),None)
    if index is None: raise ValueError('PACK.IDX missing')
    rows=[r for r in pidx(index)[2] if r['name']=='resident']
    if len(rows)!=1: raise ValueError('Expected one resident archive')
    row=rows[0];name=Path(row['pack']).name.lower()
    payload_file=next((p for p in pack_directory.iterdir() if p.name.lower()==name),None)
    if payload_file is None: raise ValueError('Resident payload file missing')
    entries=fsts_entries(read_payload(payload_file,row)[0])
    output.mkdir(parents=True,exist_ok=True)
    paths=[]
    for filename in ('font_j24x24.abf','font_j24x24_0.dds'):
        matches=[e for e in entries if e['name']=='data/font/'+filename]
        if len(matches)!=1: raise ValueError('Expected one original resident '+filename)
        destination=output/filename
        destination.write_bytes(matches[0]['payload']);paths.append(destination)
    return tuple(paths)


def validate_extension(original_abf, original_dds, extended_abf, extended_dds):
    common, glyphs, pages = read_font(original_abf)
    after, added, after_pages = read_font(extended_abf)
    if pages != after_pages or common['page_count'] != 1:
        raise ValueError('Font page definitions must remain identical and single-page')
    if {k:v for k,v in common.items() if k != 'glyph_count'} != {k:v for k,v in after.items() if k != 'glyph_count'}:
        raise ValueError('Atlas dimensions changed')
    if len(added) < len(glyphs) or len({g['codepoint'] for g in added}) != len(added):
        raise ValueError('Removed or duplicate glyphs')
    def cblk(data):return b''.join(p for _,m,k,p,_ in chunks(data) if (m,k)==(b'BFNT',b'CBLK'))
    prefix=cblk(extended_abf)[:len(cblk(original_abf))]
    metric_changes=prefix!=cblk(original_abf)
    # A metric-changing extension must match the entire explicit profile, not
    # merely alter selected glyphs or exploit an allowlist of writable bytes.
    compact=False;compact_dds=original_dds
    comparison_abf=original_abf
    if metric_changes:
        proportional=proportional_cyrillic(original_abf)
        if prefix==cblk(proportional):comparison_abf=proportional
        else:
            compact_abf=_compact_metrics(original_abf)
            if prefix!=cblk(compact_abf):raise ValueError('Existing glyph changed outside exact approved profiles')
            compact=True;comparison_abf,compact_dds,_=compact_russian_profile(original_abf,original_dds)
    _,comparison_glyphs,_=read_font(comparison_abf)
    for a,b in zip(comparison_glyphs,added):
        if any(a[k] != b[k] for k in a if k != 'record_offset'):
            raise ValueError('Existing glyph changed')
    old_chunks, new_chunks = list(chunks(comparison_abf)), list(chunks(extended_abf))
    if len(old_chunks) != len(new_chunks): raise ValueError('ABF chunk count changed')
    for a,b in zip(old_chunks,new_chunks):
        if a[1:3] != b[1:3]: raise ValueError('ABF chunk order changed')
        if a[1:3] == (b'BFNT',b'CBLK'):
            if b[3][:len(a[3])] != a[3]: raise ValueError('Original CBLK bytes changed')
            if a[4] not in (0,len(a[3])+16) or b[4] != (len(b[3])+16 if a[4] else 0):
                raise ValueError('CBLK size convention changed')
        elif a[1:3] == (b'BFNT',b'HCMN'):
            if a[3][4:] != b[3][4:] or a[4] != b[4]: raise ValueError('Original font metrics changed')
        elif a[1:] != b[1:]: raise ValueError('Non-glyph ABF chunk changed')
    w,h=common['width'],common['height']
    if (original_dds[:4] != b'DDS ' or original_dds[84:88] != b'DXT5' or
        struct.unpack_from('<II',original_dds,12) != (h,w) or
        len(original_dds) != 128+w*h or len(extended_dds) != len(original_dds) or
        original_dds[:128] != extended_dds[:128]):
        raise ValueError('Expected unchanged single base-level BC3 DDS header')
    rows=occupied_rows(common,glyphs,4)
    # Every added rectangle must be disjoint from old and other new rectangles.
    for g in added[len(glyphs):]:
        if not g['width'] or not g['height']: raise ValueError('Empty added glyph')
        x0,y0=g['x']//4*4,g['y']//4*4
        x1,y1=(g['x']+g['width']+3)//4*4,(g['y']+g['height']+3)//4*4
        if g['page'] != 0 or x1>w or y1>h: raise ValueError('Added glyph out of atlas')
        mask=((1<<(x1-x0))-1)<<x0
        for y in range(y0,y1):
            if rows[0][y]&mask: raise ValueError('Added glyph overlaps protected glyph block')
            rows[0][y]|=mask
    protected=occupied_rows(common,glyphs,4)[0]
    allowed=occupied_rows(common,added[len(glyphs):],4)[0]
    changed=0
    for y in range(0,h,4):
        for x in range(0,w,4):
            off=128+((y//4)*(w//4)+x//4)*16
            if compact_dds[off:off+16] == extended_dds[off:off+16]: continue
            changed+=1
            if protected[y]&(15<<x) or not allowed[y]&(15<<x):
                raise ValueError('DDS changed outside new glyphs or inside original glyphs')
            if any(decode_alpha(original_dds[off:off+16])):
                raise ValueError('New glyph overwrote visible original atlas pixels')
    return dict(original_glyphs=len(glyphs), added_glyphs=len(added)-len(glyphs),
                changed_blocks=changed, original_records_identical=not metric_changes,
                original_records_match_approved_metrics=True,proportional_cyrillic=metric_changes,compact_russian=compact,
                normalized_russian_glyphs=66 if metric_changes else 0,
                original_glyph_blocks_identical=not compact,unrelated_glyph_pixels_identical=True,original_dds_header_identical=True)


def font_has(font_data, cp):
    """Check actual sfnt cmap (4/12), avoiding rasterizer missing-glyph boxes."""
    n=struct.unpack_from('>H',font_data,4)[0]
    table=None
    for i in range(n):
        tag,_,off,size=struct.unpack_from('>4sIII',font_data,12+16*i)
        if tag==b'cmap': table=font_data[off:off+size]
    if table is None: raise ValueError('Font lacks cmap; standalone TTF/OTF required')
    for i in range(struct.unpack_from('>H',table,2)[0]):
        platform,encoding,off=struct.unpack_from('>HHI',table,4+8*i)
        if platform not in (0,3): continue
        fmt=struct.unpack_from('>H',table,off)[0]
        if fmt==12:
            count=struct.unpack_from('>I',table,off+12)[0]
            for j in range(count):
                lo,hi,gid=struct.unpack_from('>III',table,off+16+j*12)
                if lo<=cp<=hi and gid+cp-lo: return True
        elif fmt==4 and cp<=65535:
            count=struct.unpack_from('>H',table,off+6)[0]//2
            ends=off+14;starts=ends+2*count+2;deltas=starts+2*count;ranges=deltas+2*count
            for j in range(count):
                end,start=struct.unpack_from('>H',table,ends+2*j)[0],struct.unpack_from('>H',table,starts+2*j)[0]
                if start<=cp<=end:
                    delta=struct.unpack_from('>h',table,deltas+2*j)[0]
                    rng=struct.unpack_from('>H',table,ranges+2*j)[0]
                    gid=(cp+delta)&65535 if not rng else struct.unpack_from('>H',table,ranges+2*j+rng+2*(cp-start))[0]
                    if rng and gid: gid=(gid+delta)&65535
                    if gid: return True
    return False


def build(abf,dds,font_path,characters,output,proportional_cyrillic_metrics=False,compact_russian=False):
    from PIL import Image, ImageDraw, ImageFont
    original_abf,original_dds=Path(abf).read_bytes(),Path(dds).read_bytes()
    common,glyphs,pages=read_font(original_abf)
    if common['page_count']!=1: raise ValueError('Only single-page atlas extension supported')
    present={g['codepoint'] for g in glyphs}
    missing=sorted({ord(c) for c in characters if not c.isspace() and unicodedata.category(c) not in ('Cc','Cf')}-present)
    font_data=Path(font_path).read_bytes()
    unsupported=[f'U+{c:04X}' for c in missing if not font_has(font_data,c)]
    if unsupported: raise ValueError('Source font has no glyphs: '+', '.join(unsupported))
    rows=occupied_rows(common,glyphs,4)
    # Treat all visible unreferenced blocks as occupied, preserving images too.
    w,h=common['width'],common['height']
    if len(original_dds)!=128+w*h or original_dds[84:88]!=b'DXT5': raise ValueError('Expected single base-level BC3 DDS')
    for y in range(0,h,4):
        for x in range(0,w,4):
            off=128+((y//4)*(w//4)+x//4)*16
            if any(decode_alpha(original_dds[off:off+16])):
                for yy in range(y,y+4): rows[0][yy]|=15<<x
    placements=place_cells(rows,w,h,missing,24,4,4)
    if len(placements)!=len(missing): raise ValueError('Insufficient transparent atlas space; no output written')
    face=ImageFont.truetype(str(font_path),20)
    patched=bytearray(original_dds);records=bytearray();metrics=[]
    for cp,p in zip(missing,placements):
        # Existing Latin uses top bearings; preserve a common baseline using Pillow ascender anchor.
        bbox=face.getbbox(chr(cp),anchor='ls');left,top,right,bottom=bbox
        gw,gh=right-left,bottom-top
        if not 0<gw<=24 or not 0<gh<=24: raise ValueError(f'Glyph U+{cp:04X} exceeds 24px cell: {bbox}')
        image=Image.new('L',(24,24));ImageDraw.Draw(image).text((-left,-top),chr(cp),font=face,fill=255,anchor='ls')
        mask=image.tobytes()
        if not any(mask): raise ValueError(f'Empty raster U+{cp:04X}')
        for y in range(0,24,4):
            for x in range(0,24,4):
                values=[mask[(y+dy)*24+x+dx] for dy in range(4) for dx in range(4)]
                if not any(values): continue
                off=128+(((p['y']+y)//4)*(w//4)+(p['x']+x)//4)*16
                patched[off:off+16]=encode_white_block(values)
        advance=round(face.getlength(chr(cp)));bearing_y=20+top
        records.extend(struct.pack('>I4H3h2B',cp,p['x'],p['y'],gw,gh,left,bearing_y,advance,0,15)+b'\xFD'*12)
        metrics.append(dict(codepoint=f'U+{cp:04X}',character=chr(cp),bbox=bbox,advance=advance,bearing_y=bearing_y))
    result=bytearray()
    for _,magic,kind,payload,extra in chunks(original_abf):
        if (magic,kind)==(b'BFNT',b'HCMN'): payload=struct.pack('>I',len(glyphs)+len(missing))+payload[4:]
        elif (magic,kind)==(b'BFNT',b'CBLK'):
            if extra not in (0,len(payload)+16): raise ValueError('Unknown CBLK size convention')
            payload+=records
            if extra: extra=len(payload)+16
        result.extend(struct.pack('>4s4sII',magic,kind,len(payload),extra)+payload)
    if proportional_cyrillic_metrics:result=proportional_cyrillic(result)
    compact_report=None
    if compact_russian:
        result,patched,compact_report=compact_russian_profile(result,patched,missing)
        # Shrinking newly rasterized glyphs may leave different encodings of
        # transparent blocks behind their smaller final rectangles. Compose
        # against the independently reconstructed original compact atlas so
        # every block outside final additions has exactly canonical bytes.
        _,background,_=compact_russian_profile(original_abf,original_dds)
        _,final_glyphs,_=read_font(result)
        final_additions=occupied_rows(common,final_glyphs[len(glyphs):],4)[0]
        normalized=bytearray(patched)
        for y in range(0,h,4):
            for x in range(0,w,4):
                if final_additions[y]&(15<<x):continue
                off=128+((y//4)*(w//4)+x//4)*16
                normalized[off:off+16]=background[off:off+16]
        patched=bytes(normalized)
    report=validate_extension(original_abf,original_dds,result,patched)
    if proportional_cyrillic_metrics and not compact_russian:report['cyrillic_metrics']=cyrillic_metric_report(original_abf,result)
    if compact_report:report['compact_russian_profile']=compact_report
    report.update(source_abf_sha256=sha(original_abf),source_dds_sha256=sha(original_dds),
        font_provenance=dict(local_path=str(Path(font_path).resolve()),sha256=sha(font_data),family=face.getname(),
            distribution='Local installed font rasterization only; TTF/OTF is not copied. Font license remains applicable.'),
        abf_sha256=sha(result),dds_sha256=sha(patched),glyphs=metrics,game_rendering_verified=False)
    output=Path(output)
    if output.resolve() in (Path(abf).resolve().parent,Path(dds).resolve().parent): raise ValueError('Isolated output required')
    output.mkdir(parents=True,exist_ok=True)
    (output/Path(abf).name).write_bytes(result);(output/Path(dds).name).write_bytes(patched)
    (output/'font-extension.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--abf',type=Path,required=True);p.add_argument('--dds',type=Path,required=True)
    p.add_argument('--font',type=Path,required=True);p.add_argument('--texts',type=Path,nargs='*',default=[])
    p.add_argument('--characters',default='');p.add_argument('--output',type=Path,required=True)
    p.add_argument('--proportional-cyrillic',action='store_true',help='Explicit 66-letter Russian metrics; no atlas/Latin changes')
    p.add_argument('--compact-russian',action='store_true',help='Explicit80% ASCII/Russian/added-glyph raster and metrics profile')
    a=p.parse_args();chars=a.characters+''.join(f.read_text(encoding='utf-8-sig') for f in a.texts)
    print(json.dumps(build(a.abf,a.dds,a.font,chars,a.output,proportional_cyrillic_metrics=a.proportional_cyrillic,compact_russian=a.compact_russian),ensure_ascii=True,indent=2))


if __name__=='__main__': main()
