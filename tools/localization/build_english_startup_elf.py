"""Extend the isolated English ELF with reviewed startup and common-menu slots."""
import json
from english_pack import ROOT,digest,write
from patch_combat_notifications_v14 import transform


if __name__=='__main__':
    base=ROOT/'localization/en-US'
    doc=json.loads((base/'elf-ui-draft.json').read_text(encoding='utf8'))
    rows={int(r['file_offset'],0):r for r in doc['strings']}
    rows[0x8F1068]=dict(file_offset='0x8f1068',source='発進',translation='Launch')
    translations=json.loads((base/'catalogs/elf-startup.json').read_text(encoding='utf8'))
    translations.update(json.loads((base/'catalogs/elf-common.json').read_text(encoding='utf8')))
    missing=[]
    for catalog in ('ui_menu_complete','ui_basic_strings'):
        korean=json.loads((ROOT/f'localization/ko-KR/{catalog}.json').read_text(encoding='utf8'))
        for row in korean['strings']:
            source=row['source'];key=source.strip()
            text=translations.get(source,translations.get(key))
            if text is None:
                missing.append(dict(catalog=catalog,source=source,offset=row['file_offset']));continue
            if source not in translations:
                first=len(source)-len(source.lstrip());last=len(source.rstrip())
                text=source[:first]+text+source[last:]
            rows[int(row['file_offset'],0)]=dict(row,translation=text)
    write(base/'build/common-ui-missing.json',dict(missing=missing))
    if missing:raise ValueError(f'{len(missing)} basic UI translations missing; see common-ui-missing.json')
    source=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    slots=[(offset,row['source'],row['translation']) for offset,row in sorted(rows.items())]
    data,changes=transform(source,slots,set(range(32,127)),doc['source_sha256'])
    names=json.loads((base/'catalogs/pilot-names.json').read_text(encoding='utf8'))
    authority=json.loads((ROOT/'localization/ko-KR/ui_fixed_names.json').read_text(encoding='utf8'))
    patched=bytearray(data);name_changes=[]
    for row in authority['patch_candidates']:
        offset=int(row['file_offset'],0);size=row['slot_bytes'];old=source[offset:offset+size]
        if size!=64 or digest(old)!=row['original_slot_sha256'] or data[offset:offset+size]!=old:
            raise ValueError('Pilot field source/size mismatch')
        before=row['source'].encode('utf8')+b'\0'
        if not old.startswith(before) or any(old[len(before):]):raise ValueError('Pilot field boundary mismatch')
        text=names[row['source']];new=text.encode('ascii')+b'\0'
        if len(new)>size:raise ValueError('Pilot field overflow')
        patched[offset:offset+size]=new.ljust(size,b'\0')
        if source[offset+64:offset+216]!=patched[offset+64:offset+216]:raise ValueError('Pilot metadata changed')
        name_changes.append(dict(offset=hex(offset),source=row['source'],translation=text))
    data=bytes(patched)
    output=base/'build/EBOOT.preview-v4.elf';output.write_bytes(data)
    write(base/'build/startup-v4-manifest.json',dict(source_sha256=digest(source),output_sha256=digest(data),
        output=str(output),translated_slots=len(changes),pilot_records=len(name_changes),pilot_changes=name_changes,
        changes=changes,outside_slots_changed=0,game_verified=False))
    print(json.dumps(dict(output=str(output),translated_slots=len(changes),sha256=digest(data))))
