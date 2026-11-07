"""Prepare source crops for image-generation edits, without changing game assets."""
import struct
from pathlib import Path
from PIL import Image
from english_pack import ROOT,digest,write
from build_english_ui_images import read


if __name__=='__main__':
    registry=read(ROOT/'tools/localization/catalogs/image_registry.json')
    folder=ROOT/'localization/en-US/images/loading-names-v1';folder.mkdir(parents=True,exist_ok=True)
    plan=[]
    left={12,21,22,24,33,34,56,65,66,68,81,82}
    for asset in registry['assets']:
        if asset.get('scope')!='additional_original' or '_s.dds' in asset['name']:continue
        source=ROOT/asset['source_files']['ja-JP'];data=source.read_bytes()
        if digest(data)!=asset['original_sha256']:raise ValueError('Loading original changed')
        h,w=struct.unpack_from('<II',data,12)
        if (w,h)!=(1280,720) or len(data)!=128+w*h*4:raise ValueError('Unexpected loading bitmap format')
        # Decode only; no inpainting, cropping or pixel edits are performed by Python.
        png=folder/(Path(asset['name']).stem+'-original.png')
        Image.frombytes('RGBA',(w,h),data[128:],'raw','BGRA').save(png)
        number=int(asset['name'][8:11]);x=64 if number in left else 700
        plan.append(dict(name=asset['name'],id=asset['id'],source_dds=str(source),source_sha256=asset['original_sha256'],
                         source_png=str(png),crop_png=str(folder/(Path(asset['name']).stem+'-crop.png')),
                         roi_xywh=[x,552,624 if number in left else 548,68],
                         text=asset['reported_text']['translation_en'],source_ja=asset['reported_text']['source_ja']))
    write(folder/'edit-plan.json',dict(entries=plan,edit_mode='Local name text only; preserve all pixels outside ROI.'))
    print(f'Prepared {len(plan)} original full-size PNG references for bounded name edits.')
