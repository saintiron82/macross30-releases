import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from backend import Backend
from iso_service import analyze


class IsoServiceTests(unittest.TestCase):
    def fixture(self):
        td=tempfile.TemporaryDirectory();self.addCleanup(td.cleanup)
        root=Path(td.name);iso=root/'game.iso';iso.write_bytes(b'fixture')
        key=root/'game.dkey';key.write_text('00'*16)
        return root,iso,key

    def test_bad_key_has_a_distinct_status(self):
        _,iso,key=self.fixture()
        with patch('iso_service.inspect',return_value={'installer_header_supported':False}), \
             patch('iso_service.disc_regions',return_value=[(0,1),(3,3)]), \
             patch('iso_service.eboot_sector',return_value=(2,b'cipher')), \
             patch('iso_service.probe',side_effect=ValueError('Disc key does not produce a recognized EBOOT header')):
            result=analyze(iso,key,'unused',None)
        self.assertEqual((result['kind'],result['key_status']),('encrypted','mismatch'))

    def test_invalid_key_format_is_not_reported_as_a_mismatching_valid_key(self):
        _,iso,key=self.fixture();key.write_text('bad')
        with patch('iso_service.inspect',return_value={'installer_header_supported':False}), \
             patch('iso_service.disc_regions',return_value=[(0,1),(3,3)]), \
             patch('iso_service.eboot_sector',return_value=(2,b'cipher')):
            result=analyze(iso,key,'unused',None)
        self.assertEqual(result['key_status'],'invalid-format')

    def test_missing_or_wrong_key_prevents_download_staging_and_installer(self):
        root,iso,_=self.fixture()
        for state in ('missing','mismatch','invalid-format'):
            backend=Backend('unused',root/'settings.json')
            backend.config['iso']=str(iso)
            backend.manifest={'version':'0.3.4'}
            backend.installed={'core_ready':False}
            backend.inspect=lambda:backend.installed
            backend.bridge=lambda *a,**k:None
            backend.inspect_iso=lambda:dict(kind='encrypted',key_status=state,sha1='a'*40)
            with patch.object(backend,'download') as download,patch.object(backend,'installer') as installer:
                with self.assertRaises(ValueError):backend.play()
                download.assert_not_called();installer.assert_not_called()
            self.assertFalse(backend.busy)

    def test_iso_and_manual_key_settings_preserve_install_location(self):
        root,iso,key=self.fixture();backend=Backend('unused',root/'settings.json')
        backend.inspect=lambda:None
        previous=backend.config['folder']
        backend.settings(iso=str(iso),disc_key=str(key))
        saved=json.loads((root/'settings.json').read_text())
        self.assertEqual(saved['folder'],previous)
        self.assertEqual(saved['disc_key'],str(key))
        self.assertNotIn('00'*16,(root/'settings.json').read_text())


if __name__=='__main__':unittest.main()
