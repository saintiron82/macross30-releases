"""Resolve restart-bound locale resources without mixing language packs."""
import json
import hashlib
from pathlib import Path
LOCALES=('ja-JP','ko-KR','en-US','ru-RU')


def discover_language_packs(root, *, verify=True):
    """Describe available and unfinished languages without building any resources."""
    from pack_registry import discover_packs
    root=Path(root).resolve()
    custom=discover_packs(root,verify=verify)
    if (root/'localization/ru-RU/pack-ready.json').exists():
        custom=[row for row in custom if row['locale']!='ru-RU']
    custom_ids={row['locale'] for row in custom}
    labels={'ja-JP':'日本語','ko-KR':'한국어','en-US':'English','ru-RU':'Русский'}
    rows=[]
    for locale in LOCALES:
        if locale in custom_ids and locale=='ru-RU' and not (root/'localization/ru-RU/pack-ready.json').exists():continue
        row=dict(locale=locale,label=labels[locale],status='builtin',ready=False,error=None,selectable_preview=False,
                 verification='verified' if verify else 'pending')
        try:
            if locale=='ko-KR':
                # Listing languages must not run the Korean build/preparation step.
                row['ready']=(root/'build/game-ko-full').is_dir()
                if not row['ready']:row['error']='Korean resources need preparation'
            elif not verify:
                if locale=='ja-JP':
                    row['ready']=(root/'native/vfs/source_manifest.json').is_file()
                    if not row['ready']:row['error']='Japanese disc baseline is not prepared'
                else:
                    marker=root/f'localization/{locale}/pack-ready.json'
                    if not marker.is_file():raise ValueError(locale+' pack is not prepared')
                    doc=json.loads(marker.read_text(encoding='utf-8-sig'))
                    if not isinstance(doc,dict) or doc.get('locale')!=locale or doc.get('status')!='full_draft' or doc.get('untranslated')!=0:
                        raise ValueError(locale+' pack is incomplete')
                    for field in ('elf','vfs','subtitles','battle_catalog'):
                        value=doc.get(field)
                        if not isinstance(value,str):raise ValueError(locale+' pack is missing '+field)
                        path=root/value
                        if not (path.is_dir() if field in ('vfs','subtitles') else path.is_file()):
                            raise ValueError(locale+' pack resource is missing: '+field)
                    row['ready']=True
            else:
                resolve_language(root,locale)
                row['ready']=True
        except (ValueError,OSError,KeyError,TypeError) as error:
            row.update(status='unavailable',error=str(error),verification='failed' if verify else 'pending')
        rows.append(row)
    for row in custom:
        if verify and not row.get('error'):
            try:resolve_language(root,row['locale'],allow_partial=True,_discovery=True)
            except (ValueError,OSError,KeyError,TypeError) as error:row.update(ready=False,error=str(error))
        row['selectable_preview']=row['status']=='partial_test' and not row.get('error')
    return rows+custom


def available_languages(root):
    """Locale identifiers accepted by the CLI, including explicit preview choices."""
    return tuple(row['locale'] for row in discover_language_packs(root,verify=False) if row['locale'])


def import_language_pack(root,source):
    from pack_registry import import_pack
    return import_pack(root,source)


def saved_partial_opt_in(root):
    from pack_selection import read_selection
    return read_selection(Path(root)/'native/play/settings.ini')[1]


def apply_catalog_environment(env,profile):
    """Bind only the selected profile's catalogs; never inherit the last locale's."""
    for key,field in (('MACROSS_UI_CATALOG_FILE','native_ui'),('MACROSS_ELF_TEXT_PACK_FILE','elf_overlay')):
        env.pop(key,None)
        if profile and profile.get(field):env[key]=str(profile[field])


def preview_notice(profile):
    if not profile or profile.get('status')!='partial_test':return None
    fallback=profile.get('fallback_report',{})
    resources=', '.join(fallback.get('resources',[])) or 'none'
    return (f"PARTIAL TEST LANGUAGE PACK: {profile.get('locale')} — "
            f"{fallback.get('text_entries',0)} untranslated text entries use fallback "
            f"({fallback.get('base_locale') or 'declared pack fallback'}); "
            f"inherited resources: {resources}. This is not a complete translation.")

def file_digest(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(4*1024*1024),b''):h.update(block)
    return h.hexdigest()

def saved_language(root):
    path=root/'native/play/settings.ini'
    values={}
    if path.exists():
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            key,sep,value=line.partition('=')
            if sep:values[key.strip()]=value.strip()
    language=values.get('language','ko-KR')
    if language not in available_languages(root):raise ValueError('Unsupported saved language: '+language)
    return language

def resolve_language(root,language,allow_partial=False,_seen=(),_discovery=False):
    root=Path(root).resolve()
    from pack_registry import registry_root,valid_locale,resolve_pack
    if not valid_locale(language):raise ValueError('Unsupported language')
    if language.casefold() in {value.casefold() for value in _seen}:raise ValueError('Circular language dependency')
    custom=registry_root(root)/language/'pack.json'
    if language not in LOCALES or (language=='ru-RU' and not (root/'localization/ru-RU/pack-ready.json').exists() and custom.exists()):
        profile=resolve_pack(root,language,allow_partial=allow_partial,
            resolve_base=lambda locale,seen:resolve_language(root,locale,allow_partial=allow_partial,_seen=seen,_discovery=_discovery),_seen=_seen,materialize=not _discovery)
        # This trusted runtime is shared by every imported locale; imports cannot select an EXE.
        profile['candidate_dir']='language-packs-v2'
        return profile
    if language=='ru-RU':return resolve_russian(root)
    if language=='ko-KR':
        from localization_launch import prepare_localization
        return dict(locale=language,elf=prepare_localization(root),vfs=root/'build/game-ko-full',state=root/'native/state-ko-full',subtitles=root/'localization/subtitles/runtime',battle_subtitles=True)
    if language=='ja-JP':
        if not (root/'native/vfs/source_manifest.json').is_file():raise ValueError('Japanese disc baseline is not prepared')
        return dict(locale=language,elf=root/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf',vfs=root/'native/vfs',state=root/'native/state',subtitles=root/'localization/ja-JP/subtitles',battle_subtitles=False)
    ready=root/'localization/en-US/pack-ready.json'
    if not ready.is_file():raise ValueError('English pack is still being assembled. Select Japanese or Korean.')
    doc=json.loads(ready.read_text(encoding='utf8'))
    if doc.get('locale')!='en-US' or doc.get('status')!='full_draft' or doc.get('untranslated')!=0:
        raise ValueError('English pack is incomplete')
    def local(value,prefix):
        path=(root/value).resolve()
        allowed=[(root/prefix).resolve()]
        # Current artifacts may be gathered centrally with legacy directory junctions.
        if prefix in ('build','native','localization/en-US/subtitles'):
            allowed.append((root/'final/english').resolve())
        if not any(path.is_relative_to(base) for base in allowed):raise ValueError('English resource outside its isolated directory')
        return path
    profile=dict(locale=language,elf=local(doc['elf'],'build'),
                 vfs=local(doc['vfs'],'build'),state=local(doc['state'],'native'),
                 subtitles=local(doc['subtitles'],'localization/en-US/subtitles'),
                 battle_catalog=local(doc['battle_catalog'],'localization/en-US/subtitles'),
                 candidate_dir=doc['candidate_dir'],battle_subtitles=True)
    if not profile['vfs'].name.startswith('english-preview-') or not profile['state'].name.startswith('state-en-'):
        raise ValueError('English pack requires independent VFS and HDD state')
    if profile['elf'].parent!=profile['vfs']:raise ValueError('English ELF must belong to the frozen VFS version')
    proved=set()
    for item in doc['proofs']:
        path=local(item['path'],'.')
        if not path.is_file() or file_digest(path)!=item['sha256']:
            raise ValueError('English resource proof changed: '+item['path'])
        proved.add(path)
    required={profile['elf'],profile['battle_catalog'],profile['subtitles']/'tracks.tsv',profile['vfs']/'PS3_GAME/USRDIR/DATA/PACK/PACK.IDX'}
    if not required<=proved:raise ValueError('English resource proofs missing')
    return profile


def resolve_russian(root):
    root=Path(root).resolve();ready=root/'localization/ru-RU/pack-ready.json'
    if not ready.is_file():raise ValueError('Russian text pack is still being prepared.')
    doc=json.loads(ready.read_text(encoding='utf8'))
    if doc.get('locale')!='ru-RU' or doc.get('image_locale')!='en-US' or doc.get('status')!='full_draft' or doc.get('untranslated')!=0:
        raise ValueError('Russian text pack is incomplete or uses an unexpected image locale')
    def local(value):
        path=(root/value).resolve()
        allowed=(root/'final/russian',root/'localization/ru-RU',root/'native/subtitle-candidate')
        if not any(path.is_relative_to(p.resolve()) for p in allowed):raise ValueError('Russian resource outside isolated roots')
        return path
    profile=dict(locale='ru-RU',elf=local(doc['elf']),vfs=local(doc['vfs']),state=local(doc['state']),
                 subtitles=local(doc['subtitles']),battle_catalog=local(doc['battle_catalog']),candidate_dir=doc['candidate_dir'],battle_subtitles=True)
    if profile['elf'].parent!=profile['vfs']:raise ValueError('Russian ELF must belong to its frozen VFS')
    proved=set()
    for item in doc['proofs']:
        path=local(item['path'])
        if not path.is_file() or file_digest(path)!=item['sha256']:raise ValueError('Russian resource proof changed: '+item['path'])
        proved.add(path)
    required={profile['elf'],profile['battle_catalog'],profile['subtitles']/'tracks.tsv',profile['vfs']/'PS3_GAME/USRDIR/DATA/PACK/PACK.IDX'}
    if not required<=proved:raise ValueError('Russian resource proofs missing')
    return profile
