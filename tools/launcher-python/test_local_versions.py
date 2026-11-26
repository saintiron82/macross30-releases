import hashlib
import tempfile
import unittest
from pathlib import Path

import local_versions as versions


class LocalLanguageSwitchTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root=Path(temp.name)
        self.profile=self.root/'release-profile.json'
        self.state=self.root/'.local-build/language-views.json'
        versions.write(self.profile,{'language_views':{}})
        versions.write(self.state,{'languages':{}})
        self.first=self.install(b'first')

    def install(self,data):
        paths=versions.active_paths(self.root,'ko-KR')
        expected=hashlib.sha256(data).hexdigest()
        file=paths['view']/'data/disc/item'
        file.parent.mkdir(parents=True,exist_ok=True)
        file.write_bytes(data)
        (file.parents[1]/'installed-version.txt').write_text(expected[:16])
        paths['game.elf'].parent.mkdir(exist_ok=True)
        paths['game.elf'].write_bytes(data)
        row=dict(root='data/languages/ko-KR',elf='bin/EBOOT.ko-KR.elf',data_id=expected[:16],
                 plan_sha256=expected,elf_sha256=expected,files=[dict(path='data/disc/item',
                 size=len(data),mtime_ns=file.stat().st_mtime_ns)])
        versions.write(self.profile,{'language_views':{'ko-KR':row}})
        versions.write(self.state,{'languages':{'ko-KR':row}})
        return row

    def test_failed_install_recovers_previous_files_and_both_indexes(self):
        expected=hashlib.sha256(b'next').hexdigest()
        def fail():
            self.install(b'next')
            raise OSError('interrupted installer')
        with self.assertRaisesRegex(OSError,'interrupted installer'):
            versions.switch_language(self.root,'ko-KR',dict(plan_sha256=expected,elf_sha256=expected),fail)
        self.assertEqual(versions.read(self.profile)['language_views']['ko-KR'],self.first)
        self.assertEqual(versions.read(self.state)['languages']['ko-KR'],self.first)
        self.assertEqual((self.root/self.first['elf']).read_bytes(),b'first')
        self.assertEqual((self.root/self.first['root']/'data/disc/item').read_bytes(),b'first')

    def test_completed_versions_switch_both_ways_without_installer(self):
        expected=hashlib.sha256(b'next').hexdigest()
        versions.switch_language(self.root,'ko-KR',dict(plan_sha256=expected,elf_sha256=expected),lambda:self.install(b'next'))
        second=versions.read(self.profile)['language_views']['ko-KR']
        for row in (self.first,second,self.first):
            self.assertTrue(versions.retained(self.root,'ko-KR',row))
            versions.switch_language(self.root,'ko-KR',row,lambda:self.fail('unexpected reinstall'))
            self.assertEqual(versions.read(self.profile)['language_views']['ko-KR'],row)
            self.assertEqual(versions.digest(self.root/row['elf']),row['elf_sha256'])
