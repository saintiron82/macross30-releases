import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import patch, Mock

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'play'))
from backend import Backend, detect_workspace_root
import language_packs
from pack_selection import read_selection


class WorkspacePackLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        script=self.root/'tools/play/play_native.py';script.parent.mkdir(parents=True);script.write_text('# trusted workspace entry')
        (self.root/'native/play').mkdir(parents=True)
        self.config=self.root/'native/local-launcher/python-settings.json';self.config.parent.mkdir(parents=True)
        self.config.write_text(json.dumps(dict(game_language='fr-CA',ui_language='ko')))
        manifest=self.root/'localization/installed-packs/fr-CA/pack.json';manifest.parent.mkdir(parents=True)
        manifest.write_text('{"locale":"fr-CA","status":"partial_test"}')
        self.pack=dict(locale='fr-CA',label='Français',ready=False,status='partial_test',selectable_preview=True)
        self.discovery=patch.object(language_packs,'discover_language_packs',return_value=[self.pack]).start()
        self.resolve=patch.object(language_packs,'resolve_language',return_value={}).start()
        self.addCleanup(patch.stopall)
        self.events=[]
        self.backend=Backend(self.root/'absent-bridge.exe',self.config,workspace_root=self.root,emit=self.events.append)
        self.backend.bridge=Mock(side_effect=AssertionError('Workspace cannot invoke release Bridge'))
        self.backend.opener=Mock(side_effect=AssertionError('Workspace cannot download'))

    def test_workspace_check_lists_arbitrary_locale_without_bridge_or_network(self):
        state=self.backend.check()
        self.assertEqual(state['local_packs'][0]['locale'],'fr-CA')
        self.assertEqual(state['config']['game_language'],'fr-CA')
        self.assertEqual(state['config']['folder'],str(self.root/'native'))
        self.assertIsNone(state['manifest'])
        self.assertFalse(state['local_allow_partial'])
        self.backend.bridge.assert_not_called()
        self.backend.opener.assert_not_called()

    def test_import_selects_pack_but_does_not_silently_consent(self):
        with patch.object(language_packs,'import_language_pack',return_value=self.pack) as importer:
            self.backend.import_local_pack(self.root/'custom.zip')
        importer.assert_called_once_with(self.root,self.root/'custom.zip')
        self.assertEqual(self.backend.config['game_language'],'fr-CA')
        self.assertFalse(self.backend.local_allow_partial)
        self.assertFalse((self.root/'native/play/language-pack-consent.json').exists())
        with self.assertRaises(ValueError):self.backend.settings(game_language='not-installed')

    def test_play_partial_requires_checkbox_then_runs_trusted_local_entry(self):
        self.backend.check()
        with patch('backend.external_run') as run:
            with self.assertRaises(ValueError):self.backend.play()
            run.assert_not_called()
        self.backend.allow_local_preview(True)
        with patch('backend.external_run',return_value=subprocess.CompletedProcess([],0,'finished')) as run:
            self.backend.play()
        args=run.call_args.args[0]
        self.assertEqual(args,[sys.executable,'-X','utf8',str(self.root/'tools/play/play_native.py'),'--language','fr-CA','--allow-partial-language'])
        self.assertEqual(run.call_args.kwargs['cwd'],str(self.root))
        self.assertEqual(run.call_args.kwargs['env']['PYTHONIOENCODING'],'utf-8')
        self.assertEqual(run.call_args.kwargs['env']['PYTHONUTF8'],'1')
        self.assertEqual(read_selection(self.root/'native/play/settings.ini'),('fr-CA',True))
        self.assertFalse(self.backend.running or self.backend.busy)
        self.backend.bridge.assert_not_called()

    def test_real_python_child_prints_preview_notice_under_korean_parent_encoding(self):
        # Reproduce the launcher's redirected pipe, without starting the game.
        script=self.root/'tools/play/play_native.py'
        script.write_text('print("Español — partial preview 🚀", flush=True)\n',encoding='utf-8')
        self.backend.check();self.backend.allow_local_preview(True)
        with patch.dict(os.environ,dict(PYTHONIOENCODING='cp949',PYTHONUTF8='0')):
            self.backend.play()
        self.assertEqual(self.backend.last_log.read_text(encoding='utf-8'),'Español — partial preview 🚀\n')
        self.assertFalse(self.backend.running or self.backend.busy)

    def test_bad_proofs_prevent_process_and_receipt(self):
        self.backend.check();self.backend.allow_local_preview(True)
        self.resolve.side_effect=ValueError('Payload checksum mismatch')
        with patch('backend.external_run') as run:
            with self.assertRaisesRegex(ValueError,'checksum'):self.backend.play()
            run.assert_not_called()
        self.assertFalse((self.root/'native/play/language-pack-consent.json').exists())
        self.assertFalse(self.backend.running or self.backend.busy)

    def test_child_failure_is_visible_and_state_resets(self):
        self.backend.check();self.backend.allow_local_preview(True)
        with patch('backend.external_run',return_value=subprocess.CompletedProcess([],1,'candidate validation failed')):
            with self.assertRaisesRegex(RuntimeError,'candidate validation failed'):self.backend.play()
        self.assertFalse(self.backend.running or self.backend.busy)
        self.assertIn('candidate validation failed',self.backend.last_log.read_text())

    def test_main_ui_uses_dynamic_locale_id_and_skips_release_startup(self):
        from app import Launcher
        calls=[]
        fake=SimpleNamespace(preview=False,worker_active=True,readonly_worker=True,render=lambda:None,locale=SimpleNamespace(current=lambda:3),
            locale_ids=['ko-KR','en-US','ja-JP','fr-CA'],backend=SimpleNamespace(
                workspace_root=self.root,settings=lambda **kw:calls.append(kw),check=lambda:calls.append('local-check'),snapshot=lambda:{} ),
            run=lambda action:action(),args=SimpleNamespace(skip_launcher_update=True))
        Launcher.configure_language(fake,False)
        Launcher.startup(fake)
        self.assertEqual(calls,[dict(game_language='fr-CA'),'local-check'])

    def test_local_graphics_write_actual_settings_preserving_language_and_consent(self):
        settings=self.root/'native/play/settings.ini'
        original=b'# options\nrender_scale=1.5\nfullscreen=0\nvsync=1\nsubtitles=1\nlanguage=fr-CA\nlanguage_allow_partial=1\nfps_unlock=0\n'
        settings.write_bytes(original)
        receipt=settings.with_name('language-pack-consent.json');receipt.write_bytes(b'consent unchanged')
        hd=self.root/'native/assets/aircraft-hd.pack';hd.parent.mkdir(parents=True);hd.write_bytes(b'fixture')
        self.backend.config['folder']=str(self.root/'wrong-release-folder')
        document=self.backend.load_launch_options()
        self.assertEqual(document['path'],str(settings))
        self.assertTrue(document['hd_available'])
        self.backend.save_launch_options(document,dict(render_scale='3',fullscreen='1',vsync='0',subtitles='0',hd_textures='1'))
        self.assertTrue(settings.read_bytes().startswith(original))
        from launch_options import parse,RESOLUTIONS
        self.assertEqual(list(RESOLUTIONS),['1','1.25','1.5','2','2.5','3'])
        self.assertEqual(parse(settings.read_bytes())['render_scale'],'3')
        self.assertEqual(parse(settings.read_bytes())['hd_textures'],'1')
        self.assertEqual(receipt.read_bytes(),b'consent unchanged')
        self.assertEqual(settings.read_bytes().count(b'language=fr-CA'),1)
        self.assertEqual(settings.read_bytes().count(b'fps_unlock=0'),1)
        self.backend.bridge.assert_not_called()

    def test_local_graphics_running_or_external_lease_never_writes(self):
        settings=self.root/'native/play/settings.ini';settings.write_bytes(b'vsync=1\nlanguage=fr-CA\n')
        document=self.backend.load_launch_options();original=settings.read_bytes()
        self.backend.running=True
        with self.assertRaises(RuntimeError):self.backend.save_launch_options(document,dict(vsync='0'))
        self.backend.running=False
        # Hold the real shared runner lease in another process; no game starts.
        code='import msvcrt,sys; f=open(sys.argv[1],"a+b"); f.seek(0); msvcrt.locking(f.fileno(),msvcrt.LK_NBLCK,1); print("ready",flush=True); sys.stdin.read()'
        process=subprocess.Popen([sys.executable,'-c',code,str(self.root/'native/runner.lock')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(),'ready')
            with self.assertRaisesRegex(RuntimeError,'running game'):
                self.backend.save_launch_options(document,dict(vsync='0'))
        finally:
            process.communicate('',timeout=5)
        self.assertEqual(settings.read_bytes(),original)
        self.backend.bridge.assert_not_called()

    def test_local_graphics_missing_hd_and_stale_settings_are_safe(self):
        settings=self.root/'native/play/settings.ini';settings.write_bytes(b'vsync=1\n')
        document=self.backend.load_launch_options();self.assertFalse(document['hd_available'])
        self.backend.save_launch_options(document,dict(hd_textures='1',vsync='0'))
        self.assertNotIn(b'hd_textures=1',settings.read_bytes())
        with self.assertRaisesRegex(ValueError,'Settings changed'):
            self.backend.save_launch_options(document,dict(vsync='0'))
        with self.assertRaises(ValueError):
            self.backend.save_launch_options(self.backend.load_launch_options(),dict(fps_unlock='1'))

    def test_metadata_seed_is_fast_and_play_waits_for_full_verification(self):
        self.discovery.assert_called_with(self.root,verify=False)
        self.assertEqual(self.backend.local_packs[0]['locale'],'fr-CA')
        self.assertFalse(self.backend.local_verified)
        with patch('backend.external_run') as run:
            with self.assertRaisesRegex(RuntimeError,'verification'):self.backend.play()
            run.assert_not_called()

    def test_readonly_verification_preserves_user_changes_and_allows_graphics(self):
        entered=threading.Event();release=threading.Event();errors=[]
        packs=[self.pack,dict(locale='es-ES',label='Español',status='partial_test',ready=False,selectable_preview=True)]
        def discover(root,verify=True):
            if verify:
                entered.set()
                if not release.wait(5):raise TimeoutError('test verification stalled')
            return [dict(p,verification='verified' if verify else 'pending') for p in packs]
        self.discovery.side_effect=discover
        self.backend.refresh_local_packs(verify=False)
        settings=self.root/'native/play/settings.ini';settings.write_bytes(b'vsync=1\nlanguage=fr-CA\n')
        def check():
            try:self.backend.check()
            except Exception as error:errors.append(error)
        worker=threading.Thread(target=check);worker.start()
        try:
            self.assertTrue(entered.wait(2))
            self.assertTrue(self.backend.local_checking)
            self.assertFalse(self.backend.busy)
            self.backend.settings(game_language='es-ES',ui_language='en')
            self.backend.allow_local_preview(True)
            document=self.backend.load_launch_options()
            self.backend.save_launch_options(document,dict(vsync='0'))
            from launch_options import parse
            self.assertEqual(parse(settings.read_bytes())['vsync'],'0')
            with patch('backend.external_run') as run:
                with self.assertRaisesRegex(RuntimeError,'verification'):self.backend.play()
                run.assert_not_called()
            with self.assertRaises(RuntimeError):self.backend.import_local_pack(self.root/'pack.zip')
        finally:
            release.set();worker.join(5)
        self.assertFalse(worker.is_alive());self.assertEqual(errors,[])
        self.assertEqual(self.backend.config['game_language'],'es-ES')
        self.assertEqual(self.backend.config['ui_language'],'en')
        self.assertTrue(self.backend.local_allow_partial)
        self.assertTrue(self.backend.local_verified)
        self.assertFalse(self.backend.local_checking)
        self.assertFalse(settings.with_name('language-pack-consent.json').exists())
        self.backend.bridge.assert_not_called()

    def test_readonly_control_locks_keep_settings_open_but_release_stays_exclusive(self):
        from app import launcher_control_locks
        self.assertEqual(launcher_control_locks(True,True,True,False,False,False),
                         dict(settings=False,play=True,import_pack=True))
        self.assertEqual(launcher_control_locks(True,False,False,False,False,False),
                         dict(settings=False,play=True,import_pack=False))
        self.assertEqual(launcher_control_locks(True,False,False,False,False,True),
                         dict(settings=False,play=False,import_pack=False))
        for workspace,readonly,busy,running in ((False,True,False,False),(True,False,False,False),(True,True,True,False),(True,True,False,True)):
            with self.subTest(workspace=workspace,readonly=readonly,busy=busy,running=running):
                locks=launcher_control_locks(workspace,True,readonly,busy,running)
                self.assertTrue(all(locks.values()))

    def test_release_mode_keeps_bridge_and_preserves_custom_language_selection(self):
        release=Backend(self.root/'bridge.exe',self.config)
        self.assertIsNone(release.workspace_root)
        self.assertEqual(release.config['game_language'],'fr-CA')
        release.bridge=Mock(return_value={'core_ready':True,'language_ready':False})
        release.inspect()
        release.bridge.assert_called_once_with('inspect',manifest=None)
        with self.assertRaises(ValueError):release.settings(game_language='fr-CA')
        with patch('backend.sys.frozen',True,create=True):self.assertIsNone(detect_workspace_root())


if __name__=='__main__':unittest.main()
