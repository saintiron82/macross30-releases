# Macross 30 — Downloads

**0.3.8: HD 텍스처·실행 전 셰이더 준비·메뉴/HUD 번역 수정. 게임·한글팩·영문팩·런처 모두 0.3.8입니다.**

**720p~4K 지원. 30 FPS 제한을 유지하며 제한 해제는 수정 중으로 사용할 수 없습니다.**

**[런처 다운로드 / Download launcher / ランチャー](https://github.com/saintiron82/macross30-releases/releases/download/v0.3.8/Macross30-launcher-0.3.8.zip)**

게임 / Game **0.3.8** · 런처 / Launcher **0.3.8**

**[English installation and usage guide](README.en.md)** · [Release notes](https://github.com/saintiron82/macross30-releases/releases/tag/v0.3.8) · [Discord](https://discord.gg/WqaaJ6hTmY)

ZIP 압축 해제 → `Macross30Launcher.exe` 실행 → 플레이 언어 선택 → **플레이/업데이트** → 첫 설치 시 본인 정품에서 직접 덤프한 지원 일본판 ISO 선택.

암호화된 ISO는 해당 이미지에 맞는 복호화 키가 필요합니다. 런처의 **ISO 설정**에서 직접 준비한 `.dkey` 또는 `.key` 파일을 선택하세요.

한국어·영어팩을 제공하며 일본어는 원본 데이터를 사용합니다. 모든 언어에서 실행·렌더링·진행 문제가 있을 수 있으며 제보를 받습니다. 런처가 사전 빌드 Windows 실행 파일과 선택한 언어팩을 설치합니다. 원본 EBOOT와 게임 데이터는 사용자 ISO에서 로컬로 준비합니다.

Windows 10/11 x64, D3D12 GPU, AVX2/SSE4.1 CPU가 필요합니다. 테스트 배포이며 원작과 다른 그래픽, 이벤트 미발생·오작동, 미션 진행 정지와 실행·조작·저장·오디오·번역 문제를 제보받습니다.

**본인 소유 정품에서 직접 덤프한 ISO만 사용하세요.** 비공식·비상업 팬 프로젝트이며 관련 권리자의 승인이나 제휴를 받은 프로젝트가 아닙니다. 원본 소유가 모든 복제·변경·배포를 자동 허용하는 것은 아닙니다. 관련 법과 이용 조건을 준수하세요. **권리자 요청이나 프로젝트 사정에 따라 사전 예고 없이 언제든 배포가 중단될 수 있습니다.**

## Source and attribution

The v0.3.8 tag preserves updated renderer/options, launcher, installer and packaging component sources with hashes in release-baselines/0.3.8. Precompiled game assets retain previously verified link inputs; this is not a claim of a fully self-hosted game build from this snapshot. Based on ps3recomp; its MIT license is retained at `vendor/ps3recomp-master/LICENSE`. Bundled third-party notices also apply. The English guide describes the current installation flow.


## 기존 런처 0.3.0 사용자

1. 런처 하단 **런처 업데이트**를 누르고 교체를 승인하세요.
2. 다시 열린 런처에서 기존 플레이 언어를 선택하고 **업데이트/플레이**를 눌러 게임 수정 파일도 적용하세요.
3. 자동 업데이트에 실패하면 최신 런처 ZIP을 받아 런처를 종료한 뒤 기존 런처 파일을 교체하세요.

기존 게임 설치 폴더와 저장 파일은 삭제하지 마세요. 원본 데이터 복구가 필요하면 기존에 사용하던 본인 소유 ISO를 지정하세요.


런처 0.3.2부터 실행 시 새 런처를 자동으로 다운로드·교체·재시작합니다. 게임 실행 중에는 런처를 유지하고 버튼에 실행 중으로 표시합니다. 기존 0.3.0/0.3.1은 런처 업데이트를 한 번 눌러 최신 버전으로 교체하세요. 게임·언어팩 버전은 0.3.8입니다.


게임 종료 후 런처의 **로그 → 마지막 플레이 로그 보내기**에서 내용을 확인하고 전송할 수 있습니다. 로그인 없이 운영진 전용 Discord 채널로 보내며 세이브는 선택 첨부입니다. ZIP은 로컬 bug-reports에 남습니다.

## ISO가 암호화되어 있다고 표시되는 경우

런처 0.3.7의 **ISO 설정**에서 ISO 식별 해시(SHA-1)를 확인하고 해당 이미지에 맞게 준비한 `.dkey` 또는 `.key` 파일을 선택하세요. 키 없음·파일 형식 오류·키 불일치를 구분해서 안내합니다. 키가 맞으면 원본을 보존하며 별도 ISO를 준비하므로 ISO 크기만큼의 추가 공간이 필요합니다. 이미 복호화된 ISO는 별도 디스크 키가 필요하지 않습니다.
