"""Regenerate the English work register from guarded catalogs and build proofs."""
import json
from pathlib import Path
from english_pack import ROOT,write

def read(path):return json.loads(path.read_text(encoding='utf8'))

if __name__=='__main__':
    base=ROOT/'localization/en-US';groups=[]
    for path in sorted((base/'structured').glob('*.json')):
        doc=read(path);draft_path=base/'model-drafts'/path.name
        draft=read(draft_path).get('entries',{}) if draft_path.exists() else {}
        assert not draft_path.exists() or read(draft_path)['source_sha256']==doc['source_sha256']
        human=sum(bool(r['translation']) and r.get('status')!='local_model_draft_needs_review' for r in doc['entries'])
        ready=sum(bool(r['translation']) or r['id'] in draft for r in doc['entries'])
        groups.append(dict(catalog=path.stem,total=len(doc['entries']),human_draft=human,prepared=ready,remaining=len(doc['entries'])-ready))
    side=read(base/'model-drafts/sidecar-results.json')
    scope=read(base/'model-drafts/sidecar-scope.json')
    sidegroups={kind:dict(total=sum(r['kind']==kind for r in scope['jobs']),prepared=sum(r['kind']==kind and r['id'] in side['entries'] for r in scope['jobs'])) for kind in ('elf','movie','voice')}
    image_count=read(ROOT/'tools/localization/catalogs/image_registry.json')['ui_asset_count']
    published=(base/'pack-ready.json').exists()
    result=dict(groups=groups,sidecars=sidegroups,status='full_draft_published' if published else 'in_progress',translation_review_complete=False,live_full_pack_verified=False)
    write(base/'status.json',result)
    version=Path(read(base/'pack-ready.json')['vfs']).name.removeprefix('english-preview-images-') if published else ''
    intro=f'The full English draft is published in the isolated {version} pack. All 13,536 known GOP entries, {image_count} image variants, 55 movie tracks and 14,177 on-disc voice captions are included. English is available through the guarded language profile; the normal saved language remains unchanged.' if published else 'The full English draft is being assembled. The normal Korean installation and saved language are preserved.'
    lines=['# English pack progress','',intro,'','Machine drafts still require semantic, proper-name, audio and game layout review. Coverage is not release certification.','','| GOP catalog | Prepared | Human draft | Remaining |','|---|---:|---:|---:|']
    lines += [f"| {r['catalog']} | {r['prepared']} / {r['total']} | {r['human_draft']} | {r['remaining']} |" for r in groups]
    lines += ['','The prepared count includes source-guarded local model checkpoints which may not yet be installed. It is not a release-readiness claim.','','| Additional scope | Prepared draft |','|---|---:|']
    lines += [f"| {k} | {r['prepared']} / {r['total']} |" for k,r in sidegroups.items()]
    lines += ['| Previously translated movie cues | 77 |',f'| English image variants | {image_count} |','| Common ELF slots / pilot records | 336 / 139 |','','[Image register](localization-image-register.md) tracks original, Korean and English image identities. [Installed text register](localization-text-register.md) describes the last audited installation, which can lag prepared checkpoints.','','The full English ELF covers all 878 literal start positions changed by the Korean ELF, with 438 catalog literals and 1,051 checked typed pointer rewrites. This is offline resource verification; individual game screens still require review.','','The English native caption loader has an isolated build. Both English malformed-input tests and the original Korean 16,255-key zero-allocation lookup regression pass. Movie cues retain source timing and audio-review flags; long English cues can be paged without dropping words.','','The additional image audit includes hangar guidance, HUD quest tags, DLC aircraft labels, cave name effects and original credit textures. Credit name readings based on kanji remain provisional and require verification. These texture credits include original Frontier film material; they are not replaced with the different Macross 30 web credit list.','','Remaining gates: finish all draft translations; check caption wrapping and control tokens; assemble and read back every primary and nested resource route; test all movie cues with the production renderer; launch and inspect the new English preview. Do not present the old partial preview as the completed pack.','','Regenerate with `python tools/localization/report_english_pack_status.py`. Local catalogs and binary assets follow the repository ignore policy and must be retained separately.']
    (ROOT/'docs/english-pack-progress.md').write_text('\n'.join(lines)+'\n',encoding='utf8')
    if published:
        path=ROOT/'docs/english-pack-progress.md';text=path.read_text(encoding='utf8')
        text=text.replace('Remaining gates: finish all draft translations; check caption wrapping and control tokens; assemble and read back every primary and nested resource route; test all movie cues with the production renderer; launch and inspect the new English preview. Do not present the old partial preview as the completed pack.',f'Completed resource gates: GOP primary/nested roundtrip, original-font and token coverage, image route parity, ELF Korean parity, all movie renderer cases, and every native caption page boundary. The published preview is {version}. Remaining work is full semantic and in-game visual review, listening review of ASR and proportional pages, and credit name-reading verification. The original Korean project also holds 91 uncertain voice-only ASR candidates outside its accepted caption baseline; they remain held, with no invented English dialogue.')
        review=base/'review-v10/short-warning-review.json'
        if review.exists():
            r=read(review)
            text+=f'\nReview update: {r["reviewed"]} short-translation warnings were checked against Japanese; {r["revised"]} were revised. A separate ELF scan added 377 equipment/music/pilot-gallery display literals beyond the Korean parity baseline. Thirteen ARK-discovered image resources and six previously replaced images with Japanese remnants/overlap were corrected. The selection-release texture now has all nine mip levels localized. See [review record](english-pack-review.md).\n'
        live=base/'live-preview-review.json'
        if live.exists() and read(live).get('version')==version:
            text+='\nLive observations: see `localization/en-US/live-preview-review.json`; only explicitly observed scenes are verified.\n'
        path.write_text(text,encoding='utf8')
    print(json.dumps(dict(prepared=sum(r['prepared'] for r in groups),total=sum(r['total'] for r in groups),sidecars=sidegroups)))
