"""Unit tests for the seed/inventory tooling (stdlib unittest, real d3dcompiler).

    python -m unittest tools/shader_cache/test_shader_seed.py
"""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import d3dc  # noqa: E402
import shader_seed as ss  # noqa: E402

PS = b'float4 main(float4 p:SV_Position):SV_Target{return float4(p.xy*0.5,0,1);}'
VS = b'float4 main(float4 p:ATTR0):SV_Position{return p;}'
BUILTIN_SRC = b'[numthreads(8,1,1)] void k(uint3 i:SV_DispatchThreadID){}'


def entry_bytes(stage, source, blob=None, entry='main', target=None, flags=0):
    if stage == 'B':
        ident = (ss.BUILTIN_TAG + b'name=t\nentry=%s\ntarget=%s\nflags1=%08X\nflags2=00000000\n\n'
                 % (entry.encode(), target.encode(), flags) + source)
        blob = blob or d3dc.compile_hlsl(source, entry, target, flags, 't')
        source = ident
    else:
        blob = blob or d3dc.compile_hlsl(source, 'main', 'vs_5_0' if stage == 'V' else 'ps_5_0', 0)
    h = ss.fnv1a(source)
    name = f'{stage}_{h:016X}_{len(source):08X}.bin'
    return name, ss.serialize_entry(stage, h, source, blob)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.cache = self.dir / 'cache'
        self.cache.mkdir()
        for stage, src, kw in (('V', VS, {}), ('P', PS, {}), ('B', BUILTIN_SRC, dict(entry='k', target='cs_5_0', flags=1 << 15))):
            name, data = entry_bytes(stage, src, **kw)
            (self.cache / name).write_bytes(data)

    def tearDown(self):
        self.tmp.cleanup()


class DxbcTests(unittest.TestCase):
    def test_checksum_matches_compiler_and_detects_corruption(self):
        blob = d3dc.compile_hlsl(PS, 'main', 'ps_5_0')
        self.assertEqual(d3dc.dxbc_checksum(blob), blob[4:20])
        self.assertTrue(d3dc.dxbc_valid(blob))
        bad = bytearray(blob)
        bad[len(bad) // 2] ^= 1
        self.assertFalse(d3dc.dxbc_valid(bytes(bad)))
        self.assertFalse(d3dc.dxbc_valid(blob[:-4]))

    def test_compile_is_deterministic(self):
        self.assertEqual(d3dc.compile_hlsl(PS, 'main', 'ps_5_0'), d3dc.compile_hlsl(PS, 'main', 'ps_5_0'))


class EntryTests(Fixture):
    def test_parse_and_reject(self):
        for p in self.cache.iterdir():
            e = ss.parse_entry(p.name, p.read_bytes())
            self.assertEqual(e['stage'], p.name[0])
        p = next(self.cache.glob('P_*.bin'))
        data = bytearray(p.read_bytes())
        with self.assertRaises(ss.EntryError):
            ss.parse_entry('P_0000000000000000_00000001.bin', bytes(data))
        data[-10] ^= 1
        with self.assertRaisesRegex(ss.EntryError, 'dxbc'):
            ss.parse_entry(p.name, bytes(data))
        data = bytearray(p.read_bytes())
        data[ss.HEADER.size] ^= 1  # source no longer hashes to its name
        with self.assertRaisesRegex(ss.EntryError, 'source-hash'):
            ss.parse_entry(p.name, bytes(data))

    def test_builtin_identity_round_trip(self):
        p = next(self.cache.glob('B_*.bin'))
        e = ss.parse_entry(p.name, p.read_bytes())
        args = ss.compile_args(e)
        self.assertEqual((args['entry'], args['target'], args['flags']), ('k', 'cs_5_0', 1 << 15))
        self.assertEqual(d3dc.compile_hlsl(args['source'], args['entry'], args['target'], args['flags']), e['blob'])


class BuildTests(Fixture):
    def build(self, name, observed=()):
        out = self.dir / name
        ss.main(['build', '--from', str(self.cache), '--observed', *map(str, observed), '--out', str(out)])
        return out

    def test_build_is_deterministic_and_verifies(self):
        a, b = self.build('a'), self.build('b')
        for p in (a / ss.SEED_DIR).iterdir():
            self.assertEqual(p.read_bytes(), (b / ss.SEED_DIR / p.name).read_bytes())
        ma = json.loads((a / ss.MANIFEST).read_text())
        mb = json.loads((b / ss.MANIFEST).read_text())
        self.assertEqual(ma['manifest_sha256'], mb['manifest_sha256'])
        self.assertEqual(ma['stages'], {'V': 1, 'P': 1, 'B': 1})
        ss.main(['verify', str(a), '--recompile'])
        victim = next((a / ss.SEED_DIR).glob('V_*.bin'))
        data = bytearray(victim.read_bytes())
        data[-3] ^= 1
        victim.write_bytes(bytes(data))
        with self.assertRaises(SystemExit):
            ss.main(['verify', str(a)])

    def test_output_must_be_new(self):
        out = self.build('c')
        with self.assertRaises(SystemExit):
            self.build('c')
        self.assertTrue(out.exists())

    def test_inventory_keeps_only_resolvable_records(self):
        names = {p.name[0]: p for p in self.cache.glob('*.bin')}
        vs = ss.parse_entry(names['V'].name, names['V'].read_bytes())
        ps = ss.parse_entry(names['P'].name, names['P'].read_bytes())
        root = b'fake-root-signature'
        root_hash = ss.fnv1a(root)

        def record(ps_entry, cull):
            w = [0] * ss.RECORD_WORDS
            w[0], w[1], w[2] = 1, root_hash & 0xFFFFFFFF, root_hash >> 32
            for o, e in ((3, vs), (12, ps_entry)):
                w[o:o + 5] = [ord(e['stage']), len(e['source']), e['hash'] & 0xFFFFFFFF, e['hash'] >> 32, len(e['blob'])]
                w[o + 5:o + 9] = struct.unpack('<4I', e['blob'][4:20])
            w[100] = cull
            return struct.pack('<%dI' % ss.RECORD_WORDS, *w)

        unknown_ps = dict(ps, hash=ps['hash'] ^ 1)
        observed = self.dir / 'observed.bin'
        ss.write_records(observed, {root_hash: root},
                         {ss.record_key(r): r for r in (record(ps, 1), record(ps, 2), record(unknown_ps, 3))})
        with observed.open('ab') as f:  # torn tail as after a crash
            f.write(struct.pack('<3I', ss.CHUNK_PSOD, ss.RECORD_BYTES, 0) + b'xx')
        roots, records, stats = ss.read_records(observed)
        self.assertEqual((len(records), stats['corrupt_tail']), (3, True))
        out = self.build('inv', [observed])
        m = json.loads((out / ss.MANIFEST).read_text())
        self.assertEqual(m['pso_inventory']['records'], 2)
        self.assertEqual(m['pso_inventory']['dropped_reasons'], {'shader-not-seeded': 1})
        ss.main(['verify', str(out)])


class CoverageTests(unittest.TestCase):
    def test_parse_runtime_lines(self):
        log = '\n'.join([
            '[shader-prep] init library=loaded hr=0x00000000 entries=300 complete=1 adapter=10DE:2489:884F1043:A1 umd=1 inventory=300 observed=4 known=304',
            '[shader-miss] stage=P source=19F3F1E7D256FEA3 bytes=28034 compile_ms=154.402 frame=14155 hr=0x00000000',
            '[shader-miss] stage=B source=0000000000000001 bytes=40 compile_ms=1.000 frame=10 hr=0x00000000',
            '[pso-miss] key=00000000000000010000000000000002 known=0 library=loaded create_ms=12.500 frame=14155',
            '[shader-prep] stats reason=compile seed_hits{v=63 p=119 b=20} user_hits{v=0 p=0 b=0} compiles{v=0 p=1 b=1} '
            'compile_ms{v=0.0 p=154.4 b=1.0} rejects{user=0/0/0 seed=0/0/0} pso{library_hits=290 created_known=0 '
            'created_unknown=1 recorded=1 bypass=0 failures=0 create_ms=12.5 max_ms=12.50 load_ms=40.0} library=loaded known=305',
        ])
        r = ss.coverage_from_log(log, {'P_19F3F1E7D256FEA3_00006D82.bin'}, from_frame=1000)
        self.assertEqual(r['shader_compiles']['total'], 2)
        self.assertEqual(r['shader_compiles']['after_from_frame'], 1)
        self.assertEqual(len(r['shader_compiles']['unexpected_in_seed']), 1)
        self.assertEqual(r['pso_creates']['unknown'], 1)
        self.assertEqual(r['final_stats']['pso']['library_hits'], '290')
        self.assertEqual(r['init']['library'], 'loaded')


if __name__ == '__main__':
    unittest.main()
