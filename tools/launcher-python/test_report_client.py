import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
import zipfile
from report_client import prepare, send


class ReportTests(unittest.TestCase):
    def fixture(self):
        work = tempfile.TemporaryDirectory(); self.addCleanup(work.cleanup)
        root = Path(work.name)
        identity = 'a'*32
        session = dict(session=identity,game_version='0.3.3',launcher_version='0.3.3',language='ko-KR',exit_code=-1)
        snapshot = root/'play/sessions'/identity; snapshot.mkdir(parents=True)
        (root/'play/last-session.json').write_text(json.dumps(session))
        (snapshot/'diagnostics.log').write_text('session.begin\npassword=must-not-leave\n'+str(root)+'\nC:\\Users\\ExamplePrivateName\\file')
        for slot in ('FIRST','SECOND'):
            folder = root/'play/gamedata/dev_hdd0/home/00000001/savedata'/slot
            folder.mkdir(parents=True); (folder/'DATA.DAT').write_bytes(b'synthetic '+slot.encode())
        return root

    def test_sanitized_pinned_zip_optional_save_and_stable_retries(self):
        root = self.fixture(); report = prepare(root)
        with zipfile.ZipFile(report['archive']) as z:
            raw = z.read('logs/diagnostics.log').decode()
            self.assertNotIn('must-not-leave',raw); self.assertNotIn('ExamplePrivateName',raw)
            self.assertNotIn(str(root),raw); self.assertFalse(any(n.startswith('selected-save/') for n in z.namelist()))
        self.assertEqual(prepare(root),report)
        with_save = prepare(root,'FIRST')
        self.assertNotEqual(with_save['report_id'],report['report_id'])
        with zipfile.ZipFile(with_save['archive']) as z:
            self.assertEqual(z.read('selected-save/DATA.DAT'),b'synthetic FIRST')
            self.assertNotIn(b'SECOND',b''.join(z.read(n) for n in z.namelist()))
        with self.assertRaises(ValueError): prepare(root,'../FIRST')

    def test_cancel_offline_retry_and_verified_receipt(self):
        report = prepare(self.fixture()); cancel = threading.Event(); cancel.set()
        def forbidden(*args,**kwargs): self.fail('Cancelled report must not transmit')
        self.assertEqual(send(report,'https://relay.example',cancel,forbidden)['status'],'prepared')
        def offline(*args,**kwargs): raise OSError('offline')
        with self.assertRaises(RuntimeError): send(report,'https://relay.example',opener=offline)
        self.assertTrue(Path(report['archive']).is_file())
        def received(request,**kwargs):
            self.assertEqual(request.data,Path(report['archive']).read_bytes())
            return io.BytesIO(json.dumps(dict(report_id=report['report_id'],sha256=report['sha256'],status='sent',receipt_id='1234')).encode())
        sent = send(report,'https://relay.example',opener=received)
        self.assertEqual(sent['status'],'sent')
        self.assertEqual(send(sent,'https://relay.example',opener=forbidden)['receipt_id'],'1234')
        with self.assertRaises(ValueError): send(report,'http://untrusted.example')

    def test_false_success_is_rejected(self):
        report = prepare(self.fixture())
        fake = lambda *a,**k:io.BytesIO(json.dumps(dict(report_id=report['report_id'],sha256='0'*64,status='sent',receipt_id='123')).encode())
        with self.assertRaises(RuntimeError): send(report,'https://relay.example',opener=fake)
        self.assertNotEqual(report['status'],'sent')


if __name__ == '__main__': unittest.main()
