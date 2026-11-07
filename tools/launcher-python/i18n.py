TEXT = {
 'ko': {
  'game':'게임 버전', 'latest':'최신', 'unknown':'확인 중', 'missing':'미설치', 'ui':'런처 언어', 'check':'↻ 업데이트 확인',
  'locale':'플레이 언어','pack':'언어팩 버전','japanese':'원본 일본어 · 별도 언어팩 없음','play':'플레이','update':'업데이트',
  'working':'적용 중','running':'게임 실행 중','ready':'플레이할 준비가 되었습니다.','needsUpdate':'선택한 언어로 플레이하려면 업데이트가 필요합니다.',
  'coreUpdate':'기본 게임 패치 다운로드','languageUpdate':'선택한 언어팩 다운로드','cacheReuse':'이미 받은 파일을 재사용합니다.',
  'originalPrepare':'ISO에서 원본 게임을 준비합니다.','languageInstall':'언어 패치를 적용합니다.','launch':'게임을 실행합니다.','graphicsPrepare':'이 PC의 그래픽 파이프라인을 준비합니다(처음 실행·업데이트 후 1회).','graphicsPrepareCancelled':'그래픽 준비를 중단했습니다. 완료된 부분은 보존되며 다음 실행 때 이어서 준비합니다.','gameExited':'게임이 종료되었습니다.',
  'failed':'작업에 실패했습니다. 아래 상세 기록과 로그를 확인하세요.','notice':'공지','patches':'패치 내역','all':'전체 버전',
  'open':'정상 운영','closed':'다운로드 닫힘','stale':'서버 상태 확인 필요','discord':'디스코드 방문 ↗','iso':'ISO 변경','folder':'설치 위치 ↗',
  'logs':'로그','self':'런처 업데이트 ↗','checked':'방금 확인함','steps':('1 기본 게임 패치','2 언어 패치','3 플레이'),
  'empty':'등록된 내용이 없습니다.','confirm':'최신 버전으로 업데이트할까요?','confirmBody':'다음 항목을 적용한 뒤 게임이 자동으로 실행됩니다.',
  'bytes':'다운로드','cancelled':'업데이트를 취소했습니다. 기존 설치본을 유지합니다.','closeBusy':'진행 중인 작업이나 게임이 종료된 뒤 닫을 수 있습니다.',
  'selfCurrent':'런처는 최신 상태입니다.','selfDev':'개발 실행에서는 자체 업데이트를 하지 않습니다.','errorTitle':'설치 실패','launcher':'런처 ver.'},
 'en': {
  'game':'Game version','latest':'Latest','unknown':'Checking','missing':'Not installed','ui':'UI language','check':'↻ Check updates',
  'locale':'Play language','pack':'Language pack','japanese':'Original Japanese · No extra pack','play':'Play','update':'Update','working':'Applying','running':'Game running',
  'ready':'Ready to play.','needsUpdate':'An update is required for the selected language.','coreUpdate':'Downloading game update',
  'languageUpdate':'Downloading selected language pack','cacheReuse':'Reusing verified download.','originalPrepare':'Preparing original game data from your ISO.',
  'languageInstall':'Applying language patch.','launch':'Starting game.','graphicsPrepare':'Preparing graphics pipelines for this PC (once after install or update).','graphicsPrepareCancelled':'Graphics preparation stopped. Finished work is kept and resumes on the next launch.','gameExited':'Game closed.','failed':'Operation failed. Review the details and log below.',
  'notice':'Notices','patches':'Patch notes','all':'All versions','open':'Service available','closed':'Downloads closed','stale':'Server status unverified',
  'discord':'Visit Discord ↗','iso':'Change ISO','folder':'Install folder ↗','logs':'Logs','self':'Launcher update ↗','checked':'Just checked',
  'steps':('1 Game update','2 Language patch','3 Play'),'empty':'No entries.','confirm':'Update to the latest version?',
  'confirmBody':'Apply the following updates, then start the game automatically.','bytes':'Download','cancelled':'Update declined. Existing installation unchanged.',
  'closeBusy':'Close the launcher after the current operation or game exits.','selfCurrent':'Launcher is current.','selfDev':'Self-update is disabled during development.',
  'errorTitle':'Installation failed','launcher':'Launcher ver.'},
 'ja': {
  'game':'ゲームバージョン','latest':'最新','unknown':'確認中','missing':'未導入','ui':'表示言語','check':'↻ 更新を確認',
  'locale':'プレイ言語','pack':'言語パック','japanese':'原本日本語・追加パック不要','play':'プレイ','update':'更新','working':'適用中','running':'ゲーム実行中',
  'ready':'プレイできます。','needsUpdate':'選択した言語でのプレイには更新が必要です。','coreUpdate':'ゲーム更新を取得中',
  'languageUpdate':'言語パックを取得中','cacheReuse':'確認済みの取得ファイルを再利用します。','originalPrepare':'原本ISOからゲームを準備しています。',
  'languageInstall':'言語パッチを適用中です。','launch':'ゲームを起動します。','graphicsPrepare':'このPC向けにグラフィックパイプラインを準備しています（初回・更新後に1回）。','graphicsPrepareCancelled':'グラフィック準備を中断しました。完了分は保持され、次回起動時に続きから準備します。','gameExited':'ゲームを終了しました。','failed':'処理に失敗しました。詳細とログをご確認ください。',
  'notice':'お知らせ','patches':'更新履歴','all':'全バージョン','open':'サービス稼働中','closed':'取得停止中','stale':'サーバー状態未確認',
  'discord':'Discordへ ↗','iso':'ISOを変更','folder':'導入先 ↗','logs':'ログ','self':'ランチャー更新 ↗','checked':'確認済み',
  'steps':('1 ゲーム更新','2 言語パッチ','3 プレイ'),'empty':'項目がありません。','confirm':'最新バージョンに更新しますか？',
  'confirmBody':'以下の更新を適用後、ゲームを自動起動します。','bytes':'取得','cancelled':'更新を取り消しました。既存のインストールを維持します。',
  'closeBusy':'処理またはゲームの終了後に閉じてください。','selfCurrent':'ランチャーは最新です。','selfDev':'開発中は自動更新しません。',
  'errorTitle':'導入失敗','launcher':'ランチャー ver.'}
}

TEXT["ko"].update(launcherChecking="새 런처를 확인합니다.", launcherDownload="새 런처를 자동으로 받습니다. 완료되면 다시 시작합니다.", launcherCheckFailed="런처 업데이트에 실패했습니다. 업데이트 확인을 눌러 다시 시도하세요. 게임·언어팩 작업은 시작하지 않았습니다.")
TEXT["en"].update(launcherChecking="Checking for launcher updates.", launcherDownload="Downloading launcher update. The launcher will restart automatically.", launcherCheckFailed="Launcher update failed. Press Update Check to retry. Game and language-pack operations have not started.")
TEXT["ja"].update(launcherChecking="ランチャー更新を確認中。", launcherDownload="新しいランチャーを取得中。完了後に自動再起動します。", launcherCheckFailed="ランチャー更新に失敗しました。更新確認から再試行してください。ゲーム・言語パックの処理は開始していません。")

TEXT['ko'].update({'iso': 'ISO 설정', 'isoHelp': '암호화된 ISO에는 해당 게임의 복호화 키 파일(.dkey 또는 .key)이 필요합니다. 직접 준비한 파일을 선택하세요.', 'isoNoFile': '선택된 ISO가 없습니다.', 'isoHashLabel': 'ISO 식별 해시 (SHA-1)', 'isoChoose': 'ISO 선택', 'isoKeyChoose': '키 파일 선택', 'isoCheck': '다시 검사', 'isoKeyClear': '키 선택 해제', 'isoKeyFile': '선택한 키 파일', 'isoReady': '복호화된 ISO 형식입니다. 지원 원본 여부는 설치 중 확인합니다.', 'isoEncrypted': '암호화된 ISO입니다.', 'isoKeyMissing': '이 ISO에 맞는 복호화 키가 필요합니다. ISO 설정에서 키 파일을 선택해 주세요.', 'isoKeyInvalid': '키 파일 형식이 잘못됐습니다. 32자리 16진수 텍스트 또는 16바이트 키 파일을 선택해 주세요.', 'isoKeyMismatch': '선택한 키가 이 ISO와 맞지 않습니다. ISO 식별 해시를 확인하고 해당 게임의 키 파일을 다시 선택해 주세요.', 'isoKeyMatched': '키 검사를 통과했습니다. 설치 시 원본을 보존하고 별도 ISO를 준비합니다.', 'isoUnsupported': 'ISO 형식을 확인할 수 없습니다. 지원 일본판 이미지와 파일 상태를 확인해 주세요.', 'isoHashing': 'ISO 상태와 식별 해시를 확인합니다.', 'isoDecrypting': '원본을 보존하며 ISO를 복호화합니다.', 'isoVerifying': '복호화 결과와 지원 원본을 확인합니다.', 'isoToolFailed': '원본 확인 도구 준비에 실패했습니다. 다시 시도해 주세요.', 'isoCacheInvalid': '준비된 ISO의 검증에 실패했습니다. 설치 폴더의 .local-build/disc-iso를 확인해 주세요.', 'isoPrepareFailed': 'ISO 준비에 실패해 설치를 중단했습니다. 원본 ISO는 덮어쓰지 않았습니다.'})

TEXT['en'].update({'iso': 'ISO settings', 'isoHelp': 'An encrypted ISO needs the matching disc-key file (.dkey or .key). Select a file you have prepared.', 'isoNoFile': 'No ISO selected.', 'isoHashLabel': 'ISO identity hash (SHA-1)', 'isoChoose': 'Choose ISO', 'isoKeyChoose': 'Choose key file', 'isoCheck': 'Check again', 'isoKeyClear': 'Clear key', 'isoKeyFile': 'Selected key file', 'isoReady': 'Decrypted ISO format detected. The supported original is verified during setup.', 'isoEncrypted': 'Encrypted ISO detected.', 'isoKeyMissing': 'This ISO needs a matching disc key. Open ISO settings and select its key file.', 'isoKeyInvalid': 'Invalid key format. Select 32 hex characters in a text file or a 16-byte binary key.', 'isoKeyMismatch': 'The selected key does not match this ISO. Check the ISO identity hash and choose the matching game key.', 'isoKeyMatched': 'Key check passed. Setup will preserve the original and prepare a separate ISO.', 'isoUnsupported': 'ISO format could not be verified. Check the supported Japanese image and file integrity.', 'isoHashing': 'Checking ISO status and identity hash.', 'isoDecrypting': 'Decrypting a separate ISO; preserving the original.', 'isoVerifying': 'Verifying the prepared ISO and supported original.', 'isoToolFailed': 'Original-verification tool setup failed. Please retry.', 'isoCacheInvalid': 'Prepared ISO verification failed. Check .local-build/disc-iso in the install folder.', 'isoPrepareFailed': 'ISO preparation failed and setup stopped. The original ISO was not overwritten.'})

TEXT['ja'].update({'iso': 'ISO設定', 'isoHelp': '暗号化ISOには対応するキー(.dkey/.key)が必要です。ご自身で用意したファイルを選択してください。', 'isoNoFile': 'ISOが未選択です。', 'isoHashLabel': 'ISO識別ハッシュ (SHA-1)', 'isoChoose': 'ISOを選択', 'isoKeyChoose': 'キーを選択', 'isoCheck': '再確認', 'isoKeyClear': 'キーを解除', 'isoKeyFile': '選択したキー', 'isoReady': '復号済みISO形式です。対応する原本かどうかは導入時に検証します。', 'isoEncrypted': '暗号化ISOです。', 'isoKeyMissing': '対応するディスクキーが必要です。ISO設定でキーファイルを選択してください。', 'isoKeyInvalid': 'キー形式が無効です。32桁の16進数テキストまたは16バイトのキーを選択してください。', 'isoKeyMismatch': 'このキーはISOと一致しません。ISO識別ハッシュを確認し、対応するキーを選択してください。', 'isoKeyMatched': 'キー検証に成功。原本を保持して別のISOを準備します。', 'isoUnsupported': 'ISO形式を確認できません。対応日本版とファイルの状態を確認してください。', 'isoHashing': 'ISO状態と識別ハッシュを確認中。', 'isoDecrypting': '原本を保持し別のISOを復号中。', 'isoVerifying': '復号結果と対応原本を検証中。', 'isoToolFailed': '原本確認ツールの準備に失敗。再試行してください。', 'isoCacheInvalid': '準備済みISOの検証に失敗。導入先の.local-build/disc-isoを確認してください。', 'isoPrepareFailed': 'ISO準備に失敗したため導入を停止。原本ISOは上書きしていません。'})

# Launch options deliberately omit language, FPS unlock, and unverified HD support.
TEXT['ko'].update({
 'launchOptions':'실행 옵션', 'optionScale':'렌더 해상도 배율', 'optionFullscreen':'전체화면',
 'optionVsync':'VSync (수직 동기화)', 'optionBloom':'Bloom (빛 번짐)', 'optionSubtitles':'자막',
 'optionsRestart':'게임을 종료한 뒤 저장하세요. 저장한 옵션은 다음 게임 실행에 적용됩니다. 실행 중인 게임에는 실시간 적용되지 않습니다.',
 'optionsFpsUnavailable':'FPS 해제 — 수정 중 (변경 불가)',
 'optionsHdUnavailable':'HD 텍스처: 게임 버전 0.3.4는 미지원입니다. 다른 게임 버전도 지원 확인 전에는 변경할 수 없습니다.',
 'optionsReadFailed':'설정을 열 수 없습니다. 설치 위치의 play 폴더를 확인하세요.',
 'optionsSaveFailed':'저장하지 못했습니다. 설정 파일은 유지됩니다.', 'optionsSaved':'저장했습니다. 다음 게임 실행에 적용됩니다.',
 'optionsWait':'진행 중인 작업이 끝난 후 저장하세요.', 'optionsSave':'저장', 'optionsCancel':'취소'})
TEXT['en'].update({
 'launchOptions':'Launch options', 'optionScale':'Render resolution scale', 'optionFullscreen':'Fullscreen',
 'optionVsync':'VSync', 'optionBloom':'Bloom', 'optionSubtitles':'Subtitles',
 'optionsRestart':'Save with the game closed. Saved options apply on the next game launch; they do not update a running game.',
 'optionsFpsUnavailable':'FPS unlock — under repair (unavailable)',
 'optionsHdUnavailable':'HD textures: unsupported by game version 0.3.4. Unavailable for other game versions until support is verified.',
 'optionsReadFailed':'Cannot open settings. Check the play directory in the installation folder.',
 'optionsSaveFailed':'Could not save. The settings file was preserved.', 'optionsSaved':'Saved. Options apply on the next game launch.',
 'optionsWait':'Wait for the current operation before saving.', 'optionsSave':'Save', 'optionsCancel':'Cancel'})
TEXT['ja'].update({
 'launchOptions':'起動設定', 'optionScale':'描画解像度の倍率', 'optionFullscreen':'全画面',
 'optionVsync':'VSync（垂直同期）', 'optionBloom':'Bloom（光のにじみ）', 'optionSubtitles':'字幕',
 'optionsRestart':'ゲームを終了してから保存してください。保存した設定は次回起動時に適用されます。実行中のゲームには即時反映されません。',
 'optionsFpsUnavailable':'FPS制限解除 — 修正中（変更不可）',
 'optionsHdUnavailable':'HDテクスチャ：ゲームバージョン0.3.4は非対応です。他のゲームバージョンも対応確認までは変更できません。',
 'optionsReadFailed':'設定を開けません。導入先のplayフォルダーを確認してください。',
 'optionsSaveFailed':'保存できませんでした。設定ファイルは保持されます。', 'optionsSaved':'保存しました。次回のゲーム起動時に適用されます。',
 'optionsWait':'処理が終わってから保存してください。', 'optionsSave':'保存', 'optionsCancel':'キャンセル'})
TEXT['ko']['optionsScaleHelp'] = '배율별 해상도: 1 = 720p · 1.5 = 1080p · 2 = 1440p · 3 = 4K'
TEXT['en']['optionsScaleHelp'] = 'Resolution by scale: 1 = 720p · 1.5 = 1080p · 2 = 1440p · 3 = 4K'
TEXT['ja']['optionsScaleHelp'] = '倍率と解像度：1 = 720p・1.5 = 1080p・2 = 1440p・3 = 4K'

TEXT['ko'].update(optionHd='HD 텍스처 (다음 실행부터 적용)', hdPrepare='원본 데이터로 HD 텍스처를 준비합니다.')
TEXT['en'].update(optionHd='HD textures (next launch)', hdPrepare='Preparing HD textures from your original game data.')
TEXT['ja'].update(optionHd='HDテクスチャ（次回起動から）', hdPrepare='原本データからHDテクスチャを準備しています。')

TEXT['ko'].update(
    optionScale='렌더 해상도', runtimeOptions='게임 옵션 열기',
    runtimeOptionsOpened='게임 옵션 창에서 조절하세요. 해상도·HD 변경은 재실행 후 적용됩니다.',
    runtimeOptionsNoGame='선택한 설치 폴더의 게임이 실행 중이 아닙니다.',
    runtimeOptionsUnavailable='게임 옵션 창을 열지 못했습니다. 게임이 응답하는지 확인한 뒤 다시 눌러 주세요. 게임 안에서 F9로 열 수도 있습니다.',
    optionsRestart='저장한 옵션은 다음 게임 실행부터 적용됩니다. 게임 실행 중에는 런처의 게임 옵션 열기를 사용하세요.')
TEXT['en'].update(
    optionScale='Render resolution', runtimeOptions='Open game options',
    runtimeOptionsOpened='Adjust settings in the game options window. Resolution and HD changes require a restart.',
    runtimeOptionsNoGame='The game in the selected installation folder is not running.',
    runtimeOptionsUnavailable='Could not open game options. Check that the game is responding and try again, or press F9 in the game.',
    optionsRestart='Saved settings apply on the next game launch. While playing, use Open game options in the launcher.')
TEXT['ja'].update(
    optionScale='描画解像度', runtimeOptions='ゲーム設定を開く',
    runtimeOptionsOpened='ゲーム設定画面で調整してください。解像度とHDの変更は再起動後に反映されます。',
    runtimeOptionsNoGame='選択した導入フォルダーのゲームが起動していません。',
    runtimeOptionsUnavailable='ゲーム設定を開けませんでした。ゲームの応答を確認して再試行するか、ゲーム内でF9を押してください。',
    optionsRestart='保存した設定は次回起動から反映されます。プレイ中はランチャーの「ゲーム設定を開く」を使ってください。')

# HTTPS certificate-chain failures remain blocking; offer a clear recovery path.
TEXT['ko']['tlsCertificateFailed'] = '업데이트 서버의 보안 인증서를 확인하지 못했습니다. PC 날짜·시간과 Windows 업데이트를 확인해 주세요. 브라우저에서도 GitHub 다운로드가 되는지 확인한 뒤, 계속 실패하면 런처 버전과 이 메시지를 알려 주세요.'
TEXT['en']['tlsCertificateFailed'] = 'Could not verify the update server certificate. Check your PC date/time and Windows updates, then try the GitHub download in your browser. If it still fails, report your launcher version and this message.'
TEXT['ja']['tlsCertificateFailed'] = '更新サーバーの証明書を確認できませんでした。PCの日時とWindows Updateを確認し、ブラウザーでGitHubからダウンロードできるかお試しください。解決しない場合はランチャーのバージョンとこのメッセージをお知らせください。'

TEXT['ko'].update(packImport='언어팩 가져오기…', packPartial='미완성 테스트 팩 허용 · 대체 언어 사용', packEnglishBase='영어 이미지 기반 데이터')
TEXT['en'].update(packImport='Import language pack…', packPartial='Allow unfinished test pack with fallback', packEnglishBase='English image base')
TEXT['ja'].update(packImport='言語パックを読み込む…', packPartial='未完成のテストパックと代替言語を許可', packEnglishBase='英語画像の基盤データ')
