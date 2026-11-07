"""Verified HTTPS using the operating system's certificate-chain validator."""
from functools import lru_cache
import ssl
import urllib.error
import urllib.request

import truststore


@lru_cache(maxsize=1)
def client_context():
    # Use a scoped context: never replace Python's global SSL defaults.
    # On Windows CryptoAPI can build chains using the native root/intermediate
    # stores and retrieve missing intermediates, unlike a static OpenSSL store.
    context = truststore.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.check_hostname = True
    context.verify_mode = ssl.CERT_REQUIRED
    return context


def urlopen(request, *, timeout=25):
    return urllib.request.urlopen(request, timeout=timeout, context=client_context())


def is_certificate_error(error):
    if isinstance(error, urllib.error.URLError):
        error = error.reason
    return isinstance(error, ssl.SSLCertVerificationError)
