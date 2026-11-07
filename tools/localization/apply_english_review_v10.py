"""Source-bound human review of the v9 short-translation warnings."""
import json
from english_pack import ROOT, digest, write
from gop_strings import TOKEN

FIXES = {
 'gop_battletext:2227': 'We succeeded thanks to all of you. Thank you!',
 'gop_movie:228': 'Bonus: Macross Ultimate Frontier Opening',
 'gop_movie:231': 'Bonus: Macross Triangle Frontier Opening',
 'gop_scenariotext:2254': "Y-yes. I don't mind,\\nbut there's one more thing\\nI'd like to ask first...",
 'gop_scenariotext:3345': "Right, you wouldn't know, Mina. The Vajra\\nare creatures linked by fold waves.",
 'gop_scenariotext:3847': "I can't fully communicate with them either. We\\ncan only convey our feelings through song.",
 'gop_scenariotext:4376': "So even you haven't\\nfully analyzed it yet.",
 'gop_scenariotext:5351': "Anyway, there's still a lot\\nwe don't know about Protoculture.\\nLet's head deeper inside for now.",
 'gop_scenariotext:5372': "He's unbelievably self-centered\\nand hopelessly selfish.",
 'gop_scenariotext:5384': "Hehe. You really do like\\nBasara, don't you, Mylene?",
 'gop_scenariotext:5544': "I thought the Zentradi's\\nrampages were finally dying down...",
 'gop_scenariotext:6698': "Trust my work on your aircraft. Now,\\nmake sure you investigate thoroughly.",
 'gop_scenariotext:7233': "The Hunter Guild is up and running again.\\nNow, if only Sheryl would get better...",
 'gop_scenariotext:7287': "All right.\\nIf you're going to do this, Mina,\\nof course I'm going to help.",
 'gop_scenariotext:8430': "Yeah, a priestess named Sara Nome\\nactivated the ruins for us.",
 'gop_scenariotext:8989': "Since when did you\\njoin Havamal?!",
 'gop_scenariotext:9389': "Sorry I didn't introduce myself sooner.\\nI'm Grace O'Connor,\\nSheryl's manager.",
 'gop_scenariotext:11684': "Now we just need to rescue\\nMyung from Havamal...!",
 'gop_scenariotext:13423': "Probably the implant technology NUNS was\\ndeveloping to force the Zentradi into obedience.",
}

if __name__ == '__main__':
    report_path=ROOT/'localization/en-US/review-v10/short-warning-review.json'
    if report_path.exists():raise FileExistsError('Review is immutable; use a new review epoch')
    flags=json.loads((ROOT/'localization/en-US/build/semantic-review-flags.json').read_text(encoding='utf8'))['flags']
    docs={};decisions=[]
    for flag in flags:
        path=ROOT/'localization/en-US/structured'/(flag['catalog']+'.json')
        if path not in docs:docs[path]=json.loads(path.read_text(encoding='utf8'))
        entry=next(r for r in docs[path]['entries'] if r['id']==flag['id'])
        assert entry['source']==flag['source'] and entry['translation']==flag['translation']
        key=flag['catalog']+':'+str(flag['index']);target=FIXES.get(key,entry['translation'])
        assert TOKEN.findall(target)==TOKEN.findall(entry['source'])
        decisions.append(dict(id=entry['id'],source=entry['source'],before=entry['translation'],after=target,
                              decision='revised' if target!=entry['translation'] else 'accepted_concise_translation',
                              review='Japanese source meaning and English wording; audio and full scene context not verified'))
        entry.update(translation=target,status='english_draft',human_translation_reviewed=True,review_epoch='v10-short-warning')
    assert set(FIXES)=={r['id'].replace(':string:',':') for r in decisions if r['decision']=='revised'}
    for path,doc in docs.items():write(path,doc)
    write(report_path,dict(reviewed=len(decisions),revised=sum(r['decision']=='revised' for r in decisions),decisions=decisions,
                           full_semantic_review=False,source_flags_sha256=digest((ROOT/'localization/en-US/build/semantic-review-flags.json').read_bytes())))
    print(dict(reviewed=len(decisions),revised=sum(r['decision']=='revised' for r in decisions)))
