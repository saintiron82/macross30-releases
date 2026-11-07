"""Read-only packaged HTTPS check; no launcher window, install or report upload."""
import json
import re
from urllib.parse import urlsplit
from urllib.request import Request

from network_tls import urlopen


def run():
    probes = []
    try:
        metadata_url = 'https://raw.githubusercontent.com/saintiron82/macross30-releases/main/channels/stable/PYTHON-LAUNCHER.json'
        with urlopen(Request(metadata_url, headers={'User-Agent': 'Macross30Launcher/TLS-check'})) as response:
            metadata = json.loads(response.read(256 * 1024))
            probes.append(dict(host=urlsplit(response.url).hostname, status=response.status, verified=True))
        filename = metadata.get('file', '')
        match = re.fullmatch(r'Macross30Launcher-(\d+\.\d+\.\d+)\.exe', filename)
        if not match:
            raise ValueError('Unexpected launcher asset identity')
        asset_url = f'https://github.com/saintiron82/macross30-releases/releases/download/v{match[1]}/{filename}'
        with urlopen(Request(asset_url, headers={'User-Agent': 'Macross30Launcher/TLS-check', 'Range': 'bytes=0-63'})) as response:
            content = response.read(64)
            if not content.startswith(b'MZ'):
                raise ValueError('Unexpected launcher asset content')
            probes.append(dict(host='github.com', final_host=urlsplit(response.url).hostname,
                               status=response.status, received_bytes=len(content), verified=True))
        return dict(ok=True, probes=probes)
    except Exception as error:
        return dict(ok=False, probes=probes, error_type=type(error).__name__, error=str(error))
