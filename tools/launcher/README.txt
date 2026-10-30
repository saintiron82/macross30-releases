Macross 30 Launcher 0.1.5 — 로컬 생성 방식 / Local build / ローカルビルド

사용자 ISO로 실행 파일을 로컬 생성하는 설치 방식입니다. 실제 플레이 성능과 새 Windows의 첫 도구 설치는 추가 검증이 필요합니다.
런처 언어와 게임 언어 선택 → 최신 패치 다운로드 → 본인 소유 ISO 선택 → 자동 생성·컴파일·설치 → 게임 실행.
JSON이나 개별 패치를 고르거나 ELF를 따로 구할 필요가 없습니다.
완성된 게임 EXE/ELF는 다운로드하지 않습니다. 사용자 ISO에서 로컬로 만듭니다.
한국어·원본 일본어 지원. 영어 게임팩은 준비 중입니다. 런처 화면은 세 언어를 지원합니다.
첫 설치는 Python, Microsoft C++ Build Tools, SDL2, 원본 준비용 RPCS3 등 필요한 도구를 공식 배포처에서 받습니다.
Windows가 빌드 도구 설치를 위한 관리자 권한을 요청할 수 있습니다. 첫 빌드는 시간이 걸리고 디스크 여유 공간이 필요합니다.
기존 세이브와 설정은 유지합니다. 게임 실행 중 설치는 차단합니다.
실패하면 고급 메뉴 → 로그 폴더 → local-build.log를 확인하세요.
공개 로컬 빌드 패키지가 없으면 준비 중으로 표시합니다. 구형 게임 실행 파일을 대신 받지 않습니다.

Local generation from your own ISO. Gameplay performance and first-time tool setup on a clean Windows installation need further validation.
Choose UI/game language → Download latest patch → select your own ISO → automatic local generation/build/install → Play.
No JSON, individual patches or separate ELF selection. Game EXE/ELF files are generated on your PC.
The first install prepares official build tools; Windows may request administrator permission. Allow time and disk space.
Korean and original Japanese are supported. English game data is not ready. UI supports all three languages.
Advanced provides local package installation, logs and launcher self-update. Build failures are logged in play/local-build.log.

ご自身のISOからローカル生成する方式です。実プレイ性能と新規Windows環境での初回ツール導入は追加検証が必要です。
表示言語・ゲーム言語を選択 → 最新パッチを取得 → ご自身のISOを選択 → 自動生成・ビルド・インストール → ゲーム起動。
JSONや個別パッチ、別のELFを選ぶ必要はありません。ゲームEXE/ELFはこのPCで生成します。
初回は公式配布元のビルドツールを準備します。管理者権限の確認、時間、空き容量が必要な場合があります。
韓国語と原本の日本語に対応し、英語ゲームデータは準備中です。表示は3言語対応です。

Windows 10/11 x64 · .NET Framework 4.5+

Public downloads: https://github.com/saintiron82/macross30-releases/releases/latest
