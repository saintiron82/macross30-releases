"""Inventory original raw JOBCRT14 images; require header/footer and adjacent pointer-size corroboration. No registration."""
import pathlib,struct,json,hashlib,sys,re
R=pathlib.Path(__file__).resolve().parents[2];sys.path.insert(0,str(R/'vendor/ps3recomp-master/tools'));from spu_disasm import disassemble_spu
b=(R/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes();po=struct.unpack_from('>Q',b,32)[0];es,n=struct.unpack_from('>HH',b,54);loads=[]
for i in range(n):
 p=po+i*es
 if struct.unpack_from('>I',b,p)[0]==1:loads.append((struct.unpack_from('>Q',b,p+8)[0],struct.unpack_from('>Q',b,p+16)[0],struct.unpack_from('>Q',b,p+32)[0]))
O=R/'native/analysis/raw-jobcrt-inventory';O.mkdir(exist_ok=True);rows=[];pos=0
while True:
 marker=b.find(b'JOBCRT Ver14',pos)
 if marker<0:break
 pos=marker+1;start=marker-0x30;size=struct.unpack_from('>I',b,start+0x3c)[0]+0x40
 assert start>=0 and 0x100<size<=0x40000 and start+size<=len(b)
 data=b[start:start+size];assert data[0x20:0x30]==b'\xa5'*16;assert data[-48:-32]==b'%SPURS JOB INFO%';assert all(i.mnemonic=='ila' for i in disassemble_spu(data[:16],0))
 assert data[0x10:0x20]==bytes.fromhex('44012850320000804401285032001880')
 ea=next(va+start-fo for fo,va,sz in loads if fo<=start<fo+sz);refs=[];i=0
 while True:
  i=b.find(struct.pack('>I',ea),i)
  if i<0:break
  for delta in [-4,4]:
   if struct.unpack_from('>I',b,i+delta)[0]==size:refs.append({'pointer_file_offset':hex(i),'size_file_offset':hex(i+delta),'size':size,'bytes':b[i-8:i+12].hex()})
  i+=1
 assert refs,'No independently adjacent exact static pointer/size found'
 tail=data.find(b'\n_exit() has been called!!');assert tail>=0
 if data[tail-4:tail]==bytes.fromhex('35000000'):end=tail
 else:end=tail-8;assert data[end:tail]==bytes(8) and data[end-4:end]==bytes.fromhex('35000000')
 ii=disassemble_spu(data[:0x20],0)+disassemble_spu(data[0x60:end],0x60);entries={i.addr for i in ii};outside=[]
 for i in ii:
  if i.mnemonic in ('br','bra','brsl','brasl','brz','brnz','brhz','brhnz'):
   target=int(re.findall(r'0x[0-9A-Fa-f]+',i.operands)[-1],16)
   if target not in entries:outside.append([hex(i.addr),hex(target)])
 fp=1469598103934665603
 for x in data:fp=((fp^x)*1099511628211)&0xffffffffffffffff
 path=O/f'job_{fp:016x}.bin';path.write_bytes(data)
 row={'original_file_offset':hex(start),'original_guest_EA':hex(ea),'size':size,'header_size_formula':'be32(header+0x3C)+0x40','header_size_independently_corroborated_by_static_pointer_size':refs,'sdk_fingerprint':f'{fp:016x}','sha256':hashlib.sha256(data).hexdigest(),'code_ranges':[['0x0','0x20'],['0x60',hex(end)]],'entry_count':len(ii),'tail_start':hex(tail),'tail_padding_bytes':tail-end,'unsupported_decode':[{'pc':hex(i.addr),'word':hex(i.raw)} for i in ii if i.mnemonic=='.word'],'outside_direct_branches':outside,'analysis_binary':str(path.relative_to(R)).replace('\\','/'),'runtime_dispatch_observed':fp in [0xb8c682fceff9d543,0x84665e22d700ddb1],'registration':'No automatic registration by inventory; exactsize established statically, opcode/lift/ABI review still required'};rows.append(row)
out={'source':'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf','source_sha256':hashlib.sha256(b).hexdigest(),'marker_count':len(rows),'scope':'Original rawJOBCRT14 only; doesnotenumerate allrawedgejobs orpackfilejobs','jobs':rows};(O/'inventory.json').write_text(json.dumps(out,indent=2));print(json.dumps(rows,indent=2))

