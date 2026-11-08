"""Regression for report msg_ce423feed63b: real-font slot limits and payload scope."""
import copy,json,struct,sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools/localization'),str(ROOT/'tools/analysis')]
from layout_variants import apply_variants,audit
from gop_strings import parse

def elf_records(data):
    count=struct.unpack_from('<I',data,40)[0];at=48;rows={}
    for _ in range(count):
        address,capacity,length,refs=struct.unpack_from('<4I',data,at);at+=16
        old=data[at:at+capacity];at+=capacity
        text=data[at:at+length].decode('utf8');at+=length
        pointers=data[at:at+4*refs];at+=4*refs
        rows[address]=dict(capacity=capacity,source=old,text=text,pointers=pointers)
    if at!=len(data):raise ValueError('Trailing ELF pack bytes')
    return rows

class RussianUILayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.old=ROOT/'final/language-packs/ru-RU-full-text-v5';cls.new=ROOT/'final/language-packs/ru-RU-full-text-v6-r2'
        cls.rules=json.loads((ROOT/'localization/ru-RU/layout-overrides-v1.json').read_text(encoding='utf8'))
        cls.project=json.loads((ROOT/'localization/ru-RU/authoring-project-v1/project.json').read_text(encoding='utf8'))
    def test_all_variants_fit_actual_compact_font_including_ink(self):
        result=audit(self.rules,(self.old/'gop/font_j24x24.abf').read_bytes())
        self.assertEqual(len(result['entries']),30);self.assertTrue(result['all_fit'])
        self.assertTrue(all(r['after']['extent']<r['before']['extent'] for r in result['entries']))
    def test_source_guard_and_locale_mismatch_rejected(self):
        for key in ['source_ja','source_sha256']:
            rules=copy.deepcopy(self.rules);rules['entries'][0][key]='changed'
            with self.assertRaisesRegex(ValueError,'source changed'):apply_variants(copy.deepcopy(self.project),rules)
        p=copy.deepcopy(self.project);p['locale']='es-ES'
        with self.assertRaisesRegex(ValueError,'locale/schema'):apply_variants(p,self.rules)
    def test_compiled_payload_exactly_matches_allowed_variants(self):
        self.assertTrue(self.new.exists(),'Build RU v6 before integration checks')
        old=elf_records((self.old/'elf-text.pack').read_bytes());new=elf_records((self.new/'elf-text.pack').read_bytes())
        self.assertEqual(old.keys(),new.keys());changed=[]
        for address,before in old.items():
            after=new[address]
            for field in ['source','pointers','capacity']:self.assertEqual(before[field],after[field])
            if before['text']!=after['text']:
                key=f'elf:string:{address-0x10000}';changed.append(key)
                rule=next(r for r in self.rules['entries'] if r['id']==key)
                self.assertEqual(after['text'],rule['ru'])
        self.assertEqual(set(changed),{r['id'] for r in self.rules['entries'] if r['id'].startswith('elf:')})
        before=parse((self.old/'gop/gop_guide.gop').read_bytes());after=parse((self.new/'gop/gop_guide.gop').read_bytes())
        changed={f"gop_guide:string:{a['index']}" for a,b in zip(after,before) if a['text']!=b['text']}
        rules={r['id']:r['ru'] for r in self.rules['entries'] if r['id'].startswith('gop_guide:')}
        self.assertEqual(changed,set(rules))
        for row in after:
            key=f"gop_guide:string:{row['index']}"
            if key in rules:self.assertEqual(row['text'],rules[key])
    def test_native_loader_applies_candidate_without_pilot_structure_mutation(self):
        import tempfile,subprocess
        from tools.tests.test_runtime_elf_text import RuntimeTextTests
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as folder:
            out=Path(folder);exe=RuntimeTextTests().compile_pilot_fixture(out)
            scope=json.loads((ROOT/'localization/ja-en-ru-review/elf-scope.json').read_text(encoding='utf8'))
            names=out/'names.tsv';names.write_text(''.join(hex(r['offset']+0x10000)+'\t'+self.project['entries'][r['id']]['text']+'\n' for r in scope['entries'] if r['kind']=='pilot'),encoding='utf8')
            result=subprocess.run([str(exe),str(ROOT/'final/english/english-preview-images-v14/EBOOT.english-launch-v22.elf'),str(self.new/'elf-text.pack'),str(names),'0xB6A330','191','216'],check=True,capture_output=True,text=True)
            self.assertIn('PASS records=1068',result.stdout)
            self.assertIn('structural_aliases_unchanged=11',result.stdout)
    def test_font_captions_native_ui_and_other_gop_identical(self):
        changed=[]
        old={p.relative_to(self.old) for p in self.old.rglob('*') if p.is_file()}
        new={p.relative_to(self.new) for p in self.new.rglob('*') if p.is_file()};self.assertEqual(old,new)
        for name in old:
            if (self.old/name).read_bytes()!=(self.new/name).read_bytes():changed.append(name.as_posix())
        self.assertEqual(set(changed),{'pack.json','coverage.json','elf-authored.json','elf-text.pack','elf-text.pack.json','gop/gop_guide.gop'})
        report=json.loads((self.new/'coverage.json').read_text(encoding='utf8'))
        self.assertEqual(report['coverage'],dict(total=30509,translated=30509,fallback=0))
if __name__=='__main__':unittest.main()
