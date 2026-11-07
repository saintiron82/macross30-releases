"""Decode the registered installed English DDS snapshots for visual review."""
import json
from pathlib import Path
from PIL import Image,ImageDraw
from english_pack import ROOT,digest,write

if __name__=='__main__':
    registry=ROOT/'tools/localization/catalogs/image_registry.json'
    doc=json.loads(registry.read_text(encoding='utf8'))
    rows=sorted((r for r in doc['assets'] if r['category']=='ui_image'),key=lambda r:(r['name'],r['id']))
    out=ROOT/'analysis/localization/english-v10-all-images';out.mkdir(parents=True,exist_ok=True)
    records=[]
    for at in range(0,len(rows),24):
        sheet=Image.new('RGB',(1680,1440),(24,28,32));draw=ImageDraw.Draw(sheet)
        for n,row in enumerate(rows[at:at+24]):
            path=ROOT/row['source_files']['en-US'];assert digest(path.read_bytes())==row['english_sha256']
            im=Image.open(path).convert('RGBA');im.thumbnail((414,210))
            x,y=(n%4)*420,(n//4)*240
            sheet.paste(im,(x+(420-im.width)//2,y+30+(210-im.height)//2),im)
            draw.text((x+3,y+5),row['name'],fill='white')
            records.append(dict(id=row['id'],name=row['name'],sha256=row['english_sha256'],sheet=at//24,position=n,full_resolution_source=str(path)))
        sheet.save(out/f'contact-{at//24:02}.png')
    write(out/'inventory.json',dict(registry_sha256=digest(registry.read_bytes()),entries=records,review='pending',limitation='Contact sheets do not establish in-game UV assembly or full-resolution readability'))
    print(dict(decoded=len(records),sheets=(len(records)+23)//24))
