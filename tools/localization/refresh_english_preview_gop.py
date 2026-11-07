"""Refresh English draft text in the freshly staged v2 preview before launch."""
from pathlib import Path
import struct
import argparse
from english_pack import ROOT,digest,write,preview_version
from build_english_ui_images import read
from stage_gop_primary_mirrors import pidx,read_payload,fsts_entries


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version',type=preview_version,default='v2')
    parser.add_argument('--refresh-unpublished',action='store_true',help='Replace a previously checked draft only while it is inactive and has never launched')
    args=parser.parse_args();base=ROOT/('build/english-preview-images-'+args.version)
    pack=base/'PS3_GAME/USRDIR/DATA/PACK/DATA.DAT'
    index=pack.with_name('PACK.IDX')
    old=read(ROOT/'build/english-preview/preview.json')
    expected={r['name']:r['sha256'] for r in old['gop_payloads']}
    scenario=read(ROOT/'localization/en-US/structured/gop_scenariotext.json')
    expected['gop_scenariotext.gop']=scenario['source_sha256']
    quest=read(ROOT/'localization/en-US/structured/gop_mainquest.json')
    expected['gop_mainquest.gop']=quest['source_sha256']
    for catalog in (ROOT/'localization/en-US/structured').glob('*.json'):
        source=read(catalog)
        expected.setdefault(Path(source['source_file']).name,source['source_sha256'])
    if args.refresh_unpublished:
        current=read(base/'preview.json');active=read(ROOT/'localization/en-US/preview-active.json')
        if Path(active['manifest']).resolve()==(base/'preview.json').resolve() or (Path(current['run_directory'])/'process.json').exists():raise ValueError('Published or launched previews are immutable')
        if (ROOT/'localization/en-US/pack-ready.json').exists():raise ValueError('Do not revise a published English pack in place')
        pinned={r['name']:r['sha256'] for r in current['gop_files']}
        if set(pinned)!=set(expected):raise ValueError('Unpublished GOP scope changed')
        expected=pinned
    replacements={name:(ROOT/'localization/en-US/build/gop'/name).read_bytes() for name in expected}
    _,master,archives=pidx(index);_,embedded,embedded_archives=pidx(pack)
    def append(payload):
        with pack.open('ab') as stream:
            stream.write(b'\0'*(-stream.tell()%2048));off=stream.tell();stream.write(payload)
        return off
    def update(path,at,values):
        with path.open('r+b') as stream:
            stream.seek(at);stream.write(struct.pack('<'+'I'*len(values),*values))
    for name,payload in replacements.items():
        for rows in (master,embedded):
            found=[r for r in rows if r['name']==name]
            if not found or any(digest(read_payload(pack,r)[0])!=expected[name] for r in found):
                raise ValueError('English preview text epoch mismatch: '+name)
        off=append(payload)
        for destination,rows in ((index,master),(pack,embedded)):
            for row in rows:
                if row['name']==name:update(destination,row['record']+12,(off,len(payload),0))
    arc=next(r for r in archives if r['name']=='gop')
    earc=next(r for r in embedded_archives if r['name']=='gop')
    blob,_=read_payload(pack,arc);rebuilt=bytearray(blob);table=struct.unpack_from('<I',blob,8)[0]
    for row in fsts_entries(blob):
        name=Path(row['name']).name
        if name not in replacements:continue
        if digest(row['payload'])!=expected[name]:raise ValueError('Secondary English text epoch mismatch')
        rebuilt.extend(b'\0'*(-len(rebuilt)%16));off=len(rebuilt);rebuilt.extend(replacements[name])
        struct.pack_into('<3I',rebuilt,table+row['entry']*16+4,off,len(replacements[name]),0)
    off=append(rebuilt)
    for destination,row in ((index,arc),(pack,earc)):update(destination,row['record']+8,(off,len(rebuilt)))
    for metadata in (index,pack):
        _,rows,arcs=pidx(metadata)
        for row in rows:
            if row['name'] in replacements and read_payload(pack,row)[0]!=replacements[row['name']]:
                raise ValueError('GOP refresh readback failed')
        if read_payload(pack,next(r for r in arcs if r['name']=='gop'))[0]!=rebuilt:
            raise ValueError('Secondary GOP refresh failed')
    manifest=read(base/'preview.json');manifest['gop_translated_entries']=read(ROOT/'localization/en-US/build/gop-manifest.json')['translated']
    manifest['gop_primary_and_secondary_readback_verified']=True
    write(base/'preview.json',manifest)
    manifest['gop_files']=[dict(name=name,sha256=digest(payload)) for name,payload in replacements.items()]
    write(base/'preview.json',manifest)
    print(f'{len(replacements)} GOP catalogs verified in primary and secondary resource routes.')
