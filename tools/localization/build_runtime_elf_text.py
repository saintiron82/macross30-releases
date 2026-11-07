"""Compile a data-only, locale-agnostic guarded runtime ELF text pack.
No translation generation; texts must be explicitly authored. No ELF writes.
"""
import argparse,hashlib,json,struct
from pathlib import Path
MAGIC=b'M30TXP1\0'
def segments(blob):
 if blob[:6]!=b'\x7fELF\x02\x02':raise ValueError('Expected big-endian ELF64')
 off=struct.unpack_from('>Q',blob,32)[0];stride,count=struct.unpack_from('>HH',blob,54)
 result=[]
 if stride<56 or off+stride*count>len(blob):raise ValueError('Invalid program headers')
 for i in range(count):
  typ,flags,file,va,_,size,mem,align=struct.unpack_from('>IIQQQQQQ',blob,off+i*stride)
  if typ==1 and size:
   if file+size>len(blob) or va+mem>2**32:raise ValueError('Invalid segment')
   result.append((file,va,size,flags))
 return result
def guest(segs,offset,size):
 for file,va,length,flags in segs:
  if file<=offset and offset+size<=file+length:return va+offset-file,flags
 raise ValueError(f'Unmapped ELF offset {offset:#x}')
def build(scope_path,elf_path,texts_path,out,bindings_path=None):
 scope=json.loads(Path(scope_path).read_text(encoding='utf8'));blob=Path(elf_path).read_bytes();digest=hashlib.sha256(blob).hexdigest()
 if digest!=scope['english_elf_sha256']:raise ValueError('Frozen English baseline SHA256 mismatch')
 segs=segments(blob);doc=json.loads(Path(texts_path).read_text(encoding='utf8'));texts=doc.get('translations',doc);bindings={}
 if bindings_path:
  extended=json.loads(Path(bindings_path).read_text(encoding='utf8'))
  if extended.get('status')!='approved_source_guarded_display_data_refs' or extended['english_elf_sha256']!=digest or extended['source_scope_sha256']!=hashlib.sha256(Path(scope_path).read_bytes()).hexdigest():raise ValueError('Display binding provenance mismatch')
  for row in extended['entries']:
   key=str(row['offset'])
   if key in bindings:raise ValueError('Duplicate display binding')
   bindings[key]=row
 known={str(r['offset']):r for r in scope['entries']}
 if set(texts)-set(known):raise ValueError('Unknown translation offsets')
 records=[];fallback=[];relocated=inline=0
 for key,r in known.items():
  if key not in texts:fallback.append(dict(offset=r['offset'],reason='missing_translation',language='English',test_only=True));continue
  text=texts[key]
  if not isinstance(text,str) or not text.strip() or '\0' in text:raise ValueError(f'Invalid text: {key}')
  target=text.encode('utf8');offset=r['offset'];cap=r['capacity'];expected=blob[offset:offset+cap]
  if len(expected)!=cap or expected.split(b'\0',1)[0]!=r['installed_en'].encode():raise ValueError(f'Source guard mismatch: {key}')
  source,_=guest(segs,offset,cap);refs=[];extra=bindings.get(key)
  if extra and (extra['source_sha256']!=r['source_sha256'] or extra['installed_en']!=r['installed_en'] or extra['source_guest_ea']!=source):raise ValueError('Display binding source changed')
  pointer_offsets=list(r.get('typed_pointer_file_offsets',[]))+(extra['typed_pointer_file_offsets'] if extra else [])
  # Pilot names are inline char[64] fields in indexed 0xD8 records. An exact
  # address match can be a STRUCTURE BASE, not a display-string reference.
  # Relocating it corrupts table indexing (the first name aliases table[0]).
  if r.get('kind')=='pilot':pointer_offsets=[]
  for p in pointer_offsets:
   pos=int(p,0) if isinstance(p,str) else p;ea,flags=guest(segs,pos,4)
   if pos%4 or not(flags&2) or struct.unpack_from('>I',blob,pos)[0]!=source:raise ValueError(f'Typed pointer mismatch: {key}/{pos:#x}')
   if ea not in refs:refs.append(ea)
  if not refs and len(target)+1>cap:
   fallback.append(dict(offset=offset,reason='over_capacity_without_verified_pointer',language='English',test_only=True,required=len(target)+1,capacity=cap));continue
  if not refs and any(expected[len(r['installed_en'].encode())+1:]):raise ValueError(f'Nonzero inline padding: {key}')
  records.append(struct.pack('<IIII',source,cap,len(target),len(refs))+expected+target+b''.join(struct.pack('<I',p) for p in refs))
  relocated+=bool(refs);inline+=not refs
 data=MAGIC+bytes.fromhex(digest)+struct.pack('<II',len(records),1)+b''.join(records)
 out=Path(out);out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data)
 report=dict(schema='macross.runtime-elf-text.v1',test_only=True,baseline_elf_sha256=digest,pack_sha256=hashlib.sha256(data).hexdigest(),records=len(records),relocated=relocated,in_place=inline,total=len(known),fallback_count=len(fallback),fallback=fallback,source_scope_sha256=hashlib.sha256(Path(scope_path).read_bytes()).hexdigest(),display_bindings_sha256=hashlib.sha256(Path(bindings_path).read_bytes()).hexdigest() if bindings_path else None)
 out.with_suffix(out.suffix+'.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf8');return report
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--scope',required=True);p.add_argument('--baseline-elf',required=True);p.add_argument('--texts',required=True);p.add_argument('--output',required=True);p.add_argument('--bindings');a=p.parse_args();r=build(a.scope,a.baseline_elf,a.texts,a.output,a.bindings);print(json.dumps({k:v for k,v in r.items() if k!='fallback'}))
