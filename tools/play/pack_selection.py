"""Language selection shared by the local options UI and headless tests."""
import os
import json
from pathlib import Path
import tempfile


def read_selection(settings):
    values = {}
    try:
        for line in Path(settings).read_text(encoding='utf-8-sig').splitlines():
            key, separator, value = line.partition('=')
            if separator:
                values[key.strip()] = value.strip()
    except FileNotFoundError:
        pass
    locale = values.get('language', 'ko-KR')
    # The runtime rewrites settings.ini (F9), dropping unknown keys. The separate
    # receipt survives that rewrite and grants consent only to this exact pack.
    allowed = values.get('language_allow_partial') != '0' and read_consent(settings, locale)
    return locale, allowed


def manifest_for_selection(settings, locale):
    from pack_registry import safe_path, valid_locale
    if not valid_locale(locale):
        raise ValueError('Invalid language identifier')
    root = Path(settings).resolve().parents[2]
    return safe_path(root, 'localization/installed-packs/' + locale + '/pack.json')


def manifest_digest(settings, locale):
    from pack_registry import digest
    return digest(manifest_for_selection(settings, locale))


def read_consent(settings, locale):
    try:
        path = Path(settings).with_name('language-pack-consent.json')
        if path.stat().st_size > 8192:
            return False
        consent = json.loads(path.read_text(encoding='utf-8'))
        return (isinstance(consent, dict) and consent.get('schema_version') == 1
                and consent.get('locale') == locale
                and consent.get('manifest_sha256') == manifest_digest(settings, locale))
    except (OSError, ValueError, TypeError, IndexError):
        return False


def write_consent(settings, locale, sha256=None):
    path = Path(settings).with_name('language-pack-consent.json')
    if sha256 is None:
        path.unlink(missing_ok=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix='.language-pack-consent-', dir=path.parent)
    try:
        with os.fdopen(handle, 'w', encoding='utf-8', newline='\n') as stream:
            json.dump(dict(schema_version=1, locale=locale, manifest_sha256=sha256), stream)
            stream.write('\n')
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def write_selection(settings, locale, allow_partial):
    settings = Path(settings)
    try:
        lines = settings.read_text(encoding='utf-8-sig').splitlines()
    except FileNotFoundError:
        lines = []
    lines = [line for line in lines if line.partition('=')[0].strip()
             not in ('language', 'language_allow_partial')]
    lines += ['language=' + locale, 'language_allow_partial=' + ('1' if allow_partial else '0')]
    settings.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix='.settings-language-', dir=settings.parent)
    try:
        with os.fdopen(handle, 'w', encoding='utf-8', newline='\n') as stream:
            stream.write('\n'.join(lines) + '\n')
        os.replace(temporary, settings)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class PackSelection:
    def __init__(self, root, registry=None):
        if registry is None:
            import language_packs as registry
        self.root = Path(root)
        self.registry = registry
        self.settings = self.root / 'native/play/settings.ini'
        self.packs = []

    def refresh(self):
        self.packs = self.registry.discover_language_packs(self.root)
        return self.packs

    def import_file(self, source):
        result = self.registry.import_language_pack(self.root, Path(source))
        self.refresh()
        return result

    def save(self, locale, allow_partial=False):
        # Re-read manifests and revalidate proofs at save time, including after imports.
        self.refresh()
        pack = next((pack for pack in self.packs if pack.get('locale') == locale), None)
        if pack is None:
            raise ValueError('설치된 언어팩을 찾을 수 없습니다: ' + locale)
        partial = pack.get('status') == 'partial_test'
        if partial and not allow_partial:
            write_consent(self.settings, locale)
            raise ValueError('부분 테스트 팩은 미번역 부분의 대체 언어 사용에 동의해야 합니다.')
        if not pack.get('ready') and not (partial and pack.get('selectable_preview') and allow_partial):
            raise ValueError(str(pack.get('error') or '언어팩이 실행 준비되지 않았습니다.'))
        consent_digest = manifest_digest(self.settings, locale) if partial else None
        self.registry.resolve_language(self.root, locale, allow_partial=partial and allow_partial)
        if partial and consent_digest != manifest_digest(self.settings, locale):
            raise ValueError('Language pack changed during validation; select it again.')
        # Revoke first: interrupted saves cannot reuse another locale's consent.
        write_consent(self.settings, locale)
        write_selection(self.settings, locale, partial and allow_partial)
        if partial:
            write_consent(self.settings, locale, consent_digest)


def pack_label(pack):
    state = '부분 테스트 · 대체 언어 포함' if pack.get('status') == 'partial_test' else (
        '준비됨' if pack.get('ready') else '사용 불가')
    return f"{pack.get('label') or pack.get('locale') or '잘못된 팩'} [{pack.get('locale', '?')}] — {state}"
