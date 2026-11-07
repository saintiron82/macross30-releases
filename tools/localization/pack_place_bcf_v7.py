"""DDS codec only: consume GDI-generated RGBA; preserve original DDS headers."""
import hashlib,json,struct,shutil
from pathlib import Path
from stage_ui_images import validate_dds
ROOT=Path(__file__).resolve().parents[2]
sha=lambda x:hashlib.sha256(x).hexdigest()
def main():
 p=json.loads((ROOT/'analysis/localization/region-name-audit/b-c-f-s-definitive.json').read_text(encoding='utf8'))
 folder=ROOT/'localization/image_assets/menu-ui-v7';rgba=folder/'place-bcf-rgba';out=folder/'place-bcf-replacements';out.mkdir(exist_ok=True)
 reports=[];inputs=[]
 for row in p['rows']:
  for layer in row['layers_to_replace']:
   name=layer['filename'];stem=Path(name).stem;original=(ROOT/layer['source_DDS']).read_bytes()
   assert sha(original)==layer['source_SHA256'];validate_dds(original)
   h,w=struct.unpack_from('<II',original,12);pixels=(rgba/(stem+'.rgba')).read_bytes();assert len(pixels)==w*h*4 and len(original)==128+w*h*4
   bgra=bytearray(len(pixels))
   for i in range(0,len(pixels),4):r,g,b,a=pixels[i:i+4];bgra[i:i+4]=bytes((b,g,r,a))
   new=original[:128]+bgra;validate_dds(new);assert new[:128]==original[:128] and len(new)==len(original)
   (out/name).write_bytes(new)
   for suffix in ('.png','.black.png'):shutil.copyfile(rgba/(stem+suffix),out/(stem+suffix))
   reports.append(dict(filename=name,JP=row['JP'],KO=row['KO'],source_sha256=sha(original),replacement_sha256=sha(new),dimensions=[w,h],header_changed_bytes=0,format='BGRA32',replacement_scope='Entire standalone JP-only text/glow texture; no artwork or English layer'))
   inputs.append(dict(name=name,original_sha256=sha(original),replacement=str((out/name).relative_to(ROOT)).replace('\\','/'),replacement_sha256=sha(new)))
 assert len(reports)==40
 result=dict(status='PASS_OFFLINE_CANDIDATE',words=21,files=reports,active_modified=False,game_verified=False,source_alpha_only=True,source_alpha_cutoff=64,preserved='All nonselected DDS, including b English02, c scanning02/English04, f English02/03, s00/HUD/hng Korean and illustrations remain unmodified',source_copy_inventory='analysis/localization/region-name-audit/all-current-copy-audit.json')
 (out/'manifest.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
 (out/'stage-inputs.json').write_text(json.dumps(dict(replacements=inputs),ensure_ascii=False,indent=2),encoding='utf8')
 print('PASS 21 words /40 standalone text DDS /header and dimensions exact /active unchanged')
if __name__=='__main__':main()
