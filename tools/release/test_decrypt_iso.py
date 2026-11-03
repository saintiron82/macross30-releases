import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from decrypt_iso import key_bytes,disc_regions,load_tool,prepare,ROOT


class DecryptTests(unittest.TestCase):
    def test_text_binary_and_invalid_key_formats(self):
        with tempfile.TemporaryDirectory() as td:
            p=Path(td)/'key'
            p.write_text('000102030405060708090A0B0C0D0E0F\n')
            self.assertEqual(key_bytes(p),bytes(range(16)))
            p.write_bytes(bytes(range(16)));self.assertEqual(key_bytes(p),bytes(range(16)))
            p.write_bytes(b'invalid')
            with self.assertRaisesRegex(ValueError,'Disc key'):key_bytes(p)

    def test_region_bounds_reject_truncated_or_overlapping_images(self):
        with tempfile.TemporaryDirectory() as td:
            p=Path(td)/'fixture.iso'
            raw=bytearray(4*2048);struct.pack_into('>I',raw,0,2);struct.pack_into('>IIII',raw,8,0,1,3,3)
            p.write_bytes(raw);self.assertEqual(disc_regions(p),[(0,1),(3,3)])
            struct.pack_into('>I',raw,16,1);p.write_bytes(raw)
            with self.assertRaisesRegex(ValueError,'overlapping'):disc_regions(p)

    def test_existing_destination_and_source_alias_are_preserved(self):
        with tempfile.TemporaryDirectory() as td:
            src,out=Path(td)/'in.iso',Path(td)/'out.iso'
            src.write_bytes(b'original');out.write_bytes(b'preserve')
            for target in (src,out):
                with self.assertRaisesRegex(ValueError,'new path'):prepare(src,target,None,'missing',None)
            self.assertEqual(src.read_bytes(),b'original');self.assertEqual(out.read_bytes(),b'preserve')


class NativeIsoToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory=ROOT/'.tools/iso-tools'
        if not (directory/'tool-manifest.json').is_file():raise unittest.SkipTest('Build pinned ISO tool first')
        cls.tool,_=load_tool(directory)

    def test_file_mode_probe_and_full_roundtrip_use_the_same_final_key(self):
        with tempfile.TemporaryDirectory(prefix='iso-native-test-') as td:
            work=Path(td);secret=bytes(range(16));(work/'key.bin').write_bytes(secret)
            raw=bytearray(4*2048)
            struct.pack_into('>I',raw,0,2);struct.pack_into('>IIII',raw,8,0,1,3,3)
            raw[4096:6144]=b'SCE\0'+bytes([0x55])*2044
            (work/'plain.iso').write_bytes(raw)
            encrypted=subprocess.run([str(self.tool),'e','file','key.bin','plain.iso','encrypted.iso'],cwd=work,
                                     capture_output=True,timeout=15)
            self.assertEqual(encrypted.returncode,0,encrypted.stderr)
            self.assertNotIn(b'Decryption key',encrypted.stdout)
            cipher=(work/'encrypted.iso').read_bytes()
            result=subprocess.run([str(self.tool),'--probe','key.bin','2'],cwd=work,input=cipher[4096:6144],
                                  capture_output=True,timeout=15)
            self.assertEqual((result.returncode,result.stdout),(0,b'SCE\0'))
            (work/'wrong.bin').write_bytes(bytes([0xff])*16)
            wrong=subprocess.run([str(self.tool),'--probe','wrong.bin','2'],cwd=work,input=cipher[4096:6144],
                                 capture_output=True,timeout=15)
            self.assertEqual(wrong.returncode,2)
            decrypted=subprocess.run([str(self.tool),'d','file','key.bin','encrypted.iso','decrypted.iso'],cwd=work,
                                     capture_output=True,timeout=15)
            self.assertEqual(decrypted.returncode,0,decrypted.stderr)
            self.assertEqual((work/'decrypted.iso').read_bytes(),raw)
            self.assertEqual((work/'plain.iso').read_bytes(),raw)


if __name__=='__main__':unittest.main()
