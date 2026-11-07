import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from pipeline import identity, plan, prepare, read, verify, write
from publish import publish, verify_public


class PipelineTests(unittest.TestCase):
    def fixture(self, changed=('launcher',)):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        baseline, inputs = root/'baseline', root/'inputs'
        baseline.mkdir(); inputs.mkdir()
        def packages(folder, version):
            game = folder/f'Macross30-program-{version}.zip'
            exe = b'MZ-fixture-game'
            with zipfile.ZipFile(game,'w') as z:
                z.writestr('program/bin/Macross30Recompiled.exe',exe)
                z.writestr('program/program-manifest.json',json.dumps(dict(files=[dict(
                    path='bin/Macross30Recompiled.exe',bytes=len(exe),sha256=hashlib.sha256(exe).hexdigest())])))
            languages = {}
            for locale in ('ko-KR','en-US'):
                path = folder/f'Macross30-language-{locale}-{version}.zip'
                with zipfile.ZipFile(path,'w') as z:z.writestr('payload/translation.txt',locale)
                languages[locale] = identity(path)
            boot = dict(version=version,mode='user-original-eboot',languages=['ko-KR','en-US','ja-JP'],
                        files={'program':identity(game)},language_packs=languages)
            launcher = folder/f'Macross30Launcher-{version}.exe'; launcher.write_bytes(b'MZ-fixture-launcher-'+version.encode())
            archive = folder/f'Macross30-launcher-{version}.zip'
            with zipfile.ZipFile(archive,'w') as z:
                z.write(launcher,'Macross30Launcher.exe'); z.writestr('USAGE.txt','fixture')
            write(folder/'BOOT-INSTALL.json',boot);write(folder/'LANGUAGE-PACKS.json',languages)
            write(folder/'PYTHON-LAUNCHER.json',dict(version=version,**identity(launcher)))
            write(folder/'LAUNCHER.json',dict(version=version,**identity(launcher)))
        packages(baseline,'0.3.4'); packages(inputs,'0.3.6')
        write(baseline/'STATUS.json',dict(latest=dict(game={'version':'0.3.4'},launcher={'version':'0.3.4'},language_packs={
            'ko-KR':{'version':'0.3.4'},'en-US':{'version':'0.3.4'}}),patch_notes=[]))
        notes = root/'notes.txt';notes.write_text('Fixture release notes')
        config = dict(release='0.3.6',changed=list(changed),baseline_metadata=str(baseline),baseline_assets=str(baseline),
                      input_assets=str(inputs),notes=str(notes))
        return root, baseline, inputs, config

    def test_launcher_only_checks_four_components_and_uploads_no_game_packs(self):
        root, baseline, _, config = self.fixture()
        before = {p.name:p.read_bytes() for p in baseline.iterdir()}
        result = prepare(config,root/'stage')
        proof = verify(root/'stage')
        self.assertEqual(proof['versions'],dict(game='0.3.4',korean='0.3.4',english='0.3.4',launcher='0.3.6'))
        self.assertEqual(len(result['package_audit']['files']),3)
        names = {p.name for p in (root/'stage/assets').iterdir()}
        self.assertFalse(any('program-' in n or 'language-' in n for n in names))
        self.assertEqual(before,{p.name:p.read_bytes() for p in baseline.iterdir()})

    def test_full_release_advances_and_audits_four_components(self):
        root, _, _, config = self.fixture(('game','korean','english','launcher'))
        prepare(config,root/'stage')
        proof = verify(root/'stage')
        self.assertEqual(set(proof['versions'].values()),{'0.3.6'})
        self.assertTrue((root/'stage/assets/Macross30-program-0.3.6.zip').exists())

    def test_corrupt_reused_pack_blocks_launcher_only_release(self):
        _, baseline, _, config = self.fixture()
        (baseline/'Macross30-language-en-US-0.3.4.zip').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError,'missing or changed'):plan(config)

    def test_wrong_upload_tag_and_nonadvancing_version_are_rejected(self):
        _, _, _, config = self.fixture()
        config['release']='0.3.7'
        with self.assertRaisesRegex(ValueError,'upload tag'):plan(config)
        config['release']='0.3.4';config['input_assets']=config['baseline_assets']
        with self.assertRaisesRegex(ValueError,'must advance'):plan(config)

    def test_mismatched_launcher_zip_is_rejected(self):
        root, _, inputs, config = self.fixture()
        with zipfile.ZipFile(inputs/'Macross30-launcher-0.3.6.zip','w') as z:
            z.writestr('Macross30Launcher.exe',b'MZ-different');z.writestr('USAGE.txt','fixture')
        with self.assertRaisesRegex(ValueError,'differ'):prepare(config,root/'stage')

    def test_original_executable_disguised_in_pack_is_rejected(self):
        root, _, inputs, config = self.fixture(('english',))
        path = inputs/'Macross30-language-en-US-0.3.6.zip'
        with zipfile.ZipFile(path,'w') as z:z.writestr('payload/translation.txt',b'SCE\0-original')
        boot = read(inputs/'BOOT-INSTALL.json');boot['language_packs']['en-US']=identity(path);write(inputs/'BOOT-INSTALL.json',boot)
        with self.assertRaisesRegex(ValueError,'Disguised'):prepare(config,root/'stage')

    def test_disc_key_material_cannot_enter_release_packages(self):
        root,_,inputs,config=self.fixture(('english',))
        path=inputs/'Macross30-language-en-US-0.3.6.zip'
        with zipfile.ZipFile(path,'w') as z:z.writestr('payload/disc.dkey','00'*16)
        boot=read(inputs/'BOOT-INSTALL.json');boot['language_packs']['en-US']=identity(path);write(inputs/'BOOT-INSTALL.json',boot)
        with self.assertRaisesRegex(ValueError,'PS3 executable'):prepare(config,root/'stage')

    def test_existing_output_and_changed_stage_are_rejected(self):
        root, _, _, config = self.fixture()
        prepare(config,root/'stage')
        with self.assertRaisesRegex(ValueError,'fresh directory'):prepare(config,root/'stage')
        (root/'stage/assets/STATUS.json').write_text('{}')
        with self.assertRaisesRegex(ValueError,'changed'):verify(root/'stage')

    def test_extra_unplanned_upload_is_rejected(self):
        root, _, _, config = self.fixture()
        prepare(config,root/'stage')
        (root/'stage/assets/private.iso').write_bytes(b'not-to-upload')
        with self.assertRaisesRegex(ValueError,'inventory'):verify(root/'stage')

    def test_published_tag_is_rejected_before_any_mutation(self):
        root, _, _, config = self.fixture()
        prepare(config,root/'stage')
        checkout = root/'checkout';checkout.mkdir()
        seen = []
        def fake(argv,cwd=None):
            seen.append(argv)
            if argv[1:3]==['remote','get-url']:return 'https://github.com/saintiron82/macross30-releases.git'
            if argv[1:3]==['status','--porcelain']:return ''
            if argv[1]=='ls-remote':return 'existing-tag'
            self.fail('Unexpected mutation '+str(argv))
        with patch('publish.command',fake):
            with self.assertRaisesRegex(ValueError,'immutable'):publish(root/'stage',checkout)
        self.assertEqual(len(seen),3)

    def publication_fixture(self, bad_digest=False):
        root, baseline, _, config = self.fixture()
        prepare(config,root/'stage')
        checkout = root/'checkout';checkout.mkdir()
        staged = read(root/'stage/release-plan.json')
        (checkout/'channels/stable').mkdir(parents=True)
        assets = [dict(name=r['file'],size=r['bytes'],digest='sha256:'+r['sha256']) for r in staged['artifacts']]
        if bad_digest:assets[0]['digest']='sha256:'+'0'*64
        calls=[]
        def fake(argv,cwd=None):
            calls.append(argv)
            if argv[0]=='git':
                if argv[1:3]==['remote','get-url']:return 'https://github.com/saintiron82/macross30-releases.git'
                if argv[1]=='show':return (baseline/argv[2].split('/')[-1]).read_text()
                if argv[1]=='rev-parse':return 'a'*40 if argv[2]=='HEAD' else 'b'*40
                return ''
            if argv[:2]==['gh','api']:return json.dumps([dict(tag_name='v0.3.6',id=123,assets=assets)])
            return 'https://github.com/saintiron82/macross30-releases/releases/tag/v0.3.6'
        return root,checkout,calls,fake

    def test_publish_verifies_draft_before_stable_update_and_preserves_receipt(self):
        root,checkout,calls,fake=self.publication_fixture()
        with patch('publish.command',fake),patch('publish.verify_public',return_value={'verified':True}):
            result=publish(root/'stage',checkout)
        self.assertEqual(result['stage'],'verified')
        self.assertEqual(result['versions']['game'],'0.3.4')
        self.assertLess(next(i for i,a in enumerate(calls) if a[:3]==['gh','release','edit']),
                        next(i for i,a in enumerate(calls) if a==['git','push','origin','HEAD:main']))
        with patch('publish.command',fake):
            with self.assertRaisesRegex(ValueError,'existing receipt'):publish(root/'stage',checkout)

    def test_readme_updates_when_baseline_metadata_is_in_checkout(self):
        root,checkout,_,fake=self.publication_fixture()
        record=read(root/'stage/release-plan.json')
        baseline=Path(record['baseline_metadata'])
        destination=checkout/'channels/stable'
        for name in record['baseline_hashes']:
            (destination/name).write_bytes((baseline/name).read_bytes())
        record['baseline_metadata']=str(destination)
        write(root/'stage/release-plan.json',record)
        (checkout/'README.md').write_text('런처 0.3.4 · Launcher **0.3.4**\n'
            '/download/v0.3.4/Macross30-launcher-0.3.4.zip',encoding='utf-8')
        with patch('publish.command',fake),patch('publish.verify_public',return_value={'verified':True}):
            publish(root/'stage',checkout)
        text=(checkout/'README.md').read_text(encoding='utf-8')
        self.assertIn('런처 0.3.6',text)
        self.assertIn('Launcher **0.3.6**',text)
        self.assertIn('/download/v0.3.6/Macross30-launcher-0.3.6.zip',text)

    def test_bad_server_digest_keeps_draft_unpublished(self):
        root,checkout,calls,fake=self.publication_fixture(bad_digest=True)
        with patch('publish.command',fake):
            with self.assertRaisesRegex(ValueError,'server hash mismatch'):publish(root/'stage',checkout)
        self.assertFalse(any(a[:3]==['gh','release','edit'] for a in calls))
        self.assertFalse(any(a==['git','push','origin','HEAD:main'] for a in calls))
        self.assertEqual(read(root/'stage/publication.json')['stage'],'draft_uploaded')

    def test_release_document_replacements_require_matching_baseline(self):
        root,checkout,calls,fake=self.publication_fixture()
        record=read(root/'stage/release-plan.json')
        record['config']['readme_replacements']={'README.md':[dict(old='old release summary',new='new release summary')]}
        write(root/'stage/release-plan.json',record)
        path=checkout/'README.md';path.write_text('old release summary\n',encoding='utf-8')
        with patch('publish.command',fake),patch('publish.verify_public',return_value={'verified':True}):
            publish(root/'stage',checkout)
        self.assertEqual(path.read_text(encoding='utf-8'),'new release summary\n')
        root,checkout,calls,fake=self.publication_fixture()
        record=read(root/'stage/release-plan.json')
        record['config']['readme_replacements']={'README.md':[dict(old='old release summary',new='new release summary')]}
        write(root/'stage/release-plan.json',record)
        (checkout/'README.md').write_text('changed remotely\n',encoding='utf-8')
        with patch('publish.command',fake):
            with self.assertRaisesRegex(ValueError,'documentation baseline changed'):publish(root/'stage',checkout)
        self.assertFalse(any(a[:2]==['git','push'] for a in calls))

    def test_release_source_files_are_hash_guarded_and_added_to_tag_commit(self):
        root,checkout,calls,fake=self.publication_fixture()
        source=root/'loader.py';source.write_bytes(b'# exact frozen source\n')
        record=read(root/'stage/release-plan.json')
        record['config']['source_files']=[dict(source=str(source),target='tools/play/loader.py',sha256=identity(source)['sha256'])]
        write(root/'stage/release-plan.json',record)
        with patch('publish.command',fake),patch('publish.verify_public',return_value={'verified':True}):
            publish(root/'stage',checkout)
        self.assertEqual((checkout/'tools/play/loader.py').read_bytes(),source.read_bytes())
        self.assertIn(['git','add','--','tools/play/loader.py'],calls)
        root,checkout,calls,fake=self.publication_fixture()
        source=root/'loader.py';source.write_bytes(b'changed')
        record=read(root/'stage/release-plan.json')
        record['config']['source_files']=[dict(source=str(source),target='tools/play/loader.py',sha256='0'*64)]
        write(root/'stage/release-plan.json',record)
        with patch('publish.command',fake):
            with self.assertRaisesRegex(ValueError,'Release source changed'):publish(root/'stage',checkout)
        self.assertFalse(any(a[:2]==['git','switch'] for a in calls))

    def test_component_specific_asset_folders_and_baseline_aliases(self):
        root,baseline,inputs,config=self.fixture()
        config['baseline_assets']={name:str(baseline) for name in ('game','korean','english','launcher')}
        config['input_assets']={'launcher':str(inputs)}
        prepare(config,root/'stage')
        write(baseline/'LAUNCHER.json',{})
        with self.assertRaisesRegex(ValueError,'aliases disagree'):plan(config)

    def test_status_mismatch_is_rejected_even_if_inventory_was_updated(self):
        root,_,_,config=self.fixture();prepare(config,root/'stage')
        status=read(root/'stage/assets/STATUS.json');status['latest']['launcher']['version']='9.9.9'
        write(root/'stage/assets/STATUS.json',status)
        record=read(root/'stage/release-plan.json')
        record['artifacts']=[identity(root/'stage/assets'/r['file']) for r in record['artifacts']]
        write(root/'stage/release-plan.json',record)
        with self.assertRaisesRegex(ValueError,'Status versions disagree'):verify(root/'stage')

    def test_public_verification_uses_anonymous_payload_not_only_server_digest(self):
        root,_,_,config=self.fixture();prepare(config,root/'stage')
        record=read(root/'stage/release-plan.json')
        release=dict(draft=False,tag_name='v0.3.6',html_url='https://example.invalid/release',assets=[
            dict(name=r['file'],size=r['bytes'],digest='sha256:'+r['sha256']) for r in record['artifacts']])
        with patch('publish.command',return_value=json.dumps(release)),patch('publish.urllib.request.urlopen',return_value=io.BytesIO(b'corrupt')):
            with self.assertRaisesRegex(ValueError,'download hash mismatch'):verify_public(root/'stage')


if __name__=='__main__':unittest.main()
