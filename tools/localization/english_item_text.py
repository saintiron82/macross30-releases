"""Reviewed item vocabulary and complete numeric templates; reject partial Japanese output."""
import re
import unicodedata

WORDS={
'テスト用アイテム':'Test Item ', 'テスト':'Test ', 'リペアキット':'Repair Kit ', 'レストレーションキット':'Restoration Kit ',
'ランカ人形':'Ranka Doll','まぐろ饅':'Tuna Bun','ダブル':'Double','ミンメイ人形':'Minmay Doll','巨大まぐろ':'Giant Tuna',
'弾薬BOX':'Ammo Box','ガンポッド':'Gunpod','ライフル':'Rifle','特殊':'Special ',
'ハンドグレネード':'Hand Grenade ','プラズマグレネード':'Plasma Grenade ','設置式大型爆弾':'Large Planted Bomb ',
'高性能レーダー':'Advanced Radar ','あい君人形':'Ai-kun Doll','簡易フォールドブースター':'Basic Fold Booster',
'ステーキ':'Steak ','パインサラダ':'Pineapple Salad ','ゼントラ盛':'Zentradi Size','大盛':'Large',
'パインケーキ':'Pineapple Cake ','ブロック':'Slice','ハーフ':'Half','ホール':'Whole',
'究極カバ牛アイスクリーム':'Ultimate Hippo-cow Ice Cream','至高カバ牛アイスクリーム':'Supreme Hippo-cow Ice Cream','カバ牛アイスクリーム':'Hippo-cow Ice Cream',
'アフターバーナー':'Afterburner ','獅子印 三島産キノコ':'Lion Brand Mishima Mushroom ','三島産キノコ':'Mishima Mushroom ',
'体力':'Vitality','射撃':'Shooting','格闘':'Melee','防御':'Defense','速度':'Speed','集中':'Focus','魂':'Spirit',
'UPアイテム':' Up Item','ダミー':'Dummy','お金':'Money','カスタムパーツ':'Custom Parts ','装甲':'Armor',
'チタンボルト':'Titanium Bolt','チタニウム合金':'Titanium Alloy','強化ステンレス鋼':'Reinforced Stainless Steel',
'強化合成ゴム':'Reinforced Synthetic Rubber','強化ジェラルミン':'Reinforced Duralumin','OTMボルト':'OTM Bolt',
'OTMプラスティック':'OTM Plastic','OTM超合金':'OTM Superalloy','ハイパーCC':'Hyper CC',
'エネルギー転換ボルト':'Energy-conversion Bolt','OTMニッケル系合金':'OTM Nickel Alloy',
'エネルギー転換装甲パーツ':'Energy-conversion Armor Parts ','エネルギー転換装甲':'Energy-conversion Armor ',
'OTM結晶構造ボルト':'OTM Crystalline Bolt','OTM炭素系合金':'OTM Carbon Alloy',
'フォールドカーボン':'Fold Carbon ','フォールドクォーツ':'Fold Quartz ','ウロボロスカーボン':'Ouroboros Carbon','ウロボロスクォーツ':'Ouroboros Quartz',
'専用パーツ':' Custom Parts','フレームパーツ':' Frame Parts ','装甲パーツ':' Armor Parts ','ジャンク':'Junk ',
'ゼントラ機系':'Zentradi Aircraft','ゼントラPS':'Zentradi PS','リガード':'Regult','ヌージャデル・ガー':'Nousjadeul-Ger','ヌージャ':'Nousjadeul',
'クァドラン・レア':'Queadluun-Rhea','クァドラン・ロー':'Queadluun-Rau','クァドラン':'Queadluun',
'バルキリー':'Valkyrie','バジュラの外骨格':'Vajra Exoskeleton ','バジュラの堅尾':'Vajra Hardened Tail ',
'バジュラの外殻':'Vajra Outer Shell ','バジュラの薄羽':'Vajra Membrane Wing ',
'ディヤウスの鋭爪':'Dyaus Sharp Claw ','ディヤウス':'Dyaus',
'エンジンユニット':' Engine Unit ','ターボファンジェット改':' Improved Turbofan Jet ','ターボファンジェット':' Turbofan Jet ',
'熱核Bタービン':' Thermonuclear Burst Turbine ','熱核タービンST2':' Thermonuclear Turbine ST2 ',
'熱核タービン':' Thermonuclear Turbine ','電磁プラズマロケット':' Electromagnetic Plasma Rocket ',
'熱核反応炉':' Thermonuclear Reactor ','熱核コンバータ':' Thermonuclear Converter ',
'小型熱核反応タービン':'Compact Thermonuclear Turbine ','小型熱核コンバータ':'Compact Thermonuclear Converter ',
'ウロボロス水晶':'Ouroboros Crystal ','設計図':' Blueprint ', 'スーパーパック':' Super Pack', 'ストライクパック':' Strike Pack',
'アーマードパック':' Armored Pack','ツインブースター':' Twin Booster','パワード・スーツ':'Powered Suit',
'スカル小隊機':' Skull Squadron','レイブンズ機':' Ravens','レイヴンズ機':' Ravens','ケーニッヒ':'Koenig',
'イサム機':' Isamu Custom','オズマ機':' Ozma Custom','シン機':' Shin Custom','ノーラ機':' Nora Custom','イワノフ機':' Ivanov Custom',
'ミレーヌ機':' Mylene Custom','一条機':' Hikaru Custom','ミーナ機':' Mina Custom','アイシャ機':' Aisha Custom',
'バサラ機':' Basara Custom','ガルド機':' Guld Custom','マックス機':' Max Custom','ミリア機':' Milia Custom',
'ガムリン機':' Gamlin Custom','ガネス機':' Ganes Custom','リオン機':' Rion Custom','アルト機':' Alto Custom',
'ミハエル機':' Michel Custom','ルカ機':' Luca Custom','クラン機':' Klan Custom','ブレラ機':' Brera Custom',
'リーロン機':' Lilong Custom','ロッド機':' Rod Custom','改':' Custom',
'石板-':'Stone Tablet ', '藤堂ノート':'Todo Notebook ', 'ページ':'Page ',
'スカル小隊隊長機':' Skull Squadron Leader','ファストパック':' FAST Pack','ゴーストブースター':' Ghost Booster','トルネードパック':' Tornado Pack',
'群島の落し物':'Archipelago Lost Item ','タイムカプセル群島':'Archipelago Time Capsule ','タイムカプセル砂漠':'Desert Time Capsule ',
'タイムカプセル氷河':'Glacier Time Capsule ','砂漠の宝':'Desert Treasure ','氷河の宝':'Glacier Treasure ',
'群島Lv':'Archipelago Lv','砂漠Lv':'Desert Lv','氷河Lv':'Glacier Lv','コースプラン':' Course Plan','級証明書':'-Rank Certificate',
}

EXACT={
22:'Test 1',23:'Test Item 1',25:'Test 2',26:'Test Item 2',
44:'Restores 20000 HP.',47:'Fully restores HP.',65:'Gradually restores HP for you and nearby wingmen\nfor a limited time.',
68:'Restores 3 SP blocks\nfor you and nearby wingmen.',71:'Fully restores SP\nfor you and nearby wingmen.',
74:'Temporarily increases SP recovery\nfor you and nearby wingmen.',77:'Fully restores HP and SP\nfor you and nearby wingmen.',
80:'Gunpod ammunition box (automatic reload).',83:'Rifle ammunition box (automatic reload).',
86:'Reloads Special Weapon 1.\n(Automatic reload.)',89:'Reloads Special Weapon 2.\n(Automatic reload.)',92:'Reloads Special Weapon 3.\n(Automatic reload.)',
110:'Throws a hand grenade. (Damage: 5000)',113:'Throws a hand grenade. (Damage: 10000)',
146:'Reveals the entire map\nwhen used in a cave or base.',160:'Repels wild Valkyries, reducing encounters.',163:'Attracts wild Valkyries, increasing encounters.',
166:'Folds you back to the entrance\nwhen used in a cave or base.',
265:'Raises the level of one selected character (out of six) by 1.',291:'Dummy item used to add money.',
1090:"Rion's Pendant",1093:"Aisha's Glasses",1096:"Mina's Hair Ornament",1099:"Lilong's Hunter Manual",1102:"Ganes's Fur",1105:"Rod's Photograph",
1108:"Shin's Dog Tag",1111:"Sara's Yariwei",1114:"Mao's Necklace",1117:"Nora's Goggles",1120:"Ivanov's Flight Manual",1123:"Hikaru's Titanium Medal",
1126:"Minmay's Microphone",1129:"Misa's Swimsuit Photo",1132:"Kamjin's Cape",1135:"Isamu's GUNS CDV",1138:"Guld's Charm",1141:"Myung's Bracelet",
1144:"Sharon's AI Chip",1147:"Basara's Guitar",1150:"Mylene's Bass",1153:"Gamlin's Ring",1156:"Max's Captain's Cap",1159:"Milia's Earrings",
1162:"Alto's Paper Airplane",1165:"Michel's Broken Glasses",1168:"Luca's Laptop",1171:"Ozma's Rare Fire Bomber Disc",1174:"Klan's Ribbon",
1177:"Jeffrey's Captain's Cap",1180:"Brera's Harmonica",1183:"Ranka's Mobile Phone",1186:"Sheryl's Earring",1189:"Grace's Glasses"
}

def translate(index, source):
    if index in EXACT:return EXACT[index].replace('\n',r'\n')
    s=unicodedata.normalize('NFKC',source).strip()
    patterns=[
      (r'自機および、自機周辺にいる僚機のHPを\\n(\d+)回復。',lambda m:f'Restores {m[1]} HP to you\\nand nearby wingmen.'),
      (r'自機および、自機周辺にいる僚機のHPを\\n完全回復。',lambda m:'Fully restores HP to you\\nand nearby wingmen.'),
      (r'僚機救助用アイテム。\\n自機周辺にいる行動不能な僚機のHPを\\n(\d+)回復させた状態で復活させる。',lambda m:f'Revives nearby disabled wingmen\\nwith {m[1]} HP\\nrestored.'),
      (r'僚機救助用アイテム。\\n自機周辺にいる行動不能な僚機のHPを\\n完全回復させた状態で復活させる。',lambda m:'Revives nearby disabled wingmen\\nwith their HP\\nfully restored.'),
      (r'ハンドグレネードを投擲する。\\n\(攻撃力(\d+)\)',lambda m:f'Throws a hand grenade.\\nAttack Power: {m[1]}'),
      (r'プラズマグレネードを投擲する。\\n\(敵を(\d+)秒間動けなくする。\)',lambda m:f'Throws a plasma grenade.\\nImmobilizes enemies for {m[1]} seconds.'),
      (r'敵が近づくと爆発する爆弾を設置する。\\n\(攻撃力(\d+)\)',lambda m:f'Plants a bomb triggered by nearby enemies.\\nAttack Power: {m[1]}'),
      (r'洞窟・基地内で使用すると、\\n(\d+)分間マップ全域が表示される。',lambda m:f'Reveals the entire cave or base map\\nfor {m[1]} minutes.'),
      (r'一定時間、スピードを上昇させる。\\n\((\d+)秒間\)',lambda m:f'Temporarily increases speed.\\nDuration: {m[1]} seconds'),
      (r'一定時間、攻撃力を上昇させる。\\n\(攻撃力([\d.]+)倍\)',lambda m:f'Temporarily increases attack power.\\nAttack Power: x{m[1]}'),
      (r'一定時間、防御力を上昇させる。\\n\(防御力([\d.]+)倍\)',lambda m:f'Temporarily increases defense.\\nDefense: x{m[1]}'),
      (r'一定時間、防御力を減少させ、\\n攻撃力を上昇させる。\\n\(攻撃力([\d.]+)倍、防御力([\d.]+)倍\)',lambda m:f'Temporarily lowers defense\\nand raises attack power.\\nAttack: x{m[1]} / Defense: x{m[2]}'),
      (r'一定時間、攻撃力を減少させ、\\n防御力を上昇させる。\\n\(防御力([\d.]+)倍、攻撃力([\d.]+)倍\)',lambda m:f'Temporarily lowers attack power\\nand raises defense.\\nDefense: x{m[1]} / Attack: x{m[2]}'),
      (r'(フレーム|装甲|エンジン)パーツ。\\n開発ポイント:(\d+)pt',lambda m:{'フレーム':'Frame','装甲':'Armor','エンジン':'Engine'}[m[1]]+f' parts.\\nDevelopment Points: {m[2]} pt'),
    ]
    for pattern,target in patterns:
        m=re.fullmatch(pattern,s)
        if m:return target(m)
    stats={'体力':'Vitality','射撃':'Shooting','格闘':'Melee','防御':'Defense','速度':'Speed','集中':'Focus','魂':'Spirit','レベル':'Level'}
    m=re.fullmatch(r'出撃中のキャラクターの\\n(.+?)(?:ステータス)?を(\d+)アップさせる。',s)
    if m and m[1] in stats:return f'Raises deployed characters\'\\n{stats[m[1]]} by {m[2]}.'
    m=re.fullmatch(r'指定キャラ\(6人から選択\)の(.+?)が(\d+)上がる。',s)
    if m and m[1] in stats:return f'Raises one selected character\'s {stats[m[1]]} by {m[2]}.'
    m=re.fullmatch(r'サブクエスト納品用アイテム。\\nハンターの腕前([A-Z]+)級を示す証。([A-Z]+)級に合格すると([A-Z]+)級任務が受けられるようになる。',s)
    if m and m[1]==m[2]==m[3]:return f'Subquest delivery item.\\nCertifies {m[1]}-rank hunter ability and grants access to {m[1]}-rank missions.'
    if s.startswith('装備アイテム。'):
        result=s.replace('装備アイテム。','Equipment item.')
        for jp,en in stats.items():result=result.replace(jp,en)
        if not re.search(r'[\u3040-\u30ff\u3400-\u9fff]',result):return result
    # Only names, never prose. Accept only complete dictionary coverage.
    if len(s)>85 or '。' in s or r'\n' in s:return None
    result=s
    for jp,en in sorted(WORDS.items(),key=lambda p:-len(p[0])):result=result.replace(jp,en)
    if re.search(r'[\u3040-\u30ff\u3400-\u9fff]',result):return None
    result=re.sub(r' +',' ',result).strip()
    return result if result!=s else None
