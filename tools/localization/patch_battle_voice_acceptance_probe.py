"""Guarded acceptance-probe patch plan. Default dry-run; --apply backs up before editing."""
import argparse
import hashlib
import json
import os
import uuid
from pathlib import Path
import unittest
from patch_battle_voice_subtitles import function_span
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'native/macross30/recompiled/ppu_recomp_004.cpp'
INCLUDE='#include "../battle_voice_acceptance_probe.h"\n'
GUARDS={'001CAF7C':'7a69bb92aa4745824937db0f1abe03b79701fb2a57ba35caade896d7699ca77b',
        '001C4BB0':'3b721c47124bccece3961d28c7502e51f58ebb7c8a9495ffeb13a0a621054665'}
HOOKS={
'001CAF7C':('void func_001CAF7C(ppu_context* ctx) {\n',
 '        macross_battle_voice_probe_request(ctx, (uint32_t)ctx->gpr[3]); // acceptance-probe: request\n'),
'001C4BB0':('        ctx->lr = 0x001C4C18; func_00704444(ctx); DRAIN_TRAMPOLINE(ctx);\n',
 '        macross_battle_voice_probe_open(ctx, (uint32_t)ctx->gpr[29], (int32_t)ctx->gpr[3]); // acceptance-probe: open-result\n')}
def sha(value):return hashlib.sha256(value).hexdigest()
def transform(raw):
 text=raw.decode('utf8');newline='\r\n' if '\r\n' in text else '\n'
 if newline=='\r\n' and '\n' in text.replace('\r\n',''):raise ValueError('Mixed newline style')
 text=text.replace('\r\n','\n')
 counts=[text.count(v[1]) for v in HOOKS.values()]
 if any(n not in (0,1) for n in counts) or (any(counts) and not all(counts)):
  raise ValueError('Partial or duplicated probe hooks')
 installed=all(counts)
 if text.count(INCLUDE)!=int(installed):raise ValueError('Probe include/hook mismatch')
 original=text.replace(INCLUDE,'',1) if installed else text
 for _,line in HOOKS.values():original=original.replace(line,'',1)
 for address,expected in GUARDS.items():
  body=function_span(original,address)[2]
  if sha(body.encode())!=expected:raise ValueError('Function SHA drift '+address)
 # Verify precise register contract and restoration order at the observed call.
 body=function_span(original,'001C4BB0')[2]
 contract=HOOKS['001C4BB0'][0]+'        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\n'
 if contract not in body or 'ctx->gpr[29] = ctx->gpr[3] | ctx->gpr[3];' not in body:
  raise ValueError('Open-result register contract drift')
 if installed:
  anchor,line=HOOKS['001C4BB0']
  if HOOKS['001CAF7C'][0]+HOOKS['001CAF7C'][1] not in text or anchor+'        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\n'+line not in text:
   raise ValueError('Installed hook position drift')
  return raw
 changed=original
 for address,(anchor,line) in HOOKS.items():
  start,end,body=function_span(changed,address)
  if body.count(anchor)!=1:raise ValueError('Ambiguous hook anchor')
  changed=changed[:start]+body.replace(anchor,anchor+line,1)+changed[end:]
 # The observer must be AFTER caller TOC restoration, before r3 is overwritten.
 anchor,line=HOOKS['001C4BB0']
 changed=changed.replace(anchor+line+'        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\n',
                         anchor+'        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\n'+line,1)
 first=changed.find('\n')+1
 changed=changed[:first]+INCLUDE+changed[first:]
 return changed.replace('\n',newline).encode('utf8')
class Fixtures(unittest.TestCase):
 def setUp(self):self.raw=SOURCE.read_bytes()
 def test_idempotence(self):
  patched=transform(self.raw);self.assertEqual(transform(patched),patched)
 def test_context_hook_order(self):
  text=transform(self.raw).decode().replace('\r\n','\n')
  anchor,line=HOOKS['001C4BB0']
  self.assertIn(anchor+'        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\n'+line,text)
 def test_function_drift_rejected(self):
  with self.assertRaisesRegex(ValueError,'SHA drift'):transform(self.raw.replace(b'void func_001C4BB0(ppu_context* ctx) {',b'void func_001C4BB0(ppu_context* ctx) { /* drift */'))
 def test_partial_rejected(self):
  patched=transform(self.raw).decode().replace('\r\n','\n').replace(HOOKS['001CAF7C'][1],'')
  with self.assertRaisesRegex(ValueError,'Partial'):transform(patched.encode())
 def test_duplicate_rejected(self):
  patched=transform(self.raw).decode().replace('\r\n','\n');line=HOOKS['001CAF7C'][1]
  with self.assertRaisesRegex(ValueError,'duplicated'):transform(patched.replace(line,line+line).encode())
 def test_battle_composition(self):
  from patch_battle_voice_subtitles import transform as battle_transform
  patched=transform(self.raw)
  repeated,proof=battle_transform(patched,'ppu_recomp_004.cpp')
  self.assertEqual(repeated,patched);self.assertTrue(proof['already_patched'])
  self.assertEqual(transform(repeated),patched)
 def test_foreign_open_code_rejected_by_battle(self):
  from patch_battle_voice_subtitles import transform as battle_transform
  patched=transform(self.raw).decode().replace('\r\n','\n')
  bad=patched.replace(HOOKS['001C4BB0'][1],HOOKS['001C4BB0'][1]+'        ctx->gpr[3] = 0;\n')
  with self.assertRaises(ValueError):battle_transform(bad.encode(),'ppu_recomp_004.cpp')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--self-test',action='store_true')
 p.add_argument('--apply',action='store_true');p.add_argument('--report',type=Path);args=p.parse_args()
 if args.self_test:
  if args.apply or args.report:p.error('--self-test cannot apply/write reports')
  result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Fixtures))
  raise SystemExit(0 if result.wasSuccessful() else 1)
 raw=SOURCE.read_bytes();patched=transform(raw)
 report={'status':'applied' if args.apply else 'dry_run_only_not_applied','source':str(SOURCE),
  'before_sha256':sha(raw),'after_sha256':sha(patched),'changed':raw!=patched,'hook_count':2,
  'function_guards':GUARDS,'required_build_source':'native/macross30/battle_voice_acceptance_probe.cpp',
  'default_enabled':False,'backup':None}
 if args.report:
  destination=args.report.resolve()
  if not destination.is_relative_to(ROOT) or destination==SOURCE:raise ValueError('Unsafe report path')
 if args.apply and raw!=patched:
  cmake=(ROOT/'native/macross30/CMakeLists.txt').read_text(encoding='utf8')
  if '    battle_voice_acceptance_probe.cpp\n' not in cmake:raise ValueError('Probe is not registered in CMake')
  folder=ROOT/'native/analysis/battle-voice-acceptance-probe-backups';folder.mkdir(parents=True,exist_ok=True)
  backup=folder/(sha(raw)+'.cpp');
  if backup.exists() and backup.read_bytes()!=raw:raise ValueError('Backup collision')
  backup.write_bytes(raw)
  temp=SOURCE.with_name(SOURCE.name+'.acceptance-'+uuid.uuid4().hex+'.tmp')
  try:
   temp.write_bytes(patched)
   if SOURCE.read_bytes()!=raw:raise ValueError('Concurrent generated source change')
   os.replace(temp,SOURCE)
  finally:
   if temp.exists():temp.unlink()
  report['backup']=str(backup)
 if args.report:
  destination.parent.mkdir(parents=True,exist_ok=True)
  destination.write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
 print(json.dumps(report,indent=2))
if __name__=='__main__':main()
