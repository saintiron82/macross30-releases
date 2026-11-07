"""Verify bounded DDS edits independently of the native bitmap compositor."""
import struct
from pathlib import Path
from english_pack import ROOT,digest,write
from build_english_ui_images import read

if __name__=='__main__':
    folder=ROOT/'localization/en-US/images/loading-names-v1'
    manifest=read(folder/'manifest.json')
    registry={r['name']:r for r in read(ROOT/'tools/localization/catalogs/image_registry.json')['assets'] if r.get('scope')=='additional_original'}
    assert len(manifest['assets'])==44
    for row in manifest['assets']:
        original=(ROOT/registry[row['name']]['source_files']['ja-JP']).read_bytes()
        output=Path(row['replacement']).read_bytes()
        assert digest(original)==row['source_sha256']
        assert original[:128]==output[:128] and len(original)==len(output)
        h,w=struct.unpack_from('<II',original,12)
        x,y,rw,rh=row['roi_xywh'];assert 0<=x<x+rw<=w and 0<=y<y+rh<=h
        changed=0
        for line in range(h):
            start=128+line*w*4;end=start+w*4
            if y<=line<y+rh:
                left=start+x*4;right=left+rw*4
                assert original[start:left]==output[start:left] and original[right:end]==output[right:end]
                changed+=sum(a!=b for a,b in zip(original[left:right],output[left:right]))
            else:assert original[start:end]==output[start:end]
        assert changed>0
        row.update(replacement_sha256=digest(output),outside_roi_unchanged=True,dds_header_unchanged=True,changed_bytes=changed,visual_game_review='pending')
    write(folder/'manifest.json',manifest)
    print('44 DDS headers and all pixels outside the name ROI verified unchanged.')
