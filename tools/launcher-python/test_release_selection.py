import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

import release_selection as releases
from backend import Backend

ROOT=Path(__file__).resolve().parents[2]
TMP=ROOT/'build/tests/version-selection-20261127/tmp'
TMP.mkdir(parents=True,exist_ok=True)


class ReleaseSelectionTests(unittest.TestCase):
    def test_local_choices_follow_the_verified_installation_even_with_a_different_pin(self):
        backend=Backend.__new__(Backend)
        backend.config={'game_release':'0.4.0','folder':str(TMP/'absent-installation')}
        installed=dict(core_ready=True,installed_version='0.3.10',language_ready=False)
        backend.inspect=lambda:installed
        self.assertEqual(backend.installed_game_releases(),['0.3.10'])
        installed.update(core_ready=False)
        self.assertEqual(backend.installed_game_releases(),[])

    def test_only_official_version_urls(self):
        self.assertEqual(releases.manifest_url('0.3.10'),
            'https://github.com/saintiron82/macross30-releases/releases/download/v0.3.10/BOOT-INSTALL.json')
        for invalid in ['../0.3.10','v0.3.10','https://example.com','0.3.10?x=1',None]:
            with self.assertRaises(ValueError):releases.selection(invalid)

    def test_catalog_excludes_launcher_only_and_unpublished_tags(self):
        rows=[dict(tag_name='v'+v,draft=d,prerelease=p,assets=[dict(name=n) for n in names])
              for v,d,p,names in [('0.4.0',False,False,['BOOT-INSTALL.json','Macross30-program-0.4.0.zip']),
                                 ('0.3.10',False,False,['BOOT-INSTALL.json','Macross30-program-0.3.10.zip']),
                                 ('0.3.9',False,False,['Macross30Launcher-0.3.9.exe']),
                                 ('0.5.0',True,False,['BOOT-INSTALL.json']),
                                 ('0.6.0',False,True,['BOOT-INSTALL.json'])]]
        self.assertEqual(releases.catalog(lambda *a,**kw:io.BytesIO(json.dumps(rows).encode())),
                         ['latest','0.4.0','0.3.10'])

    def test_game_pin_keeps_launcher_and_service_status_current(self):
        backend=Backend.__new__(Backend);backend.channel_dir=None
        backend.config=dict(game_release='0.3.10',ui_language='en')
        urls=[]
        def opener(request,**kw):urls.append(request.full_url);return io.BytesIO(b'{}')
        backend.opener=opener
        for name in ['BOOT-INSTALL.json','PYTHON-LAUNCHER.json','STATUS.json']:
            with backend.get(name) as response:self.assertEqual(response.read(),b'{}')
        self.assertIn('/download/v0.3.10/BOOT-INSTALL.json',urls[0])
        self.assertIn('/main/channels/stable/PYTHON-LAUNCHER.json',urls[1])
        self.assertIn('/main/channels/stable/STATUS.json',urls[2])

    def test_staged_latest_manifest_does_not_override_explicit_old_selection(self):
        with tempfile.TemporaryDirectory(dir=TMP) as folder:
            root=Path(folder);(root/'BOOT-INSTALL.json').write_text('{"version":"0.4.0"}',encoding='utf8')
            backend=Backend.__new__(Backend);backend.channel_dir=root
            backend.config=dict(game_release='0.3.10',ui_language='en')
            urls=[]
            def opener(request,**kw):urls.append(request.full_url);return io.BytesIO(b'{"version":"0.3.10"}')
            backend.opener=opener
            self.assertEqual(backend.json('BOOT-INSTALL.json')['version'],'0.3.10')
            self.assertIn('/download/v0.3.10/',urls[0])

    def test_switch_preserves_saves_and_retires_only_verified_owned_files(self):
        with tempfile.TemporaryDirectory(dir=TMP) as folder:
            root=Path(folder);program=root/'cache'
            for name,data in [('play/gamedata/user/save.bin',b'SAVE'),('play/settings.ini',b'fps_limit=60\n'),
                              ('Macross30.cmd',b'old generated command'),('bin/assets/locale-base.json',b'old guard')]:
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
            guard=root/'bin/assets/locale-base.json'
            profile=dict(release_version='0.4.0',program_files=[dict(path='bin/assets/locale-base.json',sha256=releases.digest(guard))])
            (root/'release-profile.json').write_text(json.dumps(profile),encoding='utf8')
            source=program/'base-game/Macross30.cmd';source.parent.mkdir(parents=True)
            source.write_text('@echo off\nset "_M30_EXE=%_M30_ROOT%\\bin\\Macross30Recompiled.exe"\necho selected\n',encoding='utf8')
            (program/'program-manifest.json').write_text(json.dumps(dict(files=[dict(path='base-game/Macross30.cmd',sha256=releases.digest(source))])),encoding='utf8')
            old=releases.begin_transition(root,'0.3.10')
            result=releases.sync_bootstrap(root,program,old)
            self.assertEqual(result['retired_program_files'],['bin/assets/locale-base.json'])
            self.assertFalse(guard.exists())
            self.assertEqual((root/'play/gamedata/user/save.bin').read_bytes(),b'SAVE')
            self.assertEqual((root/'play/settings.ini').read_bytes(),b'fps_limit=60\n')
            self.assertEqual((root/old['_version_backup']/'play/gamedata/user/save.bin').read_bytes(),b'SAVE')
            self.assertEqual((root/old['_version_backup']/'retired-program/bin/assets/locale-base.json').read_bytes(),b'old guard')
            self.assertIn(b'echo selected',(root/'Macross30.cmd').read_bytes())


if __name__=='__main__':unittest.main()
