import hashlib
import io
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from download import download, output_lock


class DownloadTests(unittest.TestCase):
    def fixture(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        root=Path(temporary.name)
        payload=bytes(range(128))
        row=dict(name='fixture.iso',size=str(len(payload)),sha1=hashlib.sha1(payload).hexdigest(),md5=hashlib.md5(payload).hexdigest())
        offsets=[]
        def opener(request,timeout=None):
            begin,end=map(int,re.fullmatch(r'bytes=(\d+)-(\d+)',request.get_header('Range')).groups())
            offsets.append(begin)
            response=io.BytesIO(payload[begin:end+1]);response.status=206
            response.headers={'Content-Range':f'bytes {begin}-{end}/{len(payload)}'}
            return response
        mocked=patch('download.shutil.disk_usage',return_value=SimpleNamespace(free=10<<30));mocked.start();self.addCleanup(mocked.stop)
        return root,row,payload,opener,offsets

    def test_direct_ranges_and_full_hashes(self):
        root,row,payload,opener,offsets=self.fixture()
        result=download('fixture',row,root,workers=2,chunk_size=32,opener=opener)
        self.assertTrue(result['verified']);self.assertEqual((root/'fixture.iso').read_bytes(),payload)
        self.assertEqual(result['hashes']['sha256'],hashlib.sha256(payload).hexdigest())
        self.assertEqual(sorted(offsets),[0,32,64,96])

    def test_interrupted_download_resumes_completed_ranges(self):
        root,row,payload,opener,offsets=self.fixture()
        def failing(request,timeout=None):
            if request.get_header('Range')!='bytes=0-31':raise IOError('fixture interruption')
            return opener(request,timeout)
        with self.assertRaises(IOError):download('fixture',row,root,workers=1,chunk_size=32,attempts=1,opener=failing)
        offsets.clear()
        result=download('fixture',row,root,workers=1,chunk_size=32,opener=opener)
        self.assertTrue(result['verified']);self.assertNotIn(0,offsets)
        self.assertEqual((root/'fixture.iso').read_bytes(),payload)

    def test_torrents_are_rejected_without_network(self):
        root,row,_,_,_=self.fixture();row['name']='fixture.torrent'
        with patch('download.urllib.request.urlopen') as network:
            with self.assertRaisesRegex(ValueError,'torrent'):download('fixture',row,root,opener=network)
            network.assert_not_called()

    def test_ignored_range_fails_and_preserves_partial(self):
        root,row,_,opener,_=self.fixture()
        def wrong(request,timeout=None):
            response=opener(request,timeout);response.status=200;return response
        with self.assertRaisesRegex(ValueError,'byte range'):
            download('fixture',row,root,workers=1,chunk_size=32,attempts=1,opener=wrong)
        self.assertFalse((root/'fixture.iso').exists());self.assertTrue((root/'fixture.iso.part').exists())

    def test_existing_different_iso_and_concurrent_writer_are_rejected(self):
        root,row,_,opener,offsets=self.fixture();(root/'fixture.iso').write_bytes(b'preserve')
        with self.assertRaisesRegex(ValueError,'preserved'):download('fixture',row,root,opener=opener)
        self.assertEqual((root/'fixture.iso').read_bytes(),b'preserve');self.assertEqual(offsets,[])
        with output_lock(root/'test.lock'):
            with self.assertRaises(OSError):
                with output_lock(root/'test.lock'):self.fail('Concurrent writer was accepted')

    def test_failed_frontend_retries_a_validated_archive_mirror(self):
        root,row,payload,opener,_=self.fixture();calls=[]
        def unreliable(request,timeout=None):
            calls.append(request.full_url)
            if len(calls)==1:raise IOError('fixture frontend failure')
            return opener(request,timeout)
        mirror='https://ia800603.us.archive.org/17/items/fixture/fixture.iso'
        result=download('fixture',row,root,workers=1,chunk_size=128,opener=unreliable,mirrors=[mirror])
        self.assertTrue(result['verified']);self.assertEqual(calls[1],mirror)


if __name__=='__main__':unittest.main()
