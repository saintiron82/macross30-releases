"""Install reviewed extra display strings into exact original ELF slots only."""
import argparse
import json
import re
import struct
import textwrap
import unicodedata
import numpy as np
from english_pack import ROOT,digest,write
from gop_strings import TOKEN

BASE=ROOT/'localization/en-US/review-v10'
# Original music titles and credit fields, rather than unconstrained model names.
MUSIC={
 '愛・おぼえていますか':'Do You Remember Love?', '突撃ラブハート':'Totsugeki Love Heart',
 "射手座☆午後九時Don't be late":"Iteza 9PM Don't be late", '星間飛行':'Starflight',
 'プラネット・クレイドル':'Planet Cradle', 'ワンダーリング':'Wonder Ring',
 'サヨナラノツバサ':'Sayonara no Tsubasa', 'ミーナ歌3':'Mina Song 3',
 '滅びの歌':'Song of Destruction', 'サラ':'Sara', 'ミュン':'Myung',
 'プロローグ(ゼロ)':'Prologue (Zero)', 'ブリッツクリーク':'Blitzkrieg',
 'ガウォーク・ストライクス':'GERWALK Strikes', '戦争の臓腑':'Guts of War',
 '巨人達の咆哮':'Roar of the Giants', 'フォーメーション・テイクオフ':'Formation Takeoff',
 'リメンバー・クァドラン':'Remember Queadluun', 'エア・ビート':'Air Beat', '遭遇':'Meeting',
 'サターン・コンフリクト':'Saturn Conflict', 'クァドランキルカ':'Queadluun Kilka',
 '拒絶する力':'Repelling Power', 'リトル・ワンダー':'Little Wonder',
 'エースフロンティア':'Ace Frontier', 'カタパルト・スタンバイ':'Catapult Standby',
 'トライアングル・フォース':'Triangle Force', 'トライアンフォント・リターン':'Triumphant Return',
 'ひとりぼっち':'All Alone', 'フェスティバル':'Festival', 'ラヴァーズ':'Lovers',
 '一緒に帰ろう':"Let's Go Home", '責任取ってよ!':'Take Responsibility!',
 '恋する乙女、夢見る季節':'Girls in Love, Seasons of Dreams', 'はじまりの終わり':'End of the Beginning',
 'Tフロンティア':'T Frontier', '健康的辺境生活':'Healthy Frontier Life', 'オイルと砂塵':'Oil and Sand',
 'マリンスノー':'Marine Snow', '夢見る星ウロボロス':'Dreaming Planet Ouroboros',
 '楼蘭幻想紀行':'Loulan Fantasy Journey', '永久凍土':'Permafrost', '面的制圧':'Area Control',
 '禁断のアーカイブ':'Forbidden Archive', '眠りつく機械':'Sleeping Machines',
 'さすらう艦ゲフィオン':'Wandering Ship Gefion', 'ヒドラジン・カクテル':'Hydrazine Cocktail',
 '鋼鉄のゆりかご':'Cradle of Steel', '作戦会議':'War Council', 'いつまでも学生気分':'Forever a Student',
 'ふたりの秘密兵器':'Our Secret Weapon', '危機':'Crisis', '隔てるもの':'What Divides Us',
 'カフェイン中毒':'Caffeine Addiction', '荒野の風':'Wildland Wind', 'イレギュラー・ワン':'Irregular One',
 '名もなきイカロス':'Nameless Icarus', '歌姫救出作戦!':'Rescue the Songstress!',
 '結界':'Barrier', '夢と妄執の果て':"Dreams and Obsession", 'ドリームチーム':'Dream Team',
 '凍てつく炎':'Frozen Flame', 'オーロラの中へ':'Into the Aurora', '無事故無違反':'A Clean Record',
 '喪失':'Loss', 'パンドラの箱':"Pandora's Box", '手がかり':'Clue', '百戦練磨':'Battle-Hardened',
 '決戦':'Finale', '咆哮':'Roar', 'ブリタイ=シティ':'Britai City', 'ワルツ':'Waltz',
 '気分転換は大事':'A Change of Pace', 'お仕事アルよ!':"There's Work to Do!", '歪んだ希望':'Twisted Hope',
 '俺はバンデット':'I Am a Bandit', '邂逅':'Meeting', '形勢逆転':'Turning Tide',
 'ささやかな晩餐':'A Modest Supper', 'ラスト戦用':'Final Battle', '行進曲':'March', 'シリアス系':'Serious',
 'ミレーヌ機':"Mylene's Craft", 'ミーナ機':"Mina's Craft", 'ファイヤーバルキリー':'Fire Valkyrie',
 '超時空要塞マクロス':'Super Dimension Fortress Macross', '劇場版 マクロスF':'Macross Frontier Movie',
 'マクロス30~銀河を繋ぐ歌声~':'Macross 30: Voices Across the Galaxy', 'マクロスプラス':'Macross Plus',
 'マクロス7':'Macross 7', 'マクロスF':'Macross Frontier', 'ゲームオリジナル':'Game Original',
 'シェリル・ノーム':'Sheryl Nome', 'ランカ・リー':'Ranka Lee', '飯島 真理':'Mari Iijima',
 '安井 かずみ':'Kazumi Yasui', '加藤 和彦':'Kazuhiko Kato', '川野 美紀':'Miki Kawano',
 '河内 淳貴':'Junki Kawauchi', '福山 恭子':'Kyoko Fukuyama', '福山 芳樹':'Yoshiki Fukuyama',
 '佐藤 大':'Dai Sato', 'マイクスギヤマ':'Mike Sugiyama', '菅野 よう子':'Yoko Kanno',
 '松本 隆':'Takashi Matsumoto', '千菅 春香':'Haruka Chisuga', '中島 愛':'Megumi Nakajima',
 '河森 正治':'Shoji Kawamori', '歌武装':'Song Weapon', 'スキル':'Skill', 'インスト':'Inst.',
 '作品':'Work', '作詞':'Lyrics', '作曲':'Music', '歌':'Song',
}
PROFILES={
 0x928d10:("A pilot with the S.M.S. Sephira branch. Rion's story begins when he visits planet Ouroboros on a mission.",'Miyu Irino'),
 0x928e50:("The brilliant, beautiful young manager of the S.M.S. Ouroboros branch. Also a mechanic, she excels at aircraft development. Ruins sometimes make her lose all sense of herself.",'Marina Inoue'),
 0x928f90:("The brilliant, beautiful young manager of the S.M.S. Ouroboros branch. Also a mechanic, she excels at aircraft development and is a highly skilled pilot.",'Marina Inoue'),
 0x9290e0:("A mysterious girl found sleeping in ancient ruins on Ouroboros. With no memory but her name, she travels with Rion and the others she meets upon awakening.",'Haruka Chisuga'),
 0x9292a8:("The Hunter Guild representative in Britai City. Her speech is unusual and she looks young, but is apparently much older. Nobody knows her real age...",'Ayana Taketatsu'),
 0x9293b8:("A former hunter whose many troubles made him a bandit. A fool at heart, he adores Aisha and looks up to Kamjin as a big brother.",'Hiroyuki Yoshino'),
 0x9294e8:("A Havamal pilot and Rion's childhood friend. He was in love with Rion's sister Mia. He works to activate Ylva Aga so he can see her again.",'Mamoru Miyano'),
 0x929638:('', 'Jouji Nakata'), 0x9296b0:('', 'Ayana Taketatsu'),
 0x929740:("A UN Forces pilot. Shot down by Nora, he washes ashore on Mayan Island and meets Sara. Despite their clashes, the two slowly come to understand each other.",'Kenichi Suzumura'),
 0x929850:("Mayan Island's priestess and guide of the winds. Deeply hostile to outsiders, she treats Shin harshly. During the struggle over the AFOS fragment, she gradually opens her heart to him.",'Sanae Kobayashi'),
 0x929980:("Sara's curious younger sister dreams of city life. She grows close to Shin as he tells her about the city after washing ashore, and begins to fall in love.",'Yuuka Nanri'),
 0x929a80:("An Anti-UN Alliance pilot whose family was killed by the UN Forces. Skilled enough to shoot Shin down twice, she teamed up with Ivanov to outmaneuver the UN Forces.",'Minami Takayama'),
 0x929b88:("An Anti-UN Alliance ace and Focker's former instructor. Known as 'Daisy Ivanov,' his piloting skills are so great that he treats Focker like a rookie.",'Ryuzaburo Otomo'),
 0x929ca0:("A pilot in SDF-1 Macross's Skull Squadron. Repeated battles with the Zentradi help him mature into a pilot worthy of succeeding as the squadron's leader.",'Kenji Nojima'),
 0x929de0:("An immensely popular idol aboard Macross. By ending the war with her singing in the battle against Bodolza, she becomes a legendary songstress remembered for generations.",'Mari Iijima'),
 0x929f70:("An elite UN Forces officer and SDF-1 Macross's chief flight controller. Stern and devoted to duty, she is called an 'old lady' by Hikaru but gradually falls for him.",'Mika Doi'),
 0x92a0a8:("A belligerent, hot-tempered Zentradi division commander. He leads from the front, obsessed with sinking Macross. While many choose life alongside Earth, he continues seeking battle.",'Kosuke Meguro'),
 0x92a1c0:("An exceptional pilot selected to test a new fighter. Once close to Myung and Guld, he cut ties after a certain incident. A troublemaker from Dalmatian High School.",'Takumi Yamazaki'),
 0x92a308:("Lead developer and pilot of the BDI-equipped YF-21 prototype. He clashes fiercely with Isamu over the next-generation fighter competition and their childhood friend Myung.",'Unsho Ishizuka'),
 0x92a468:("Sharon's producer and a childhood friend of Isamu and Guld. An incident drove them apart. Reunited on Eden after seven years, she finds herself torn between the two.",'Rika Fukami'),
 0x92a580:("A virtual idol who quickly becomes the galaxy's most popular star. Her hypnotic songs let her control people and plunge Macross City into chaos.",'Mako Hyodo'),
 0x92a6b0:("'My profile? Don't go digging into someone's past. Just listen to my songs instead. If you want to know me, my music will tell you. Now, let's go!'",'Nobutoshi Canna'),
 0x92a810:("FIRE BOMBER's bassist and vocalist. The daughter of Max and Milia, she is also an excellent pilot. She is at an age when both Basara and Gamlin catch her eye.",'Tomo Sakurai'),
 0x92a920:("An elite pilot who joined the special attack unit Diamond Force at a young age. He initially resented Basara singing in battle, but comes to trust him and the power of song.",'Takehito Koyasu'),
 0x92aa68:("Commander of the Macross 7 emigrant fleet. Though retired from active duty, his genius piloting skills remain, and he fights alongside Milia. A genius who seems never to age.",'Sho Hayami'),
 0x92abe8:("A former Meltrandi ace and mayor of City 7. She still sorties when the city is attacked, earning its citizens' support as a mayor who fights for them.",'Eri Takeda'),
 0x92ad50:("A high school student in Mihoshi Academy's pilot course. Flying a VF-25 during the Vajra attack leads him to join S.M.S. and become caught up in the fighting.",'Yuichi Nakamura'),
 0x92aec0:("Alto's classmate at Mihoshi Academy and an expert S.M.S. sniper. He grew up with Klan, and his teasing of her micronized form often ends in arguments.",'Hiroshi Kamiya'),
 0x92b018:("Heir to the L.A.I. corporation, he skipped grades to enter Mihoshi Academy. Like Michel, he belongs to S.M.S. and excels at operating electronic equipment.",'Jun Fukuyama'),
 0x92b148:("Leader of S.M.S. Skull Squadron. He took Ranka in eleven years ago and has lovingly raised her as her older brother, though he can be a little overprotective...",'Katsuyuki Konishi'),
 0x92b280:("A Zentradi who leads S.M.S. Pixie Squadron. She has a striking figure in giant form, but for some reason takes on a little girl's physique when micronized.",'Megumi Toyoguchi'),
 0x92b3f0:("Klan in micronized form. Both her physique and personality become childlike. Michel often teases her about it, leading to endless arguments.",'Megumi Toyoguchi'),
 0x92b530:("Captain of Macross Quarter. A battle-hardened former UN Forces pilot, his broad perspective and ability to read a battle make him a pillar of S.M.S.",'Toru Okawa'),
 0x92b698:("A cybergrunt with implants throughout his body. His superhuman abilities make him Sheryl's bodyguard. As Ranka's older brother, he also never fails to support her.",'Soichiro Hoshi'),
 0x92b7f8:("The 'Super Dimension Cinderella,' an idol whose rise to stardom began when she met Sheryl. She has a budding crush on Alto and is doing her best to win him over.",'Megumi Nakajima'),
 0x92b9c0:("The 'Galactic Fairy,' a songstress so popular that people say no day in the galaxy passes without hearing her songs. She is trying hard to capture Alto's heart.",'Aya Endo'),
 0x92bbb0:("The talented manager who made Sheryl a top idol. She is also a scientist involved in Vajra technology and a high-ranking officer of Macross Galaxy.",'Kikuko Inoue'),
}
OVERRIDES={
 0x909cf8:"Ruins' Sleeping Beauty",0x93c168:"Ruins' Sleeping Beauty",0x94d758:"Ruins' Sleeping Beauty",
 0x9133c0:'All-Out Fire',0x9138d8:'Double Strike Pack',0x9138f8:'VF-1 Double Strike Pack',
 0x913958:'Additional VF-1 equipment. The Double Strike Pack replaces the weapons on both boosters with twin beam cannons, giving it even more firepower than the Strike Pack.',
 0x918720:'MY FRIENDS - Temporary Invincibility',0x919d18:'Wonder Ring - Temporary Invincibility',
 0x91c140:'Standard equipment\nMain: Anti-air/ground gun\nSpecial 1: Triple heavy anti-ground/ship missiles\nSpecial 2: Quad 320mm railguns\nSpecial 3: Reaction weapons',
 0x91dd28:'Standard equipment\nMain: Heavy quantum beam gun pod\nSpecial 1: Micro-missiles (Rapid/Blast)\nSpecial 2: Micro-missiles (Multi/Blast)\nSpecial 3: Retractable twin MDE beam cannon',
 0x91e408:'Sayonara no Tsubasa - Temporary Invincibility',
 0x9204f0:'Song (HOLY LONELY LIGHT)\nA fiery song that shakes the soul',
 0x9206c8:"Mina's Song - HP Recovery",0x9206f8:"Mina's Song",0x921668:'Sayonara no Tsubasa - HP Recovery',0x9216a8:'Sayonara no Tsubasa',
 0x928cc8:'Rion in the Pit',0x92aa20:'Gamlin Kizaki (Brainwashed)',
 0x92acf8:'Milia Fallyna Jenius (Brainwashed)',0x92b3b0:'Klan (Micronized P-Suit)',0x92b4e0:'Klan (Micronized S.M.S. Uniform)',
 0x92b958:'Ranka (Sayonara no Tsubasa)',0x92bb40:'Sheryl (Sayonara no Tsubasa)',0x92b8e0:'Ranka (Interstellar Flight)',
 0x92bdf8:'A mantis-like Vajra with scythes on both arms. A fast combat type with great melee ability and mobility. It tried to capture and carry Ranka away, but its purpose is unknown.',
 0x92bd10:'A large Vajra over five times the size of a small one. A huge beam weapon on its back can shatter a warship in one hit. Missiles and cannon-like weapons give it immense firepower.',
 0x94d728:'Boosts the defense of your craft and wingman.',
}

def translated(r,draft):
    off=r['offset'];source=r['source'];count=source.count('\n')+1
    if off in PROFILES:
        body,actor=PROFILES[off]
        if count==2:return '\n(Voice: '+actor+')'
        words=body.split();lines=[]
        for i in range(count-1):
            take=round(len(words)/(count-1-i));lines.append(' '.join(words[:take]));words=words[take:]
        return '\n'.join(lines+['(Voice: '+actor+')'])
    if 0x925918<=off<0x928cc8:
        # User policy: music titles/credits stay original in both locales.
        return source
    text=OVERRIDES.get(off,draft)
    text=unicodedata.normalize('NFKC',text).replace('●','-').replace('※','-').replace('★','').replace('×','x')
    if text.count('\n')!=count-1:
        words=text.split();lines=[]
        for i in range(count):
            take=round(len(words)/(count-i));lines.append(' '.join(words[:take]));words=words[take:]
        text='\n'.join(lines)
    return text


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--apply',action='store_true');args=parser.parse_args()
    scope=json.loads((BASE/'supplemental-elf-scope.json').read_text(encoding='utf8'))
    drafts=json.loads((BASE/'supplemental-elf-drafts.json').read_text(encoding='utf8'))
    assert drafts['scope_sha256']==digest((BASE/'supplemental-elf-scope.json').read_bytes())
    original=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    path=ROOT/'localization/en-US/build/EBOOT.full-english.elf';baseline=path.read_bytes()
    assert digest(original)==scope['original_sha256']
    # Base may include regenerated typed catalog translations, but supplemental
    # intervals must still contain the exact original bytes.
    output=bytearray(baseline);rows=[];holds=[];allowed=set()
    words=np.frombuffer(original[:len(original)//4*4],dtype='>u4')
    # ELF load mapping for each candidate comes from the original source scan.
    candidates=json.loads((ROOT/'analysis/localization/elf_japanese_candidates.json').read_text(encoding='utf8'))['entries']
    eas={r['file_offset']:r['virtual_address'] for r in candidates}
    # Reject any interval in executable sections before it becomes editable.
    sh=struct.unpack_from('>Q',original,40)[0];stride,num=struct.unpack_from('>HH',original,58)
    code=[]
    for i in range(num):
        p=sh+i*stride;flags=struct.unpack_from('>Q',original,p+8)[0];f,n=struct.unpack_from('>QQ',original,p+24)
        if flags&4:code.append((f,f+n))
    for r in scope['entries']:
        record=drafts['entries'][r['id']];assert record['source']==r['source']
        off=r['offset'];old=r['source'].encode()+b'\0';assert baseline[off:off+len(old)]==old
        target=translated(r,record['translation']);raw=target.encode()+b'\0'
        # Only independently verified original NUL padding before the next
        # 8-byte boundary is available; never consume a neighboring literal.
        cap=len(old)
        while cap<len(old)+7 and (off+cap)%8 and original[off+cap]==baseline[off+cap]==0:cap+=1
        problems=[]
        ea=eas[off]
        interior=np.flatnonzero((words>ea)&(words<ea+len(old)-1))
        if len(interior):problems.append('interior literal pointer requires review')
        if re.search(r'[^\x20-\x7e\n]',target) and not 0x925918<=off<0x928cc8:problems.append('non-ASCII text')
        if len(raw)>cap:problems.append('slot capacity')
        if target.count('\n')!=r['source'].count('\n'):problems.append('line count')
        if TOKEN.findall(target)!=TOKEN.findall(r['source']):problems.append('control tokens')
        if any(a<off+cap and off<b for a,b in code):problems.append('executable section')
        row=dict(id=r['id'],offset=hex(off),source=r['source'],target=target,source_nul_bytes=len(old),capacity=cap,target_nul_bytes=len(raw),interior_pointer_offsets=[hex(int(n)*4) for n in interior],review='source meaning reviewed; personal name readings and game layout need final review')
        rows.append(row)
        if problems:holds.append(dict(**row,problems=problems));continue
        assert not allowed.intersection(range(off,off+cap))
        output[off:off+cap]=raw.ljust(cap,b'\0');allowed.update(range(off,off+cap))
    report=dict(baseline_sha256=digest(baseline),original_sha256=digest(original),entries=rows,holds=holds,
                translated=len(rows)-len(holds),output_sha256=digest(output),game_render_verified=False)
    write(BASE/'supplemental-elf-review.json',report)
    print(dict(translated=report['translated'],holds=len(holds),examples=[(r['offset'],r['target'],r['problems']) for r in holds[:20]]))
    if args.apply:
        if holds:raise ValueError('Unresolved supplemental display review holds')
        assert len(output)==len(baseline) and all(i in allowed for i,(a,b) in enumerate(zip(baseline,output)) if a!=b)
        for row in rows:
            off=int(row['offset'],16);assert output[off:output.index(0,off)].decode()==row['target']
        path.write_bytes(output)
        manifest_path=path.with_name('full-elf-manifest.json');manifest=json.loads(manifest_path.read_text(encoding='utf8'))
        manifest.update(pre_supplemental_sha256=manifest['output_sha256'],output_sha256=digest(output),supplemental_display_literals=len(rows),supplemental_review=str(BASE/'supplemental-elf-review.json'))
        write(manifest_path,manifest)
