import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

import portable_locale as packs
from backend import Backend


class PortableLocaleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'game 한글 folder'
        self.root.mkdir()
        self.put('bin/Macross30Recompiled.exe', b'MZdummy; never executed')
        self.put('bin/EBOOT.en-US.elf', b'\x7fELFdummy')
        self.put('data/english/PS3_GAME/USRDIR/DATA/PACK/PACK.IDX', b'index')
        self.put('data/english/PS3_GAME/USRDIR/DATA/PACK/DATA.DAT', b'base data')
        self.put('data/english-hdd/BLJS10184_INSTALL/USRDIR/data/pack/data.dat', b'hdd data')
        self.put('data/hdd0/home/00000001/savedata/SAVE/data', b'USER SAVE')
        self.put('play/settings.ini', b'render_scale=3\nvsync=1\n')
        self.put('Macross30.cmd', b'@echo off\nset "MACROSS_DT_STEP=0"\n"%~1" "%~2" "%~3" 1>"%~4\\stdout.log" 2>"%~4\\stderr.log"\n')
        spec = dict(schema=1, runtime_version='0.3.9', runtime_exe_sha256=packs.digest(self.root/'bin/Macross30Recompiled.exe'),
                    english_elf_sha256=packs.digest(self.root/'bin/EBOOT.en-US.elf'), english_data_id='base-id', english_plan_sha256='base-plan',
                    pack_files=[dict(file=name,sha256=packs.digest(self.root/'data/english/PS3_GAME/USRDIR/DATA/PACK'/name)) for name in ('PACK.IDX','DATA.DAT')])
        self.put(packs.BASE_FILE, json.dumps(spec).encode())
        self.put('.local-build/language-views.json', json.dumps(dict(languages={'en-US':dict(disc='data/english',hdd0_game='data/english-hdd',data_id='base-id',plan_sha256='base-plan')})).encode())

    def put(self, name, value):
        path=self.root/name
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(value)
        return path

    def archive(self, locale='fr-CA', text=b'first', guard=None):
        files={'gop/catalog.gop':text,'text.pack':b'text','native-ui.tsv':b'UI',
               'subtitles/tracks.tsv':b'tracks','subtitles/battle.fr.tsv':b'battle'}
        doc=dict(schema_version=1,locale=locale,label='Français',status='complete',base_locale='en-US',image_locale='en-US',
                 coverage=dict(total=1,translated=1,fallback=0),
                 resources=dict(gop_overlay='gop',elf_overlay='text.pack',native_ui='native-ui.tsv',subtitles='subtitles',battle_catalog='subtitles/battle.fr.tsv'),
                 proofs=[dict(path=k,sha256=hashlib.sha256(v).hexdigest()) for k,v in files.items()],
                 source_guards=[dict(path=packs.BASE_FILE,sha256=guard or packs.digest(self.root/packs.BASE_FILE))])
        target=Path(self.temp.name)/('pack-'+hashlib.sha256(text).hexdigest()[:8]+'.zip')
        manifest=json.dumps(doc).encode()
        with zipfile.ZipFile(target,'w') as z:
            for name,data in files.items():z.writestr(name,data)
            z.writestr('pack.json',manifest)
        return target,dict(file='Macross30-text-fr-CA-0.3.9.zip',bytes=target.stat().st_size,sha256=packs.digest(target),manifest_sha256=hashlib.sha256(manifest).hexdigest())

    def test_portable_base_uses_installation_data_and_never_copies_saves(self):
        base=packs.base_profile(self.root)
        self.assertEqual(base['elf'],self.root/'bin/EBOOT.en-US.elf')
        self.assertFalse((base['state']/'hdd0/home').exists())
        self.assertEqual((self.root/'data/hdd0/home/00000001/savedata/SAVE/data').read_bytes(),b'USER SAVE')
        self.put('bin/EBOOT.en-US.elf',b'changed')
        with self.assertRaises(ValueError):packs.base_profile(self.root)

    def test_atomic_update_preserves_previous_pack_when_cache_build_fails(self):
        source,row=self.archive()
        with patch('pack_vfs.materialize_pack_vfs'):
            packs.install(self.root,source,row)
        old=(packs.registry_root(self.root)/'fr-CA/pack.json').read_bytes()
        newer,new_row=self.archive(text=b'newer')
        with patch('pack_vfs.materialize_pack_vfs',side_effect=ValueError('bad cache')):
            with self.assertRaises(ValueError):packs.install(self.root,newer,new_row)
        self.assertEqual((packs.registry_root(self.root)/'fr-CA/pack.json').read_bytes(),old)
        with patch('pack_vfs.materialize_pack_vfs'):
            installed=packs.install(self.root,newer,new_row)
        self.assertEqual((Path(installed['backup'])/'pack.json').read_bytes(),old)
        self.assertEqual(packs.installed(self.root,'fr-CA',{'locale_packs':{'fr-CA':new_row}})['current'],True)

    def test_download_hash_source_guard_and_running_lock_reject_before_replacement(self):
        source,row=self.archive()
        with self.assertRaises(ValueError):packs.install(self.root,source,{**row,'sha256':'0'*64},prepare_cache=False)
        wrong,_=self.archive(text=b'bad guard',guard='0'*64)
        with self.assertRaises(ValueError):packs.install(self.root,wrong,prepare_cache=False)
        with packs.stopped(self.root):
            with self.assertRaises((RuntimeError,BlockingIOError)):
                packs.install(self.root,source,prepare_cache=False)
        self.assertFalse((packs.registry_root(self.root)/'fr-CA').exists())

    def test_profile_activation_preserves_graphics_and_can_be_disabled(self):
        source,row=self.archive()
        packs.install(self.root,source,row,prepare_cache=False)
        folder=packs.registry_root(self.root)/'fr-CA'
        profile=dict(elf=self.root/'bin/EBOOT.en-US.elf',vfs=self.root/'data/english',state=self.root/'data',
                     subtitles=folder/'subtitles',battle_catalog=folder/'subtitles/battle.fr.tsv',elf_overlay=folder/'text.pack',native_ui=folder/'native-ui.tsv')
        before=(self.root/'play/settings.ini').read_bytes()
        with patch('portable_locale.resolve_pack',return_value=profile):
            values=packs.activate(self.root,'fr-CA')
        self.assertEqual(values['locale'],'fr-CA')
        self.assertIn('launch-locale.ps1',(self.root/'Macross30.cmd').read_text())
        self.assertIn('MACROSS_DT_STEP=0',(self.root/'Macross30.cmd').read_text())
        self.assertEqual((self.root/'play/settings.ini').read_bytes(),before)
        packs.deactivate(self.root)
        self.assertFalse((self.root/'.local-build/active-locale.json').exists())

    def test_frozen_backend_keeps_arbitrary_locale_and_rejects_unverified_catalog(self):
        cfg=Path(self.temp.name)/'settings.json';cfg.write_text(json.dumps(dict(folder=str(self.root),game_language='fr-CA')))
        backend=Backend('unused',cfg)
        self.assertEqual(backend.config['game_language'],'fr-CA')
        self.assertTrue(backend.portable_target())
        _,row=self.archive()
        with self.assertRaises(ValueError):packs.metadata({'locale_packs':{'fr-CA':{**row,'file':'../escape.zip'}}})

    def test_failed_core_update_never_installs_or_launches_new_language_pack(self):
        cfg=Path(self.temp.name)/'settings.json'
        b=Backend('unused',cfg,confirm=lambda _:True)
        b.config.update(folder=str(self.root),game_language='fr-CA')
        _,row=self.archive()
        b.manifest=dict(version='0.3.9',files={'program':{'file':'core','bytes':1}},language_packs={'en-US':{}},locale_packs={'fr-CA':row})
        b.check_status=lambda:None;b.status={'closed':False};b.stale=False
        b.inspect=lambda:None;b.download=Mock(return_value='downloaded')
        calls=[]
        def bridge(op,**kwargs):
            calls.append(op)
            if op=='inspect':return dict(core_ready=True,language_ready=True,runtime_status=1,language_status=2,program='program')
            if op=='extract':return {'path':'program'}
            if op=='stage':raise RuntimeError('core stage failed')
            if op=='launch':self.fail('Must not launch after failed core update')
            return {}
        b.bridge=bridge
        with patch('portable_locale.install') as install:
            with self.assertRaisesRegex(RuntimeError,'core stage failed'):b.play_portable()
            install.assert_not_called()
        self.assertNotIn('launch',calls)
        self.assertFalse(b.busy or b.running)

    def test_official_text_pack_download_uses_verified_cache_and_rejects_tampering(self):
        source,row=self.archive()
        channel=Path(self.temp.name)/'channel';channel.mkdir()
        artifact=channel/row['file'];artifact.write_bytes(source.read_bytes())
        b=Backend('unused',Path(self.temp.name)/'config.json',channel_dir=channel)
        b.config['folder']=str(self.root)
        cached=b.download(row)
        self.assertEqual(cached.read_bytes(),source.read_bytes())
        self.assertEqual(b.download(row),cached)
        cached.unlink();artifact.write_bytes(b'corrupted')
        with self.assertRaises(ValueError):b.download(row)
        self.assertFalse(cached.exists())
        with self.assertRaises(ValueError):b.download({**row,'file':'../escape.zip'})


if __name__=='__main__':unittest.main()
