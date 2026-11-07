"""Select the reviewed Korean ELF and install a pending UI pack before launching."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def prepare_localization(root):
    pointer = root/'localization/image_assets/active-ui.json'
    if not pointer.exists():
        legacy = root/'localization/image_assets/ui-ko-v1'
        if (legacy/'installation.json').exists():
            return legacy/'EBOOT.ui-ko.elf'
        return root/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
    active = json.loads(pointer.read_text(encoding='utf-8'))
    name = active['bundle']
    if not isinstance(name, str) or not re.fullmatch(r'[a-z0-9-]+', name):
        raise ValueError('Invalid localization bundle name')
    bundle = root/'localization/image_assets'/name
    ready = json.loads((bundle/'ready.json').read_text(encoding='utf-8'))
    if not isinstance(ready.get('version'), str) or not re.fullmatch(r'[a-f0-9]{64}', ready['version']):
        raise ValueError('Invalid localization bundle version')
    elf = (bundle/ready['elf']['path']).resolve()
    if bundle.resolve() not in elf.parents:
        raise ValueError('Localized ELF outside bundle')
    if hashlib.sha256(elf.read_bytes()).hexdigest() != ready['elf']['sha256']:
        raise ValueError('Localized ELF checksum changed')
    installed_path = bundle/'installation.json'
    installed = json.loads(installed_path.read_text(encoding='utf-8')) if installed_path.exists() else {}
    if installed.get('bundle_version') != ready['version']:
        # Installer refuses an active game and acquires the shared runner lease.
        subprocess.run([sys.executable, str(root/'tools/localization/install_korean_ui.py'),
                        '--bundle', name, '--apply'], check=True, cwd=root)
    override=active.get('elf_override')
    if override is not None:
        if not isinstance(override,dict) or override.get('base_elf_sha256')!=ready['elf']['sha256']:
            raise ValueError('Localized ELF override base changed')
        selected=(bundle/override['path']).resolve()
        if bundle.resolve() not in selected.parents or hashlib.sha256(selected.read_bytes()).hexdigest()!=override['sha256']:
            raise ValueError('Localized ELF override checksum/path changed')
        return selected
    return elf
