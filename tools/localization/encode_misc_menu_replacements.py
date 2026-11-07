"""Encode GDI-approved bitmap candidates; original headers and other blocks persist."""
import argparse,json,hashlib,struct
from pathlib import Path
import sys as _sys
_sys.path[:0]=[str(p) for p in Path(__file__).resolve().parents[1].iterdir() if p.is_dir()]
from build_quest_title_bc3 import decode,encode,png,colors
from build_korean_font import alpha_palette
from stage_ui_images import validate_dds

def sha(b):return hashlib.sha256(b).hexdigest()
def pixels(data,w,h):
 if data[84:88]!=b'DXT5':return [tuple(data[i+2:i+3]+data[i+1:i+2]+data[i:i+1]+data[i+3:i+4]) for i in range(128,len(data),4)]
 result=[(0,0,0,0)]*(w*h)
 for by in range((h+3)//4):
  for bx in range((w+3)//4):
   off=128+(by*((w+3)//4)+bx)*16
   for j,p in enumerate(decode(data[off:off+16])):
    x,y=bx*4+j%4,by*4+j//4
    if x<w and y<h:result[y*w+x]=p
 return result
def constrained(block,target,editable):
 # Retain both original palettes and every protected pixel's exact indices.
 # This handles shared BC3 boundary blocks (e.g. JP logo and English row43).
 out=bytearray(block);ap=alpha_palette(block[0],block[1]);a=int.from_bytes(block[2:8],'little');c0,c1,bits=struct.unpack_from('<HHI',block,8);cp=colors(c0,c1)
 for i in editable:
  rgba=target[i];idx=min(range(8),key=lambda k:abs(ap[k]-rgba[3]));a=(a&~(7<<(i*3)))|(idx<<(i*3))
  idx=min(range(4),key=lambda k:sum((cp[k][j]-rgba[j])**2 for j in range(3)));bits=(bits&~(3<<(i*2)))|(idx<<(i*2))
 out[2:8]=a.to_bytes(6,'little');struct.pack_into('<I',out,12,bits);return bytes(out)
def main():
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('--composition',type=Path,required=True);a.add_argument('--plan',type=Path,required=True);a.add_argument('--output',type=Path,required=True);n=a.parse_args()
 if n.output.exists():raise SystemExit('New candidate directory required')
 plan=json.loads(n.plan.read_text(encoding='utf-8-sig'));comp=json.loads(n.composition.read_text(encoding='utf-8-sig'))
 sources={(r['name'],r['source_sha256']):r for r in plan['assets']};results=[];pending=[]
 for asset in comp['assets']:
  src=sources[(asset['name'],asset['source_sha256'])];original=Path(src['source_DDS']).read_bytes();assert sha(original)==src['source_sha256'];w,h=src['width'],src['height'];rgba=Path(asset['output_stem']+'.rgba').read_bytes();assert len(rgba)==w*h*4
  pts=[tuple(rgba[i:i+4]) for i in range(0,len(rgba),4)];allowed=set()
  for label in src['labels']:
   if label['glyph_id'] not in {p['glyph_id'] for p in asset['labels']}:continue
   x,y,rw,rh=label['roi_xywh'];allowed.update(yy*w+xx for yy in range(y,y+rh) for xx in range(x,x+rw))
  old=pixels(original,w,h)
  if any(old[i]!=pts[i] for i in range(w*h) if i not in allowed):raise ValueError('GDI changed protected source pixels')
  output=bytearray(original);blocks=set();constrained_count=0
  if src['format']=='BGRA8':
   for i in allowed:
    r,g,b,alpha=pts[i];output[128+i*4:128+i*4+4]=bytes((b,g,r,alpha))
   outside=0
  else:
   blocks={(i%w//4,i//w//4) for i in allowed}
   for bx,by in blocks:
    p=[pts[min(h-1,by*4+y)*w+min(w-1,bx*4+x)] for y in range(4) for x in range(4)]
    off=128+(by*((w+3)//4)+bx)*16
    editable={j for j in range(16) if (by*4+j//4)*w+bx*4+j%4 in allowed}
    keep=[j for j in range(16) if j not in editable and old[(by*4+j//4)*w+bx*4+j%4][3]>0]
    if keep:output[off:off+16]=constrained(original[off:off+16],p,editable);constrained_count+=1
    else:output[off:off+16]=encode(p)
   outside=sum(output[off:off+16]!=original[off:off+16] for off in range(128,len(original),16) if (((off-128)//16)%((w+3)//4),((off-128)//16)//((w+3)//4)) not in blocks)
  output=bytes(output);validate_dds(output);assert output[:128]==original[:128] and outside==0
  decoded=pixels(output,w,h)
  outside_visible=[i for i in range(w*h) if i not in allowed and (old[i][3]!=decoded[i][3] or old[i][3]>0 and old[i][:3]!=decoded[i][:3])]
  if outside_visible:raise ValueError(f'Visible pixels outside all approved ROI changed {src["name"]}: {len(outside_visible)}')
  outside_hidden=sum(i not in allowed and old[i][3]==decoded[i][3]==0 and old[i][:3]!=decoded[i][:3] for i in range(w*h))
  protected=[]
  if src['name']=='hangar_text00.dds':protected=[('ALL',[0,64,40,16]),('SPA',[82,64,42,16]),('digits123',[82,80,45,16]),('L1R1arrows',[0,112,80,16])]
  if src['name']=='hng_itembox_logo.dds':protected=[('ITEM BOX',[0,43,78,16]),('line',[0,60,256,2])]
  if src['name']=='course_menu00.dds':protected=[('COURSE',[0,44,53,11])]
  if src['name']=='hangar_takeoff.dds':protected=[('micro-English',[217,43,85,2])]
  protection=[]
  for name,(x,y,rw,rh) in protected:
   indexes=[yy*w+xx for yy in range(y,y+rh) for xx in range(x,x+rw)]
   bad=[i for i in indexes if old[i][3]!=decoded[i][3] or old[i][3]>0 and old[i][:3]!=decoded[i][:3]]
   if bad:raise ValueError(f'Visible protected pixels changed {src["name"]}/{name}: {len(bad)}')
   protection.append(dict(name=name,roi_xywh=[x,y,rw,rh],visible_RGBA_unchanged=True))
  stem=src['output_stem'];target=n.output/(stem+'.dds')
  row=dict(name=src['name'],source_DDS=src['source_DDS'],source_sha256=sha(original),replacement=str(target),replacement_sha256=sha(output),format=src['format'],width=w,height=h,header_changed_bytes=0,outside_approved_blocks_changed=outside,outside_ROI_visible_pixels_changed=0,outside_ROI_fully_transparent_RGB_pixels_changed=outside_hidden,approved_BC3_blocks=len(blocks),boundary_blocks_with_exact_protected_palette_and_indices=constrained_count,protected=protection,labels=asset['labels'],primary_records=src['primary_records'])
  results.append(row);pending.append((target,output,decoded,w,h))
 n.output.mkdir(parents=True)
 for target,b,px,w,h in pending:target.write_bytes(b);png(target.with_suffix('.png'),px,w,h)
 report=dict(status='VERIFIED_OFFLINE_CANDIDATES_NOT_INSTALLED',assets=results,asset_variants=len(results),unique_labels=len({x['glyph_id'] for a in results for x in a['labels']}),active_modified=False,source_composition=str(n.composition),source_plan=str(n.plan))
 (n.output/'misc-replacements-manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 print(json.dumps(dict(assets=len(results),glyphs=report['unique_labels'],protected='PASS')))
if __name__=='__main__':main()
