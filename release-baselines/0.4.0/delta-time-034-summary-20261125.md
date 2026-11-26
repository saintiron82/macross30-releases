# delta-time-034 브랜치 총정리 · 2026-10-26 – 11-26

`saintiron82/delta-time-034`는 `main`(`d3159a5`)에서 갈라진 뒤 `main`에 합치지 않은 작업 브랜치다. 기간은 10-26부터 11-26까지, 커밋은 270여 개다. 정리 시점의 표시 브랜치는 `saintiron82/delta-time-034-final`이다. push, rebase와 squash는 하지 않았고 커밋 이력은 그대로다. 세부 수치와 근거는 각 절의 링크 문서에 있다.

## 최종 상태

- **최종 빌드** — `build/tests/Macross-real60/m29-lto/Macross30Recompiled.exe`(SHA-256 `f2776236ec0606e457495445e5fa1c2f245ea087e1c39565ea7a956354545332`). 11-26 사용자 확인("이대로 간다"). m25-lto에 진동 기능만 더한 빌드라 렌더·런타임 코드는 m25와 같다. 다음이 모두 들어 있다.
  - lifted PPU 96개 전체 speed flags + ThinLTO
  - 델타 타임 변환 전체
  - F9 `30 FPS 제한 해제` 실시간 전환
  - R8–R10 렌더·런타임 개선
  - 1P 진동(피격·다운, F9 세기 슬라이더, 지면·벽·수면 충돌)
- **프레임** — F9 체크 해제 시 30 FPS 고정(원본 속도), 체크 시 상한 없음. 게임 시간은 실측 dt로 진행한다.
- **실측** — 1.5× 내부 해상도 필드 67–86 FPS(m22-lto). 4K(3/1) 필드는 m25-lto에서 5초 평균 최저 **49.5**, 중앙값 **69.5 FPS**.
- **4K 기준** — 사용자가 정한 4K 최저 60 기준은 미달인 채 종료했다. 고부하 구간은 렌더 스레드 CPU 한계(15–18 ms/frame)다. [성능 R8–R10](performance-r8-r10-20261125.md)
- **PGO** — 학습은 하지 않았다. 같은 소스의 trainer `m25-instr`가 남아 있다.

## 주제별 작업

### 1. SPU 제거·안정성 (10-26 – 11-01)
- **크래시·멈춤 수정** — 동굴 스테이지 지오메트리 깨짐·정지(SPU ring 순서), 세이브 메뉴 멈춤, BGM ABI 수정.
- **SPU 작업의 네이티브 대체** — Edge Animation(10006/10002), notify(10001), ribbon(10009), 스키닝(geom2 scheme 2·3, 3-fragment, 7-image 검증), 후처리 taskset. 모두 섀도 비교 후 신뢰하는 방식으로 넣었다.
- **보병 다수 장면 감속** — geom2 local dispatch로 해결했고 사용자가 확인했다.
- **원본 SPU guard** — 바이트 배열을 SHA-256 서명으로 교체했다.
- [fix-log](wiki/fix-log.md) · [SPU 인수인계](wiki/handoff-spu-removal.md) · [SPU 인벤토리](wiki/spu-inventory.md)

### 2. 현지화·배포·런처 (10-26 – 10-31)
- **현지화** — 윙맨 명령 다이얼, 옵션 화면 타이틀, 한국어 영상·전투 음성 자막, 영어 팩, 세이브 대화상자.
- **패치·로컬 빌드** — 패치 0.1.0부터 버전을 매겼다. 사용자 ISO로 로컬 빌드하는 0.2.0이 있다.
- **런처** — 0.1.x Electron에서 0.3.2 Python 네이티브 런처까지 진행했다. 자동 업데이트, 언어 팩 선택, 패치 승인 기능이 있다.
- **릴리스 기준선** — 0.3.4(30 FPS 제한, 1800p/4K)를 보존했다.
- [런처](launcher.md) · [언어 팩](language-packs.md) · [0.3.4 후보](release-0.3.4-candidate.md)

### 3. 델타 타임 (11-01 – 11-25)
- **조사 단계** — 가변 델타 v13–v22와 고정 60 Hz 시도 r26–r29를 했다. 고정 60의 정지 원인은 motion flag(`4fe5a82`)였고 수정했다. [고정 60 결과](fixed60-result-20261114.md) · [물리 델타](physics-delta-20261101.md)
- **동작 핸들러** — 30 Hz 틱에서 프레임 dt로 바꿨다. 대상은 다음과 같다.
  - 파이터·배트로이드 부스트
  - 공격 대시와 롤아웃
  - command-32 대시
  - mover 회전과 감쇠
  - 스틱 홀드
  - 추진 화염
  - [설계](action-handler-dt-design-20261121.md) · [파일럿 결과](action-handler-dt-pilot-result-20261121.md)
- **전 영역 경과 시간화** — 월드 이펙트, 애니메이션, 게임플레이·AI, 카메라·연출, 액터 물리, 오디오·컨트롤러.
  - 이후 카메라 궤도, 필드 감쇠·조향, 오버레이 페이드, 버튼 홀드, Lua 대기까지 바꿨다.
  - 기준은 단순 dt 식(`a·k`, `d^k`)과 항목별 가벼운 fixture다.
- **페이싱** — vblank 계수 스텝과 even pace를 시도했지만 측정상 이득이 없어 되돌렸다. 실측 dt를 유지한다. [프레임 페이싱 결과](frame-pacing-result-20261121.md)

### 4. 프레임 상한 해제 (11-24)
- **F9 전환** — `30 FPS 제한 해제` 체크가 실시간으로 바뀐다. 해제 쪽에서는 다음이 바뀐다.
  - 게스트 vblank 대기 0
  - 작업 기반 flip(FIFO·GPU 완료가 다음 프레임을 깨움)
  - Present interval 0
- **기준점** — `m22-toggle` 이후의 모든 후보가 이 구조를 쓴다. [W12 페이싱](../build/scratch/r8/w12/REPORT.md)

### 5. 성능 (11-23 – 11-25)
- **R1–R7(m15–m21)**
  - Edge Animation 네이티브 경로
  - lifted PPU dispatch·바이트 permute 축소
  - 렌더러 트레이싱·상수 클리어 제거
  - SPURS 캐시
  - ThinLTO·PGO 파이프라인
- **R8–R10(m22–m25)**
  - scalar 메모리 한계 사전 계산
  - Edge clip SSE2
  - 잡 제출 축소
  - descriptor·PSO·vertex·바인딩 재사용
  - GCM 빈 pump 단축
  - RAWN 업로드 풀
- **미채택** — threaded recorder, animation worker 기본 사용, ISA·링크 순서, R9 PPU 동기화 일부(되돌림)는 넣지 않았다.
- [성능 R8–R10](performance-r8-r10-20261125.md) · [성능 위키](wiki/performance.md)

### 6. 진동 (11-25 – 11-26)
- **원작에는 진동이 없다** — 게임이 패드에 보내는 진동 요청은 항상 0이고, 진동 값을 채우는 게임 코드도 진동 옵션도 없다. 피격 쪽에 플레이어 전용으로 부르는 빈 함수 두 개가 남아 있다. 그래서 모든 진동은 게스트 훅으로 새로 넣었다. [W13 조사](../build/scratch/r8/w13/RUMBLE.md)
- **피격·다운·격추** — 1P 피격 생존 시 약 펄스, 다운·격추 시 강 펄스. 호스트 시계로 만료해 프레임과 무관하다. [W13 진동](../build/scratch/r8/w13/RUMBLE-BUILD.md)
- **세기 슬라이더** — F9 `진동` 아래 `진동 세기` 0–200%(10 단위, `settings.ini rumble_strength`). 세기를 올리다 모터가 포화하면 길이가 늘어난다. 11-26에 예전 200%를 새 100%(기본값)로 옮겼다. 새 100%는 약 펄스 저주파 70%·고주파 30%·225 ms, 강 펄스 100%·100%·750 ms다. [세기](../build/scratch/r8/w13/RUMBLE-STRENGTH.md)
- **충돌** — 1P 기체가 지면, 벽·건물, 수면(아래로 들어갈 때)에 부딪힌 순간 접근 속도 `c`로 세기를 정한다. `c < 1.5`는 무시하고, `c = 1.5–8` 구간에서 약 펄스부터 강 펄스까지 보간한다. 300 ms 안의 연속 튕김은 더 센 충돌만 갱신한다. 상수는 `native/host/gameplay_rumble.h` 한 곳에 있다. [충돌 진동](../build/scratch/r8/w13/IMPACT-RUMBLE.md) · [접촉 조사](../build/scratch/r8/w13/SURFACE-CONTACT.md)
- **충돌 기록** — 기본 꺼짐. `MACROSS_ACTOR_FIELD_TRACE` + `MACROSS_ACTOR_SURFACE_TRACE=1`로 켜고 `tools/diag/analyze_surface_trace.py <run>`으로 분석한다. [기록](../build/scratch/r8/w13/SURFACE-TRACE.md)

## 빌드와 실행

```powershell
$env:PYTHONUTF8 = '1'; $env:PYTHONIOENCODING = 'utf-8'
python -B tools/build/build_real60_candidate.py <이름> --mode thinlto --jobs 3 --gameplay-rumble --render-capture
# m29-lto는 m28 객체를 재사용해 만들었다(정확한 스위치와 해시: build/scratch/r8/w15/REPORT-4.md#m29-lto)
python -B tools/build/build_m29_from_m28.py m29-lto --mode thinlto --lifted-scope all --jobs 3 --field-probes --anim-workers --render-capture --gameplay-rumble
python -B tools/diag/run_real60.py --build <이름> --label field --step measured --seconds 60 --render-scale 3
```

- 실행 폴더는 `build/tests/Macross-real60/<빌드>/run-*`이고 세이브는 격리 복사본이다.
- `stderr.log`에서 다음을 확인한다.
  - `[render-scale]`
  - `[runtime-options] fps_unlock=… effective_vblanks=…`
  - 초당 `[macross-frame-time]`
- 스위치와 진단 옵션 표: [성능 R8–R10 · 빌드와 실행](performance-r8-r10-20261125.md#빌드와-실행)

## 남은 일

- **선택 사항** — `m25-instr`로 PGO 학습 1회, 최종 PGO 빌드 비교.
- **4K 고부하 구간** — 렌더 스레드 CPU와 FIFO 중 CPU 밖 대기(2.6–4.2 ms/frame)의 원인은 미분리다. `GCM_FIFO_DRAIN_SPLIT` trace로 GCM 락과 walk를 나눌 수 있다.
- **실게임 짝비교 미완** — root 이동 클립, 특수 AI, 프레임 대기 Lua 장면. F9 진동 설정 저장 후 재실행 확인.
- **충돌 진동 미확인 조합** — 11-26 플레이는 파이터·배트로이드, 프레임 상한 해제, 세기 80%였다. GERWALK, F9 30 FPS, 세기 0%·진동 끔은 실게임으로 보지 않았다. 사용자는 이 상태로 확정했다.
- **배포 반영 미정** — 이 브랜치는 `main`과 배포본에 합치지 않았다. 합칠 때는 릴리스 기준선 0.3.4와 런처 패키징을 따로 검토해야 한다.

## 참고: 함께 커밋한 이전 기록

- [30 FPS를 넘지 못한 이유](why-not-above-30fps-20261118.md) — 10-25~11-15 시도의 종합. 이후 11-24 상한 해제로 결론이 바뀌었다.
- `tools/diag/framegen_blend_check.py` — 프레임 생성 영상이 움직임 보간인지 단순 혼합인지 판정하는 진단 도구.
