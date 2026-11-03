"""Package contract for shader-prep assets (stage_program_assets + audit side).

    python -m unittest tools/shader_cache/test_package_contract.py
"""
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import shader_seed as ss  # noqa: E402
import stage_program_assets as spa  # noqa: E402
from test_shader_seed import Fixture  # noqa: E402

TOOL = b'MZ' + b'\0' * 64 + b'MacrossShaderPrep\0pso-library-v1.bin\0'
GAME = b'MZ' + b'\0' * 64 + b'[shader-prep] init\0[pso-library-hit]\0'


class ContractTests(Fixture):
    def setUp(self):
        super().setUp()
        names = {p.name[0]: p for p in self.cache.glob('*.bin')}
        vs = ss.parse_entry(names['V'].name, names['V'].read_bytes())
        ps = ss.parse_entry(names['P'].name, names['P'].read_bytes())
        root = b'root-signature'
        rh = ss.fnv1a(root)
        w = [0] * ss.RECORD_WORDS
        w[0], w[1], w[2] = 1, rh & 0xFFFFFFFF, rh >> 32
        for o, e in ((3, vs), (12, ps)):
            w[o:o + 5] = [ord(e['stage']), len(e['source']), e['hash'] & 0xFFFFFFFF, e['hash'] >> 32, len(e['blob'])]
            w[o + 5:o + 9] = struct.unpack('<4I', e['blob'][4:20])
        raw = struct.pack('<%dI' % ss.RECORD_WORDS, *w)
        observed = self.dir / 'observed.bin'
        ss.write_records(observed, {rh: root}, {ss.record_key(raw): raw})
        self.seed = self.dir / 'seed'
        ss.main(['build', '--from', str(self.cache), '--observed', str(observed), '--out', str(self.seed)])
        self.tool = self.dir / 'MacrossShaderPrep.exe'
        self.tool.write_bytes(TOOL)
        self.exe = self.dir / 'Macross30Recompiled.exe'
        self.exe.write_bytes(GAME)

    def files(self):
        files, summary = spa.program_files(self.seed, self.tool, self.exe)
        return files, summary

    def test_complete_inputs_produce_rows(self):
        files, summary = self.files()
        self.assertIn(spa.TOOL_PATH, files)
        self.assertIn(spa.INVENTORY_PATH, files)
        self.assertEqual(summary['entries'], 3)
        self.assertFalse(summary['local_library_shipped'])
        rows = {rel: src.read_bytes() for rel, src in files.items()}
        self.assertEqual(spa.check_package_rows(set(rows), rows.__getitem__)['entries'], 3)

    def test_all_or_nothing(self):
        self.assertEqual(spa.program_files(None, None, self.exe), ({}, None))
        with self.assertRaises(ValueError):
            spa.program_files(self.seed, None, self.exe)
        with self.assertRaises(ValueError):
            spa.program_files(None, self.tool, self.exe)

    def test_rejects_game_without_hooks_and_wrong_tool(self):
        self.exe.write_bytes(b'MZ old game')
        with self.assertRaisesRegex(ValueError, 'hooks'):
            self.files()
        self.exe.write_bytes(GAME)
        self.tool.write_bytes(b'MZ something else')
        with self.assertRaisesRegex(ValueError, 'MacrossShaderPrep'):
            self.files()

    def test_rejects_tampered_or_partial_seed(self):
        entry = next((self.seed / ss.SEED_DIR).glob('P_*.bin'))
        data = entry.read_bytes()
        entry.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        with self.assertRaisesRegex(ValueError, 'verification'):
            self.files()
        entry.write_bytes(data)
        entry.unlink()
        with self.assertRaisesRegex(ValueError, 'verification'):
            self.files()

    def test_rejects_local_gpu_artifacts(self):
        (self.seed / 'pso-library-v1.bin').write_bytes(b'RSPL')
        with self.assertRaisesRegex(ValueError, 'verification'):
            self.files()

    def test_audit_side_rejections(self):
        (self.seed / 'pso-library-v1.bin').unlink(missing_ok=True)
        files, _ = self.files()
        rows = {rel: src.read_bytes() for rel, src in files.items()}
        read = rows.__getitem__
        partial = set(rows) - {spa.INVENTORY_PATH}
        with self.assertRaisesRegex(ValueError, 'partial'):
            spa.check_package_rows(partial, read)
        extra = dict(rows)
        extra['bin/assets/rsx_shader_cache_v2/P_0000000000000000_00000001.bin'] = b'x'
        with self.assertRaisesRegex(ValueError, 'differ'):
            spa.check_package_rows(set(extra), extra.__getitem__)
        bad = dict(rows)
        name = next(n for n in rows if n.endswith('.bin') and '/V_' in n)
        bad[name] = rows[name][:-1] + bytes([rows[name][-1] ^ 1])
        with self.assertRaisesRegex(ValueError, 'hash'):
            spa.check_package_rows(set(bad), bad.__getitem__)
        local = dict(rows)
        local['bin/assets/pso-library-v1.bin'] = b'RSPL'
        with self.assertRaisesRegex(ValueError, 'local'):
            spa.check_package_rows(set(local), local.__getitem__)
        self.assertIsNone(spa.check_package_rows({'bin/Macross30Recompiled.exe'}, read))
        self.assertTrue(spa.is_allowed_bin(spa.INVENTORY_PATH))
        self.assertFalse(spa.is_allowed_bin('bin/EBOOT.bin'))
        self.assertFalse(spa.is_allowed_bin('bin/assets/rsx_shader_cache_v2/../EBOOT.bin'))


if __name__ == '__main__':
    unittest.main()
