"""Freeze Japanese ELF sources; display and validate assistant-authored translations only."""
import argparse,hashlib,json,re,zipfile
from pathlib import Path
from gop_strings import TOKEN
ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'localization/ja-en-ru-review'
def read(p): return json.loads(Path(p).read_text(encoding='utf8'))
def prepare():
 target=BASE/'elf-scope.json'
 assert not target.exists(),'Frozen scope already exists'
 old=read(ROOT/'localization/ru-RU/scope.json');full=read(ROOT/'localization/en-US/build/full-elf-manifest.json');startup=read(ROOT/'localization/en-US/build/startup-v4-manifest.json');sources={}
 def add(rows,key):
  for r in rows:
   off=int(r[key],16) if isinstance(r[key],str) else r[key]
   sources[off]=(r['source'],r.get('typed_pointer_file_offsets',[]))
 add(full['assignments'],'destination_file_offset');add(startup['changes'],'file_offset');add(startup['pilot_changes'],'offset');add(full['extra_common_ui_changes'],'file_offset')
 with zipfile.ZipFile(ROOT/'final/exports/Macross30-en-US-v14-hd-034-draft.zip') as z:
  name=next(n for n in z.namelist() if n.endswith('review-v10/supplemental-elf-review.json'));sup=json.loads(z.read(name))
 add(sup['entries'],'offset');sources[0x8f1068]=('発進',[])
 missing=[r for r in old['elf_slots'] if r['offset'] not in sources]
 if missing:
  raw=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
  assert hashlib.sha256(raw).hexdigest()==sup['original_sha256'],'Original Japanese ELF hash mismatch'
  for r in missing:
   o=r['offset'];s=raw[o:raw.index(0,o)].decode('utf8');sources[o]=(s,[])
 rows=[]
 for r in old['elf_slots']:
  o=r['offset'];ja,ptr=sources[o];rows.append(dict(id='elf:string:'+str(o),offset=o,source_ja=ja,source_sha256=hashlib.sha256(ja.encode()).hexdigest(),installed_en=r['source'],capacity=r['capacity'],typed_pointer_file_offsets=ptr,kind=r['kind']))
 target.write_text(json.dumps(dict(source_language='ja-JP',russian_action='direct_from_Japanese',english_action='semantic_recheck',english_elf_sha256=old['english_elf_sha256'],song_policy=old['song_policy'],entries=rows),ensure_ascii=False,indent=2)+'\n',encoding='utf8')
 print(json.dumps(dict(total=len(rows),manifest_joined=len(rows)-len(missing),original_elf_fallback=len(missing))))
def show(start,count):
 for r in read(BASE/'elf-scope.json')['entries'][start:start+count]:print(json.dumps([r['offset'],r['source_ja'],r['installed_en'],r['capacity']],ensure_ascii=False))
def check(path):
 scope={str(r['offset']):r for r in read(BASE/'elf-scope.json')['entries']};d=read(path);assert d['catalog']=='elf';overflow=[]
 for k,ru in d['translations'].items():
  r=scope[k]
  for lang,t in [('ru',ru),('en',d.get('english_corrections',{}).get(k,r['installed_en']))]+([('es',d['translations_es'][k])] if k in d.get('translations_es',{}) else []):
   assert t.strip() and '\0' not in t,(k,lang)
   assert TOKEN.findall(t)==TOKEN.findall(r['source_ja']),(k,lang,'tokens')
   assert t.count('\n')==r['source_ja'].count('\n'),(k,lang,'newlines')
   if lang=='ru':assert not re.search('[\u3040-\u30ff\u3400-\u9fff\uac00-\ud7af]',t),(k,'foreign script')
  if len(ru.encode())+1>r['capacity']:overflow.append(k)
 print(json.dumps(dict(status='validated',entries=len(d['translations']),holds=len(d.get('source_holds',{})),russian_over_capacity=len(overflow),spanish_entries=len(d.get('translations_es',{})))))
def coverage():
 ids=set();es_ids=set();holds=0
 for p in (BASE/'authored').glob('elf-catalog-*.json'):
  d=read(p)
  for k in d['translations']:assert k not in ids,k;ids.add(k)
  es_ids.update(d.get('translations_es',{}));holds+=len(d.get('source_holds',{}))
 print(json.dumps(dict(reviewed=len(ids),spanish_reviewed=len(es_ids),total=len(read(BASE/'elf-scope.json')['entries']),holds=holds,remaining=len(read(BASE/'elf-scope.json')['entries'])-len(ids))))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('action',choices=['prepare','show','check','coverage']);p.add_argument('--start',type=int,default=0);p.add_argument('--count',type=int,default=40);p.add_argument('--file',type=Path);a=p.parse_args()
 if a.action=='prepare':prepare()
 elif a.action=='show':show(a.start,a.count)
 elif a.action=='check':check(a.file)
 else:coverage()
