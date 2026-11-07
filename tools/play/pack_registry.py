"""Data-only local language packs. Paths in pack.json never select host programs."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import uuid
import zipfile

BUILTIN_LOCALES = ('ja-JP', 'ko-KR', 'en-US', 'ru-RU')
RESOURCE_KEYS = frozenset(('elf', 'vfs', 'subtitles', 'battle_catalog', 'native_ui', 'elf_overlay', 'gop_overlay'))
COMPLETE_TEXT_OVERLAY_KEYS = frozenset(('gop_overlay','elf_overlay','native_ui','subtitles','battle_catalog'))
FILE_KEYS = RESOURCE_KEYS - {'vfs', 'subtitles', 'gop_overlay'}
FORBIDDEN = {'.exe', '.dll', '.com', '.bat', '.cmd', '.ps1', '.psm1', '.py', '.pyc', '.sh', '.vbs', '.js', '.cjs', '.mjs', '.msi', '.scr', '.lnk', '.url', '.hta'}
MAX_FILES = 100000
MAX_BYTES = 32 * 1024**3


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def valid_locale(locale):
    return isinstance(locale, str) and re.fullmatch(r'[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*', locale) is not None


def _parts(value):
    if not isinstance(value, str) or not value or any(c in value for c in '\\:<>"|?*') or any(ord(c) < 32 for c in value):
        raise ValueError('Unsafe pack path')
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ('', '.', '..') for part in value.split('/')):
        raise ValueError('Unsafe pack path: ' + value)
    for part in path.parts:
        if part.endswith((' ', '.')) or re.match(r'(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)', part):
            raise ValueError('Unsafe Windows filename: ' + value)
    return path.parts


def _linked(path):
    return path.is_symlink() or getattr(path, 'is_junction', lambda: False)()


def safe_path(base, value):
    base = Path(base).absolute()
    path = base.joinpath(*_parts(value))
    # Check lexical ancestors, not just the resolved target (junctions can stay inside).
    for current in (path, *path.parents):
        if _linked(current):
            raise ValueError('Links are not allowed in pack paths: ' + str(current))
        if current == base:
            break
    if not path.resolve().is_relative_to(base.resolve()):
        raise ValueError('Pack path escapes its root')
    return path


def registry_root(root):
    return safe_path(Path(root), 'localization/installed-packs')


def create_inherited_stage(parent,prefix):
    """Create an unpredictable stage while inheriting the installation's Windows ACL.

    Python 3.12's tempfile.mkdtemp requests mode 0700, which installs a protected
    OWNER_RIGHTS DACL on Windows. A stage created by a build identity then becomes
    unreadable to the actual player after rename. 0777 leaves ACL policy to the
    already-authorized installation parent; it does not grant a new Windows ACL.
    """
    parent=Path(parent)
    for _ in range(10):
        stage=parent/(prefix+uuid.uuid4().hex)
        try:
            os.mkdir(stage,mode=0o777)
            return stage
        except FileExistsError:
            continue
    raise FileExistsError('Could not allocate a unique language-pack stage')


def _read(path):
    if path.stat().st_size > 4 * 1024**2:
        raise ValueError('Pack manifest is too large')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate manifest key: ' + key)
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=unique)


def _inventory(folder):
    result = {}
    total = 0
    for current, directories, files in os.walk(folder, followlinks=False):
        for name in directories + files:
            path = Path(current) / name
            if _linked(path) or not (path.is_file() or path.is_dir()):
                raise ValueError('Pack contains a link or special file')
            relative = path.relative_to(folder).as_posix()
            _parts(relative)
            folded = relative.casefold()
            if folded in result:
                raise ValueError('Case-colliding pack paths')
            result[folded] = path
            if path.is_file():
                total += path.stat().st_size
                if path.suffix.lower() in FORBIDDEN:
                    raise ValueError('Host executable/script files are not allowed: ' + relative)
                with path.open('rb') as stream:
                    if stream.read(2) == b'MZ':
                        raise ValueError('Host executable payload is not allowed')
        if len(result) > MAX_FILES or total > MAX_BYTES:
            raise ValueError('Pack exceeds import limits')
    return [p for p in result.values() if p.is_file()]


def validate_pack(root, folder, *, verify=True):
    """Validate a standalone staged/installed pack, without resolving inheritance."""
    root, folder = Path(root).absolute(), Path(folder).absolute()
    manifest = safe_path(folder, 'pack.json')
    doc = _read(manifest)
    allowed = {'schema_version', 'locale', 'label', 'status', 'image_locale', 'base_locale', 'coverage', 'resources', 'proofs', 'source_guards', 'description'}
    if not isinstance(doc, dict) or set(doc) - allowed:
        raise ValueError('Unknown manifest fields (host launch instructions are forbidden)')
    if doc.get('schema_version') != 1 or not valid_locale(doc.get('locale')):
        raise ValueError('Unsupported manifest schema or invalid locale')
    if doc['locale'].casefold() in {p.casefold() for p in BUILTIN_LOCALES if p != 'ru-RU'} or (
            doc['locale'].casefold() == 'ru-ru' and (root/'localization/ru-RU/pack-ready.json').exists()):
        raise ValueError('Built-in language packs cannot be replaced by local imports')
    if not isinstance(doc.get('label'), str) or not doc['label'].strip() or len(doc['label']) > 100 or any(ord(c) < 32 for c in doc['label']):
        raise ValueError('Invalid language label')
    if doc.get('status') not in ('complete', 'partial_test'):
        raise ValueError('Pack status must explicitly be complete or partial_test')
    if not valid_locale(doc.get('image_locale')):
        raise ValueError('An explicit image_locale is required')
    base = doc.get('base_locale')
    if base is not None and (not valid_locale(base) or base.casefold() == doc['locale'].casefold()):
        raise ValueError('Invalid base locale')
    coverage = doc.get('coverage', {})
    if set(coverage) != {'total', 'translated', 'fallback'} or any(type(n) is not int or n < 0 for n in coverage.values()):
        raise ValueError('Coverage requires nonnegative total, translated and fallback counts')
    if coverage['translated'] + coverage['fallback'] != coverage['total']:
        raise ValueError('Coverage counts do not add up')
    if doc['status'] == 'complete' and (coverage['fallback'] or not coverage['total']):
        raise ValueError('Complete packs require nonzero coverage with no fallback')
    resources = doc.get('resources', {})
    if not isinstance(resources, dict) or not resources or set(resources) - RESOURCE_KEYS:
        raise ValueError('Invalid resources; no executable or command fields are permitted')
    resolved = {key: safe_path(folder, value) for key, value in resources.items()}
    for key, path in resolved.items():
        if not (path.is_file() if key in FILE_KEYS else path.is_dir()):
            raise ValueError('Missing pack resource: ' + key)
    if not verify:
        # Manifest discovery is provisional. Never walk payload directories, open
        # guest binaries, or hash proofs/guards merely to populate a UI list.
        if 'vfs' in resolved and not (resolved['vfs']/'PS3_GAME/USRDIR/DATA/PACK/PACK.IDX').is_file():
            raise ValueError('Pack VFS is missing PACK.IDX')
        if 'subtitles' in resolved and not (resolved['subtitles']/'tracks.tsv').is_file():
            raise ValueError('Subtitle resource is missing tracks.tsv')
        return doc, resolved
    if 'elf' in resolved:
        with resolved['elf'].open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                raise ValueError('ELF resource must be a guest ELF, not a host program')
    if 'elf' in resolved and 'vfs' in resolved and resolved['elf'].parent != resolved['vfs']:
        raise ValueError('Pack ELF must belong to its frozen VFS')
    if 'vfs' in resolved and not (resolved['vfs'] / 'PS3_GAME/USRDIR/DATA/PACK/PACK.IDX').is_file():
        raise ValueError('Pack VFS is missing PACK.IDX')
    if 'subtitles' in resolved and not (resolved['subtitles'] / 'tracks.tsv').is_file():
        raise ValueError('Subtitle resource is missing tracks.tsv')
    if 'gop_overlay' in resolved:
        if base != 'en-US':raise ValueError('GOP overlays currently require base_locale en-US')
        overlay_files=_inventory(resolved['gop_overlay'])
        if not overlay_files or any(p.parent!=resolved['gop_overlay'] or (p.suffix.lower()!='.gop' and p.name not in ('font_j24x24.abf','font_j24x24_0.dds')) for p in overlay_files):
            raise ValueError('GOP overlay accepts only direct *.gop files, font_j24x24.abf and font_j24x24_0.dds')
        if (resolved['gop_overlay']/'font_j24x24_0.dds').exists() and not (resolved['gop_overlay']/'font_j24x24.abf').is_file():
            raise ValueError('A font atlas requires its matching ABF font')
    if not base and not {'elf', 'vfs', 'subtitles'} <= set(resolved):
        raise ValueError('Standalone packs require ELF, VFS and subtitles')
    if doc['status']=='complete' and not ({'elf','vfs','subtitles'} <= set(resolved) or (base=='en-US' and COMPLETE_TEXT_OVERLAY_KEYS <= set(resolved))):
        raise ValueError('Inherited resource fallback requires partial_test status')
    proofs = doc.get('proofs')
    if not isinstance(proofs, list) or not proofs:
        raise ValueError('Payload SHA-256 proofs are required')
    proved = set()
    for proof in proofs:
        if not isinstance(proof, dict) or set(proof) != {'path', 'sha256'} or not re.fullmatch('[a-f0-9]{64}', proof.get('sha256', '')):
            raise ValueError('Invalid payload proof')
        path = safe_path(folder, proof['path'])
        if path == manifest or path in proved or not path.is_file() or digest(path) != proof['sha256']:
            raise ValueError('Payload proof mismatch: ' + proof['path'])
        proved.add(path)
    payload = set(_inventory(folder)) - {manifest}
    if proved != payload:
        raise ValueError('Every payload file must have exactly one SHA-256 proof')
    guards = doc.get('source_guards')
    if not isinstance(guards, list) or not guards:
        raise ValueError('Installation source guards are required')
    for guard in guards:
        if not isinstance(guard, dict) or set(guard) != {'path', 'sha256'} or not re.fullmatch('[a-f0-9]{64}', guard.get('sha256', '')):
            raise ValueError('Invalid source guard')
        path = safe_path(root, guard['path'])
        if path.is_relative_to(registry_root(root)) or not path.is_file() or digest(path) != guard['sha256']:
            raise ValueError('Installation source guard mismatch: ' + guard['path'])
    return doc, resolved


def discover_packs(root, *, verify=True):
    """List invalid packs as errors too; discovery never turns fallback into completion."""
    try:
        base = registry_root(root)
        if not base.exists():return []
        folders=sorted(base.iterdir(),key=lambda p:p.name.casefold())
    except OSError as error:
        return [dict(locale='',label='Language pack registry',status='invalid',ready=False,error=str(error),verification='failed' if verify else 'pending')]
    rows = []
    for folder in folders:
        if folder.name.startswith('.'):
            continue
        row = dict(locale=folder.name, label=folder.name, status='invalid', ready=False, manifest=str(folder/'pack.json'))
        try:
            safe_path(base, folder.name)
            doc, resources = validate_pack(root, folder, verify=verify)
            if doc['locale'] != folder.name:
                raise ValueError('Installed folder must match manifest locale')
            row.update({key: doc[key] for key in ('locale', 'label', 'status', 'image_locale', 'coverage')})
            row.update(base_locale=doc.get('base_locale'), ready=doc['status'] == 'complete', error=None,
                       verification='verified' if verify else 'pending')
        except (ValueError, OSError, KeyError, TypeError) as error:
            row['error'] = str(error)
            row['verification']='failed' if verify else 'pending'
        rows.append(row)
    return rows


def resolve_pack(root, locale, allow_partial=False, resolve_base=None, _seen=(), materialize=True):
    if not valid_locale(locale) or locale.casefold() in {s.casefold() for s in _seen}:
        raise ValueError('Invalid or circular language dependency')
    folder = safe_path(registry_root(root), locale)
    doc, resources = validate_pack(root, folder)
    if doc['locale'] != locale:
        raise ValueError('Pack locale mismatch')
    if doc['status'] == 'partial_test' and not allow_partial:
        raise ValueError('Partial test pack requires explicit preview opt-in')
    profile, inherited, base_profile = {}, [], None
    if doc.get('base_locale'):
        if resolve_base is None:
            raise ValueError('Base locale resolver is required')
        base = resolve_base(doc['base_locale'], (*_seen, locale))
        if base.get('status')=='partial_test' and doc['status']!='partial_test':
            raise ValueError('Partial base dependency requires partial_test status')
        base_profile=base
        profile.update({key: base[key] for key in RESOURCE_KEYS if key in base})
        inherited = sorted(set(profile) - set(resources))
        if doc['status']=='complete' and COMPLETE_TEXT_OVERLAY_KEYS <= set(resources):
            # The immutable ELF/VFS provide images and base bytes; all scoped
            # text is supplied by the proved overlay, not a text fallback.
            inherited=[key for key in inherited if key not in ('elf','vfs')]
        if inherited and doc['status'] != 'partial_test':
            raise ValueError('Inherited resource fallback requires partial_test status')
    profile.update(resources)
    if not {'elf', 'vfs', 'subtitles'} <= set(profile):
        raise ValueError('Resolved pack is missing game resources')
    if doc['image_locale'] != locale and doc['image_locale'] != doc.get('base_locale'):
        if resolve_base is None:
            raise ValueError('Image dependency resolver is required')
        dependency=resolve_base(doc['image_locale'], (*_seen, locale))
        if dependency.get('status')=='partial_test' and doc['status']!='partial_test':
            raise ValueError('Partial image dependency requires partial_test status')
    manifest_digest=digest(folder/'pack.json')
    state = safe_path(Path(root), 'native/state-packs/' + locale + '/' + manifest_digest[:16])
    if 'gop_overlay' in resources and not materialize:
        # Listing never creates or hashes a multi-GB VFS. It must still expose an
        # old sandbox-owner-only cache as unavailable instead of offering it.
        cache=safe_path(Path(root),'native/installed-language-cache/'+locale+'/'+manifest_digest)
        if cache.exists():
            with safe_path(cache,'cache.json').open('rb') as stream:stream.read(1)
    if 'gop_overlay' in resources and materialize:
        from pack_vfs import materialize_pack_vfs
        vfs,state=materialize_pack_vfs(root,doc,folder,base_profile,resources['gop_overlay'])
        profile['vfs']=Path(vfs)
    profile.update(locale=locale, label=doc['label'], image_locale=doc['image_locale'], state=state,
                   battle_subtitles='battle_catalog' in profile, status=doc['status'],
                   manifest=folder/'pack.json', coverage=doc['coverage'],
                   fallback_report={'base_locale': doc.get('base_locale'), 'resources': inherited, 'text_entries': doc['coverage']['fallback']})
    return profile


def import_pack(root, source):
    """Validate then atomically install a folder or root-manifest ZIP. Never overwrite."""
    root, source = Path(root).absolute(), Path(source).absolute()
    if any(_linked(path) for path in (source,*source.parents)):
        raise ValueError('Import source cannot contain links')
    if source.is_file() and source.suffix.lower()=='.json':
        if source.name!='pack.json':raise ValueError('Select the pack.json manifest')
        source=source.parent
    base = registry_root(root)
    base.mkdir(parents=True, exist_ok=True)
    stage = create_inherited_stage(base,'.import-')
    try:
        if source.is_dir():
            if base.resolve().is_relative_to(source.resolve()):
                raise ValueError('Import source cannot contain the pack registry')
            for path in _inventory(source):
                target = safe_path(stage, path.relative_to(source).as_posix())
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        else:
            with zipfile.ZipFile(source) as archive:
                seen, total = set(), 0
                if len(archive.infolist()) > MAX_FILES:
                    raise ValueError('Too many ZIP members')
                for item in archive.infolist():
                    name = item.filename.rstrip('/')
                    target = safe_path(stage, name)
                    folded = name.casefold()
                    if folded in seen:
                        raise ValueError('Duplicate or case-colliding ZIP members')
                    seen.add(folded)
                    mode = item.external_attr >> 16
                    if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                        raise ValueError('ZIP links and special files are forbidden')
                    total += item.file_size
                    if item.flag_bits & 1 or total > MAX_BYTES:
                        raise ValueError('Encrypted or oversized ZIP is unsupported')
                    if item.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with archive.open(item) as incoming, target.open('xb') as outgoing:
                            shutil.copyfileobj(incoming, outgoing)
        doc, resources = validate_pack(root, stage)
        destination = safe_path(base, doc['locale'])
        if any(p.name.casefold() == doc['locale'].casefold() for p in base.iterdir() if p != stage):
            raise ValueError('Locale is already installed; local import never overwrites packs')
        stage.rename(destination)
        return next(row for row in discover_packs(root) if row['locale'] == doc['locale'])
    finally:
        # stage is a freshly-created private directory; never delete the source/destination.
        if stage.exists():
            shutil.rmtree(stage)
