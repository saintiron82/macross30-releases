import http.server
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading
import unittest
import urllib.error
from unittest.mock import patch

from backend import Backend
import network_tls


class NetworkTlsTests(unittest.TestCase):
    def test_context_requires_verified_host_and_tls12(self):
        context = network_tls.client_context()
        self.assertTrue(context.check_hostname)
        self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        self.assertGreaterEqual(context.minimum_version, ssl.TLSVersion.TLSv1_2)

    def test_all_backend_downloads_use_scoped_context(self):
        with tempfile.TemporaryDirectory() as td, patch('network_tls.urllib.request.urlopen') as open_url:
            backend = Backend('unused', Path(td) / 'settings.json')
            backend.get('BOOT-INSTALL.json')
            backend.get('Macross30Launcher-0.3.8.exe')
            self.assertEqual(open_url.call_count, 2)
            for call in open_url.call_args_list:
                self.assertIs(call.kwargs['context'], network_tls.client_context())
                self.assertEqual(call.kwargs['timeout'], 25)

    def test_certificate_failure_is_localized_and_never_retried_unverified(self):
        error = urllib.error.URLError(ssl.SSLCertVerificationError('missing issuer'))
        with tempfile.TemporaryDirectory() as td, patch('network_tls.urllib.request.urlopen', side_effect=error) as open_url:
            backend = Backend('unused', Path(td) / 'settings.json')
            for language in ('ko', 'en', 'ja'):
                backend.config['ui_language'] = language
                with self.assertRaises(OSError) as caught:
                    backend.get('BOOT-INSTALL.json')
                self.assertNotIn('urlopen error', str(caught.exception))
                self.assertIn('GitHub', str(caught.exception))
                self.assertIs(caught.exception.__cause__, error)
            self.assertEqual(open_url.call_count, 3)

    def test_other_network_errors_are_preserved(self):
        error = urllib.error.URLError('offline')
        with tempfile.TemporaryDirectory() as td:
            backend = Backend('unused', Path(td) / 'settings.json', opener=lambda *a, **k: (_ for _ in ()).throw(error))
            with self.assertRaises(urllib.error.URLError) as caught:
                backend.get('BOOT-INSTALL.json')
            self.assertIs(caught.exception, error)


class LocalHttpsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        openssl = shutil.which('openssl') or 'C:/Program Files/Git/usr/bin/openssl.exe'
        if not Path(openssl).is_file():
            raise unittest.SkipTest('OpenSSL needed for temporary local certificate fixtures')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cls.ca = folder / 'ca.pem'
        def command(*args):
            subprocess.run([openssl, *args], cwd=folder, check=True, capture_output=True)
        command('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem',
                '-days', '1', '-subj', '/CN=Macross temporary test CA', '-addext', 'basicConstraints=critical,CA:TRUE',
                '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
        command('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'server.key', '-out', 'server.csr',
                '-subj', '/CN=localhost')
        (folder / 'extensions.txt').write_text('subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n')
        command('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial',
                '-out', 'server.pem', '-days', '1', '-extfile', 'extensions.txt')
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'verified local fixture')
            def log_message(self, *args):
                pass
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(folder / 'server.pem', folder / 'server.key')
        cls.server.socket = context.wrap_socket(cls.server.socket, server_side=True)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.addClassCleanup(cls.server.server_close)
        cls.addClassCleanup(cls.server.shutdown)

    def get(self, hostname, trusted):
        context = type(network_tls.client_context())(ssl.PROTOCOL_TLS_CLIENT)
        if trusted:
            context.load_verify_locations(cafile=self.ca)
        with patch('network_tls.client_context', return_value=context):
            return network_tls.urlopen(f'https://{hostname}:{self.server.server_port}/', timeout=5)

    def test_explicitly_trusted_chain_succeeds(self):
        with self.get('localhost', trusted=True) as response:
            self.assertEqual(response.read(), b'verified local fixture')

    def test_untrusted_chain_is_rejected(self):
        with self.assertRaises(urllib.error.URLError) as caught:
            self.get('localhost', trusted=False)
        self.assertTrue(network_tls.is_certificate_error(caught.exception))

    def test_wrong_hostname_is_rejected_even_with_trusted_chain(self):
        with self.assertRaises(urllib.error.URLError) as caught:
            self.get('127.0.0.1', trusted=True)
        self.assertTrue(network_tls.is_certificate_error(caught.exception))


if __name__ == '__main__':
    unittest.main()
