import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from disc_key import resolve,KEY_NAME


class DiscKeyTests(unittest.TestCase):
    def test_existing_key_reused_without_network(self):
        with tempfile.TemporaryDirectory() as td:
            (Path(td)/KEY_NAME).write_bytes(bytes(range(16)))
            with patch('disc_key.urllib.request.urlopen') as network:
                result=resolve(td,'missing',opener=network)
            self.assertTrue(result['reused']);network.assert_not_called()
            self.assertNotIn('key',result)

    def test_invalid_catalog_preserves_destination(self):
        with tempfile.TemporaryDirectory() as td:
            tool=Path(td)/'7zr.exe';tool.write_bytes(b'fixture')
            import hashlib
            with patch('disc_key.SEVENZIP_SHA256',hashlib.sha256(b'fixture').hexdigest()):
                with self.assertRaisesRegex(ValueError,'catalog hash mismatch'):
                    resolve(td,tool,opener=lambda *a,**k:io.BytesIO(b'wrong catalog'))
            self.assertFalse((Path(td)/KEY_NAME).exists())


if __name__=='__main__':unittest.main()
