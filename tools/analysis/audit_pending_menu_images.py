"""Read back recent prepared menu images from actual disc and HDD routes."""
import hashlib,json,struct,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/localization'))
from stage_gop_primary_mirrors import pidx,read_payload
from stage_nested_ui import region

def main():
    load=lambda p:json.loads((ROOT/p).read_text(encoding='utf8'))
    ko=load('localization/custom-sound/menus.json')['assets']+[load('localization/itembox-tabs/manifest.json'),load('localization/quest-badges/manifest.json'),load('localization/viewer-guide/manifest.json')['ko-KR']]
    hud=ROOT/'localization/image_assets/hud-quest-badges-v21/review.json'
    if hud.exists():ko+=json.loads(hud.read_text(encoding='utf8'))['authorities']
    en=[load('localization/viewer-guide/manifest.json')['en-US']]
    english=load('localization/en-US/pack-ready.json')
    state=ROOT/'native/analysis/vf0-hd-20261101T093620440781Z/state'
    roots=[('ko-disc',ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK',ko),
           ('ko-hdd',ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack',ko),
           ('ko-test-hdd',state/'hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack',ko),
           ('en-disc',ROOT/english['vfs']/'PS3_GAME/USRDIR/DATA/PACK',en),
           ('en-hdd',ROOT/english['state']/'hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack',en)]
    report=[]
    for label,folder,assets in roots:
        if not folder.exists():
            report.append(dict(root=label,status='ROOT_MISSING'));continue
        _,rows,archives=pidx(folder/'pack.idx');files={p.name.lower():p for p in folder.iterdir() if p.is_file()}
        expected={}
        for a in assets:expected.setdefault(a['name'],set()).add(a['replacement_sha256'])
        seen=set()
        def check(name,raw,route):
            actual=hashlib.sha256(raw).hexdigest();seen.add(name)
            report.append(dict(root=label,name=name,route=route,sha256=actual,expected=sorted(expected[name]),status='INSTALLED' if actual in expected[name] else 'PREPARED_NOT_INSTALLED'))
        for r in rows:
            if r['name'] in expected:check(r['name'],read_payload(files[Path(r['pack']).name.lower()],r)[0],'primary')
        for a in archives:
            if Path(a['pack']).name.lower() not in files:continue
            pack=files[Path(a['pack']).name.lower()];base=a['offset'];h=region(pack,base,20)
            if h[:4]!=b'FSTS':continue
            count,table,ns,nsz=struct.unpack_from('<4I',h,4)
            tab=region(pack,base+table,count*16);names=region(pack,base+ns,nsz)
            for i in range(count):
                no,off,size,zs=struct.unpack_from('<4I',tab,i*16);end=names.find(b'\0',no)
                path=names[no:end].decode('utf8');name=Path(path).name
                if name in expected:
                    raw=read_payload(pack,dict(offset=base+off,size=size,compressed_size=zs))[0]
                    check(name,raw,a['name']+'/'+path)
        for name in expected.keys()-seen:report.append(dict(root=label,name=name,status='RESOURCE_MISSING'))
    out=ROOT/'analysis/pending-menu-images-audit.json';out.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(dict(routes=len(report),installed=sum(r['status']=='INSTALLED' for r in report),pending=[r for r in report if r['status']!='INSTALLED']),ensure_ascii=True))
    # An installation stamp is justified only by the exact install record and readback.
    bundle=load('localization/image_assets/active-ui.json')['bundle']
    ready=load('localization/image_assets/'+bundle+'/ready.json')
    installation=load('localization/image_assets/'+bundle+'/installation.json')
    if installation['bundle_version']==ready['version'] and all(r['status']=='INSTALLED' for r in report if r['root']=='ko-test-hdd'):
        (state/'ui-bundle-installed.json').write_text(json.dumps(dict(bundle=bundle,version=ready['version']),indent=2),encoding='utf8')

if __name__=='__main__':main()
