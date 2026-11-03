# 배포 전용 도구

`tools/release`는 원본 ISO 진단, 선택 빌드, 네 구성요소 검사, 배포 준비와 GitHub 공개를 한곳에서 실행하는 도구입니다. 기존 `tools/dist`, `tools/local_build`, `tools/launcher-python`의 구현을 재사용합니다. 게임과 언어팩은 원본 게임 데이터 없이 패키징하고, 플레이어 PC에서는 원본 데이터를 로컬로 준비합니다.

## 현재 버전과 실행 예

공개 구성은 게임 **0.3.4**, 한국어 **0.3.4**, 영어 **0.3.4**, 런처 **0.3.6**입니다. 모든 구성요소가 같은 버전일 필요는 없습니다. 이번에 변경하는 구성요소만 새 릴리스 번호를 쓰며 나머지는 기존 버전과 해시를 유지합니다. 현재 런처의 다운로드 규칙상 새 파일의 버전은 업로드할 GitHub 태그와 일치해야 합니다.

다음 예제는 완료된 0.3.4 → 런처 0.3.6 배포를 로컬에서 재구성합니다. 기존 공개 태그가 있으므로 다시 공개할 수 없습니다. 다음 배포에는 새 설정 파일과 새 출력 폴더를 사용하세요.

```powershell
$python = '.tools/launcher-python-venv/Scripts/python.exe'
& $python tools/release/pipeline.py plan --config tools/release/examples/launcher-only.json
& $python tools/release/pipeline.py prepare --config tools/release/examples/launcher-only.json --output C:/GameDev/Macross-release-builds/rehearsal-036
& $python tools/release/pipeline.py verify C:/GameDev/Macross-release-builds/rehearsal-036
```

`prepare`는 게임·한국어·영어·런처 **네 항목을 모두 확인**합니다. 기존 세 팩도 파일 크기와 SHA-256을 대조하고 기존 배포 검사기로 ZIP 내부를 확인합니다. 런처 ZIP과 단독 EXE가 같은 파일인지 확인합니다. 임시 검사 사본은 자동 정리합니다. 업로드 폴더에는 변경된 팩만 넣으며 원본 ISO/ELF, 설치 데이터나 세이브를 넣지 않습니다. 변경하지 않은 메타데이터는 바이트 그대로 유지합니다.

`baseline_assets`와 `input_assets`는 단일 폴더 또는 구성요소별 폴더 매핑을 받습니다. 서로 다른 버전의 팩을 한 폴더로 복사할 필요가 없습니다. 다음 빌드용 설정 틀은 `examples/next-launcher.template.json`입니다. 실제 버전, 릴리스 노트, 빌드 입력을 준비해 별도 설정으로 저장한 뒤 사용하세요.

## 빌드 연결

설정의 `build_steps`는 구성요소별 **인자 배열 목록**입니다. 셸 문자열을 조합하지 않습니다.

```powershell
& $python tools/release/pipeline.py build --config path/to/next-release.json
```

- `launcher`: `tools/launcher-python/build.ps1`에 `Output`, `ManifestPath`, `StatusPath`를 명시합니다. `version.txt`와 `backend.VERSION`을 새 버전으로 맞춘 뒤 빌드하세요. 빌드 출력 경로와 설정의 `input_assets` 경로도 맞춰야 합니다.
- `game`: 검증할 네이티브 빌드를 명시적으로 선택해 기존 빌드 도구로 만든 후, `tools/dist/prebuilt_package.py`의 `--program-dir`, `--runtime-dir`, `--data-archive`, `--output`, `--version`으로 패키징합니다. 자동으로 활성 실험 후보를 선택하지 않습니다.
- `korean`, `english`: 기존 번역 생성 도구와 `tools/dist/split_language_packages.py`를 연결합니다. 원본 의존 입력은 로컬에서 준비하며 기존 언어팩을 재사용할 수도 있습니다.

빌드 명령이 실패하면 이후 단계로 진행하지 않습니다. 이미 빌드한 입력은 `build` 없이 `prepare`할 수 있습니다. 패키지 검사는 게임 플레이 검증을 대신하지 않습니다. 네이티브 코드나 런처 동작 변경에 필요한 실제 실행 검증은 공개 전에 별도로 완료해야 합니다.

## 공개

새 버전의 스테이지만 공개할 수 있습니다. `--checkout`은 깨끗한 **전용 공개 메타데이터 저장소 체크아웃**을 지정합니다. 현재 개발 저장소의 HEAD나 인덱스를 바꾸지 않습니다.

```powershell
& $python tools/release/pipeline.py publish C:/GameDev/Macross-release-builds/next-release --checkout C:/GameDev/Macross-public-metadata
& $python tools/release/pipeline.py verify-public C:/GameDev/Macross-release-builds/next-release
```

순서: 로컬 해시 재확인 → 공개 stable 기준과 설정 일치 확인 → 별도 릴리스 브랜치/태그 → 초안 업로드 → 서버 자산 해시 확인 → 공개 → stable 갱신 → 로그인 없는 실제 다운로드 해시 확인. 공개 태그 덮어쓰기와 force push를 하지 않습니다. 변경하지 않은 팩은 새 릴리스에 업로드하지 않습니다.

`publication.json`에 완료 단계를 남깁니다. 중간 실패 시 무작정 다시 공개하지 않습니다. 이미 공개된 경우 `verify-public`으로 네트워크 검증을 재시도할 수 있습니다. 태그나 초안이 생성된 상태의 복구는 기록된 commit/tag/release_id와 실제 GitHub 상태를 대조한 뒤 처리하세요. 원격 raw 메타데이터의 캐시 전파는 수분 지연될 수 있습니다.

## ISO 진단

```powershell
& $python tools/release/iso.py 'C:/path/to/game.iso' --report C:/GameDev/Macross-release-builds/iso-report.json
& $python tools/release/iso.py 'C:/path/to/game.iso' --hashes
```

EBOOT의 크기와 시작 헤더를 읽고 `ELF`, `SCE SELF`, 미인식 상태를 구분합니다. 헤더가 맞아도 지원하는 게임의 최종 ELF 해시가 확인된 것은 아닙니다. 미인식 헤더만으로 암호화 또는 손상을 확정하지 않습니다. `--hashes`를 지정할 때만 ISO 전체를 읽습니다.

## 디스크 ISO 복호화

원본 ISO를 보존하고 별도 ISO를 생성하는 도구입니다. **공개 런처 0.3.6에는 아직 통합하지 않았습니다.** 복호화된 결과 ISO는 기존 설치기에서 선택할 수 있습니다.

```powershell
& $python tools/release/build_iso_tool.py
& $python tools/release/disc_key.py --sevenzip D:/Game/mAC30/.local-build/tools/7zr.exe
& $python tools/release/decrypt_iso.py 'C:/path/game.iso' --key-file '.tools/iso-private/Macross 30 - Ginga o Tsunagu Utagoe (Japan).dkey' --probe
& $python tools/release/decrypt_iso.py 'C:/path/game.iso' --output 'C:/path/game.decrypted.iso' --key-file '.tools/iso-private/Macross 30 - Ginga o Tsunagu Utagoe (Japan).dkey' --rpcs3 D:/Game/mAC30/.local-build/tools/rpcs3/rpcs3.exe --report C:/path/decryption-report.json
```

- 유지보수용 빌드는 고정된 PS3Dec/mbedTLS 커밋을 사용합니다. 기존 MSVC/CMake가 필요합니다. 최종 실행 파일과 소스 패치의 SHA-256을 `tool-manifest.json`에 기록합니다.
- 키 조회는 고정 해시의 공개 aldostools 목록에서 지원 일본판 항목만 추출합니다. 별도 키 파일을 지정할 수도 있습니다. 키나 카탈로그를 배포 패키지에 넣지 않습니다.
- `.dkey`는 32자리 hex 텍스트, 바이너리 키는 16바이트를 받습니다. 키 값은 프로세스 인자나 출력에 노출하지 않습니다.
- 전체 변환 전에 EBOOT 첫 섹터를 검사합니다. 잘못된 키는 큰 출력 파일을 만들기 전에 거부합니다.
- 출력 크기와 원본 불변성을 확인하고, 기존 RPCS3 원본 준비 기능으로 최종 ELF SHA-256 `44d7112c...782b5b`까지 일치해야 결과 ISO를 확정합니다. 기존 출력 경로는 덮어쓰지 않습니다.
- 완성된 키를 D1으로 재변환하지 않는지 실제 네이티브 도구의 암호화·복호화 왕복 및 독립 섹터 probe를 검사합니다.

2026-11-01 실제 16,317,612,032바이트 입력 검증: `58a15ab9` → `SCE` → 지원 ELF 해시 일치. 입력 SHA-256 `c6f45958...198c45` 보존, 출력 SHA-256 `58f586f9...6e36e8`. 증적은 `native/analysis/iso-compatibility-20261101/decryption-verification.json`에 있습니다. 게임 실행 검증을 뜻하지 않습니다.

## 직접 다운로드

```powershell
& $python tools/release/download.py --item ITEM_ID --file 'exact-name.iso' --output-dir C:/GameDev/Macross-downloads/ITEM_ID
```

명시한 항목의 ISO 한 개를 HTTPS로 받습니다. 토렌트는 받지 않습니다. 범위별 이어받기와 동시 실행 잠금, 서버 범위 응답 확인, 완료된 구간 재검사, 최종 SHA-1/MD5 대조를 수행합니다. 검증 후에만 `.part`를 `.iso`로 바꿉니다. ISO 선택과 다운로드는 사용자가 요청한 파일에 한해 실행하세요. 현재 진행 중인 최초 다운로드는 기존 실행을 유지하며, 이 도구는 후속 다운로드부터 사용할 수 있습니다.

## 검사

```powershell
& $python -m unittest discover -s tools/release -p 'test_*.py'
```

런처 단독/전체 구성 배포, 재사용 팩 손상, 버전 역행, 잘못된 업로드 태그, 원본 실행 파일 혼입, 런처 ZIP 불일치, 임의 파일 추가, 기존 태그, 초안 서버 해시 실패와 ISO 헤더 진단을 검사합니다.
