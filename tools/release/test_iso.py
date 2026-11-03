from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from iso import inspect


class IsoTests(unittest.TestCase):
    def test_headers_distinguish_supported_format_from_possible_encryption(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'fixture.iso';path.write_bytes(b'fixture')
            with patch('iso.inventory',return_value=[dict(path='PS3_GAME/USRDIR/EBOOT.BIN',size=12036512)]):
                for header,kind in ((b'SCE\0','sce-self'),(b'\x7fELF','elf'),(bytes.fromhex('58a15ab9'),'unrecognized')):
                    with patch('iso.read_file',return_value=header):result=inspect(path)
                    self.assertEqual(result['eboot_format'],kind)
                    self.assertEqual(result['installer_header_supported'],kind!='unrecognized')
                    self.assertNotIn('encrypted',result)

    def test_missing_eboot_is_reported(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'fixture.iso';path.write_bytes(b'fixture')
            with patch('iso.inventory',return_value=[]):
                with self.assertRaisesRegex(ValueError,'not found'):inspect(path)


if __name__=='__main__':unittest.main()
