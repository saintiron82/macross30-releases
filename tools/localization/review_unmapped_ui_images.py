"""Create diagnostic contact sheets of original UI candidates outside the register."""
import io
import json
import argparse
import re
from pathlib import Path
from PIL import Image,ImageDraw
from english_pack import ROOT,write,digest
from stage_gop_primary_mirrors import pidx,read_payload

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ark-discovery',action='store_true');args=parser.parse_args()
    inventory=json.loads((ROOT/'analysis/localization/texture_inventory.json').read_text(encoding='utf8'))['entries']
    registered={r['name'] for r in json.loads((ROOT/'tools/localization/catalogs/image_registry.json').read_text(encoding='utf8'))['assets']}
    names={r['archive_entry'] for r in inventory if r['review_priority']=='inspect'}-registered
    if args.ark_discovery:
        refs=set()
        for ark in (ROOT/'analysis/ui_assets').glob('*.ark'):
            refs.update(Path(n.decode('ascii')).name for n in re.findall(rb'[a-zA-Z0-9_./-]+\.dds',ark.read_bytes()))
        names=refs-registered-{r['archive_entry'] for r in inventory if r['review_priority']=='inspect'}
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK';_,rows,_=pidx(pack/'PACK.IDX');paths={p.name.lower():p for p in pack.iterdir()}
    out=ROOT/('analysis/localization/english-ark-discovery' if args.ark_discovery else 'analysis/localization/english-unmapped-images');out.mkdir(parents=True,exist_ok=True);records=[];seen=set();tiles=[]
    for row in rows:
        if row['name'] not in names:continue
        raw,_=read_payload(paths[Path(row['pack']).name.lower()],row);sha=digest(raw)
        if (row['name'],sha) in seen:continue
        seen.add((row['name'],sha));image=Image.open(io.BytesIO(raw)).convert('RGBA');filename=Path(row['name']).stem+'-'+sha[:8]+'.png';image.save(out/filename)
        tile=Image.new('RGB',(420,200),(40,44,50));image.thumbnail((414,174));tile.paste(image,((420-image.width)//2,24+(174-image.height)//2),image);ImageDraw.Draw(tile).text((4,4),row['name'],fill='white');tiles.append(tile)
        records.append(dict(name=row['name'],sha256=sha,png=str(out/filename),source=row,review='pending'))
    for at in range(0,len(tiles),24):
        sheet=Image.new('RGB',(1680,1200),(20,24,28))
        for n,tile in enumerate(tiles[at:at+24]):sheet.paste(tile,((n%4)*420,(n//4)*200))
        sheet.save(out/f'contact-{at//24:02}.png')
    write(out/'inventory.json',dict(entries=records,diagnostic_only=True,game_assets_modified=False))
    print(dict(unregistered_names=len(names),unique_images=len(records),sheets=(len(tiles)+23)//24))
