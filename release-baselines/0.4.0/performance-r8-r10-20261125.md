# Macross 30 성능 정리 · R8–R10 · 2026-11-25

**종료 기준**은 4K 내부 해상도(`--render-scale 3`), FPS 제한 해제, 동일한 고부하 필드 전투에서 **연속 5초 평균의 최저값 ≥ 60 FPS**다. 라운드당 개선이 0.5 ms/frame 미만이면 최적화를 종료한다. R10을 마지막 최적화 라운드로 두었다. 빌드는 자동 실행하지 않으며 실게임 실행마다 사용자 승인을 받는다.

**결과(2026-11-25)** — 기준 미달로 종료했다. 최종 후보 `m25-lto`의 4K 필드 최저는 **49.5 FPS**(m24 53.3), R10의 실게임 이득은 측정되지 않았다(< 0.5 ms/frame). 사용자 결정으로 PGO 학습 없이 `m25-lto`를 최종 빌드로 두고 정리한다.

## 실게임 측정

| 빌드 | 조건 | 필드 결과 |
| --- | --- | --- |
| `m21-pgo` | 구형 27-object 구성, m20 필드 프로파일 | 대체로 60 FPS, 하락 구간 46–55 FPS, 렌더 17–19 ms/frame. `m21-lto`는 59–60 FPS, 렌더 약 11 ms/frame. 동선이 다르고 F8 측정 창이 없어 PGO 단독 효과는 미확정. |
| `m22-toggle` | F9 실시간 전환의 중간 빌드, vblank 2/1 | 해제 체크를 끈 상태에서 265초간 30.0 FPS. 체크하자 다음 초에 60 FPS. |
| `m22-lto` | 1.5×(3/2), 제한 해제 | 첫 필드 67–86 FPS(사용자 관측 77). 두 번째 필드 45–92 FPS, 메뉴 1,100–2,000 FPS. 두 번째도 실제 로그는 1.5×였다. |
| `m22-lto` | 4K(3/1), 제한 해제 | 47–95 FPS, 주로 55–80. 고부하 구간 렌더 CPU 18–21 ms/frame, GPU 여유. 이후 30 FPS로 13분 플레이에서 게임 속도 차이 보고 없음. |
| `m24-lto` | 4K(3/1), 제한 해제, 필드 약 140초 | 1초 간격 5초 창 132개: **최저 53.3 / p5 58.6 / 중앙값 74.4 FPS**, 60 미만 13개. 겹치지 않는 5초 창 28개: 최저 53.3 / 중앙값 71.7, 60 미만 4개, 렌더 CPU 중앙값 12.38 ms/frame. 진행 기록의 m22 4K 최저는 47 FPS. 사용자 진동 확인. |
| `m25-lto` | 4K(3/1), 제한 해제, 필드 약 230초 | 겹치지 않는 5초 창 46개: **최저 49.5 / 중앙값 69.5 FPS**, 60 미만 10개, 렌더 CPU 중앙값 12.27 ms/frame. 하위 창은 모두 필드 후반(m24가 닿지 않은 구간일 수 있음), 앞 140초만의 최저 59.9. 오류·큐 포화·드로우 누락 0. 첫 50초 짧은 실행은 최저 64.6. |

m24-lto의 하위 구간에서 렌더 스레드 idle은 없었다. FIFO wall 16–20 ms/frame, 렌더 CPU 14–17 ms/frame으로 약 3–4 ms의 대기 차이가 관측됐다. 당시 병목은 렌더 경로였고 대기의 세부 위치는 R10에서 확인한다. 기록 동선과 계측 설정 차이가 있어 m22→m24 차이를 개별 패치의 효과로 환산할 수 없다. [프레임별 비교](../build/scratch/r8/w12/REPORT-4.md#field-baseline)

실게임 수치는 로컬 진행 기록 `.superpowers/sdd/action-handler-dt-plan-20261121/progress.md`의 R8 항목을 기준으로 했다. 4K 실행 ID는 m22 `run-4k-free-20261124-235940`, m24 `run-field-4k-20261125-013904`다.

## 게임 동작

- **페이싱** — F9 `30 FPS 제한 해제`는 즉시 반영한다. 체크 해제 시 `effective_vblanks=2`와 저장된 VSync를 사용한다. 체크 시 `effective_vblanks=0`, 기본 Present interval 0이다. PUT·GPU 완료·GCM 콜백이 다음 작업을 깨운다. 게스트 flip의 100 µs 폴링은 해제 모드에서 상태 이벤트 대기로 바뀐다. 두 위치 모두 기본 러너에서 실측 dt를 쓴다. [W12 페이싱](../build/scratch/r8/w12/REPORT.md)
- **진동** — F9 `진동`은 기본 켜짐이고 `settings.ini`에 `rumble=0/1`로 저장한다. 1P 피격 생존 시 저주파 35%로 150 ms. 첫 다운 또는 파괴 시 저주파 100%·고주파 60%로 500 ms. 다운 진동은 상태 진입 때 한 번만 발동하고 SDL2/XInput에서 호스트 시계로 만료한다. 조작 중 ON/OFF를 적용하며 컨트롤러 테스트 펄스는 별도다. [W13 진동](../build/scratch/r8/w13/RUMBLE-BUILD.md)
- **카메라** — `k = elapsed_seconds × 30`. 수동 궤도 각도·락 오프셋은 `k`로 적분하고 추적·시점 수렴은 `1-(1-c)^k`로 보정한다. 락 타이머, 스크립트 스프링과 샷 램프에 경과 시간을 전달하고 샷 전환은 원래 틱을 따른다. [W13 카메라](../build/scratch/r8/w13/REPORT.md)
- **이동·UI** — mover 감쇠 9곳을 `d^k`로 변환했다. 매번 새로 구한 충돌 속도에 곱하는 접촉 비율은 유지한다. 매니저 조향 두 곳은 `1-(1-c)^k`, authored-tick 경로 `0052D7C4`는 기존 이득을 쓴다. 필드 오버레이 페이드는 정수 변환에서 버리던 소수분을 이월한다. [W13 이동·UI](../build/scratch/r8/w13/REPORT-2.md)
- **홀드·Lua** — 버튼 홀드 `1`은 프레스당 한 샘플, `2`는 지속 표식, `3` 이상은 경과 시간 기준이다. Lua `WaitFrame`/`EQWaitFrame`은 같은 값으로 다시 설정해도 재무장하고 같은 프레임의 반복 poll은 한 번만 진행한다. 관측된 AI override 두 곳은 읽기 전용이고 관측한 스켈레탈 경로는 이미 elapsed를 받았다. [W13 홀드·Lua](../build/scratch/r8/w13/REPORT-4.md)

## CPU 런타임과 렌더 경로

- **메인 PPU** — scalar guest load/store의 일반 RAM 구간 한계를 접근 폭별로 미리 계산한다. 특수 주소·진단·예약에 해당하면 기존 검사와 coherence를 탄다. hot lifted 14개 합계 **−28.06%**(이전 샘플 기반 **0.44 ms/frame 추정**). `jc_start`는 `PS3_JC_LATCH`를 캐시하고 성공 제출의 불필요한 전체 wake를 제거했다. 대기자 없는 제출 fixture 중앙값 **1,449 → 10.5 ns**. Edge clip은 SSE2로 바꿔 보존 메시 fixture **−31.1%**. [메모리](../build/scratch/r8/w14/REPORT.md) · [SPURS/Edge](../build/scratch/r8/w14/REPORT-2.md) · [제출](../build/scratch/r8/w14/REPORT-3.md)
- **GCM 콜백** — R9는 pending tick과 큐 힌트로 빈 pump가 경합 락에 들어가는 횟수를 줄였다. 활성 콜백 순서와 FIFO label·flip 완료는 유지한다. 동시 빈 pump 조건의 렌더 present-request probe **47.27 → 7.34 ns**. 실게임 프레임 기여는 미분리. [W14 GCM](../build/scratch/r8/w14/REPORT-5.md)

렌더 수치는 D3D12/RSX 리플레이 기준이다. 계측 범위가 겹치는 항목은 합산하지 않는다.

| 채택한 변경 | 리플레이 효과 |
| --- | --- |
| R8 `5621077` — 불변 descriptor table 재사용, heap base 조회 이동, 덮어쓰는 FP 상수 zero 제거 | recording **−0.205 ms/frame**(serial), **−0.199 ms/frame**(async). |
| R8 `721be7b` — transform-constant FIFO word 한 번 읽고 기존 경로에 전달 | FIFO **−0.007 / −0.075 / −0.280 ms/frame**(기본 / 추가 burst 1 / 4). |
| R8 `32944a7` — PSO 구조 키 memo, vertex register template과 조밀한 범위 준비, 바이트 검증을 거친 RAWN 재사용 | `get_pso` **−0.212**, `fetch_batches_hoisted` **−0.129 ms/frame**. 전체 recording의 paired 중앙값 **−0.221 ms/frame**. |
| R9 `9c755d6` — SRV source slot 버전과 sampler row 재사용, heap/root/PSO 중복 바인딩 제거, 소형 FP 상수 불변 업로드 재사용 | 일반 리플레이 descriptor **0.402 → 0.204**, draw state/binding **0.327 → 0.240 ms/frame**. 5프레임 descriptor copy **1,390 → 644**, heap/root/PSO 호출 **26,445 → 530**, FP 상수 쓰기 **423,120 → 240 B**. |
| R9 `9c755d6` — shader snapshot 재검증·variant 순회 축소, exact index/RAWN 재사용, texture decode·alias lookup·RT lookup 단축 | 일반 리플레이 `get_pso` **0.607 → 0.519**, vertex fetch **0.429 → 0.412**, texture binding **0.966 → 0.903**, RT/barrier **0.109 → 0.105 ms/frame**. R9 전체 recording **−0.65 ms/frame**(일반), **−2.27 ms/frame**(96 target). |

R9의 1×·3× 검증에서 메서드, 바인딩 데이터와 픽셀이 일치했다. [Descriptor·FIFO](../build/scratch/r8/w12/REPORT-2.md) · [PSO·vertex](../build/scratch/r8/w12/REPORT-3.md) · [R9 리소스·바인딩](../build/scratch/r8/w12/REPORT-4.md)

## 빌더와 미채택 실험

- **컴파일** — m22부터 lifted 96개를 전부 speed flags + ThinLTO로 빌드한다. 런타임 4개를 합쳐 LLVM 객체 100개, 게스트 진입점 26,746개를 검증한다. `/arch:AVX2`, FP contraction off, 최대 3 compile/ThinLTO backend jobs를 유지한다. [W15 통합](../build/scratch/r8/w15/REPORT.md)
- **PGO** — m20의 atomic trainer는 실게임 5–6 FPS였다. 새 trainer는 `single` 카운터 99개와 계측을 뺀 `spurs_job.c` 1개를 쓴다. PPU fixture에서 PGO 빌드 대비 **atomic 28.43× → single 2.23×**. 새 학습과 적용 빌드의 lifted/runtime 프로파일 입력 해시를 맞춘다. m20의 선택 범위 프로파일로 최신 전체 범위를 대체할 수 없다. [PGO 측정·절차](../build/scratch/r8/w15/REPORT.md#field-training-and-pgo-handoff)
- **ISA·배치** — x86-64-v3는 호스트에서 지원하지만 기존 AVX2 타깃 대비 재현 가능한 이득이 없었다. m17 `/ORDER`는 상위 90% 샘플 함수의 시작 페이지를 **693 → 604**로 줄였으나 반복 타이밍 이득이 미확정이었다. m20 순서, call-graph sort 해제, `/Gy /Gw /OPT:REF,ICF`도 속도 개선을 재현하지 못했다. `-fwhole-program-vtables`는 코드 동일, machine-function splitting은 Windows 타깃에서 지원되지 않았다. 기존 ISA와 링크 순서를 유지한다. [W15 실험](../build/scratch/r8/w15/REPORT-2.md)
- **렌더·런타임** — threaded recorder는 오프스크린에서 42% 느렸고 write-watch/RAWN 필드 경로는 주요 드로우가 동기 실행된다. fence마다 descriptor 무효화하는 안은 async recording 절감이 0.060 ms/frame에 그쳤다. HLE/간접 호출, VMX min/max, PPU 예약 스캔, SPU coherence 및 guest lock/semaphore의 R9 추가 실험은 고부하 병목 개선 근거가 부족해 되돌렸다. 4-way native animation worker도 두 8-job 보존 리스트에서 **+0.224 / +0.106 ms/list**, 실게임 park는 **0.1–0.8 ms/frame**이어서 기본 경로를 유지했다. [Threaded recorder](../build/scratch/r7/w10/REPORT.md) · [Descriptor trial](../build/scratch/r8/w12/REPORT-2.md#host-replay-results) · [R9 런타임](../build/scratch/r8/w14/REPORT-5.md#r9-scope) · [Animation worker](../build/scratch/r8/w14/REPORT-4.md)

## 빌드와 실행

PowerShell 예시. 이미 존재하는 후보는 새 이름으로 빌드한다. 산출물과 실행 로그는 `build/tests/Macross-real60/<build>/` 아래에 남는다. `--anim-workers`는 비교용 코드를 포함하며 실행 기본값은 기존 단일 경로다. 실행 명령은 **승인 후 별도로** 수행한다. [후보 명령과 해시](../build/scratch/r8/w15/REPORT-3.md)

```powershell
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
python -B tools/build/build_real60_candidate.py m25-local --mode thinlto --jobs 3 `
    --gameplay-rumble --render-capture --field-probes --anim-workers
```

```powershell
python -B tools/diag/run_real60.py --build m25-local --label field-4k --step measured --seconds 60 --render-scale 3
```

| 스위치·진단 | 사용법 |
| --- | --- |
| 페이싱·해상도 | `--step measured` 기본, `--step ref30` 원래 30 Hz 스텝 비교, `--vsync`는 해제 상태의 동기 Present 비교. `--render-scale 3`을 지정하고 `stderr.log`에서 `[render-scale] 3/1`과 `[runtime-options] fps_unlock=1 effective_vblanks=0` 확인. |
| 프레임 진단 | `intervals.csv`로 present 간격 집계. F8 또는 `measure.go`로 `--seconds` 길이의 상세 측정 창을 연다. `--trace`는 `motion.csv`, `--stall 12`는 인위적 프레임 지연, `--fifo-idle`/`--fifo-idle-file`은 렌더 idle 대기 실험. `--profile-spu`, `--anim-lifted`/`--anim-compare`, `--record`는 별도 계측 옵션. |
| 필드 probe | `--field-probes`로 빌드 후 `--env 'MACROSS_ACTOR_FIELD_TRACE={run}/field-trace.csv'`. 홀드·반복, AI, root/body pose, overlay, Lua 이벤트를 확인한다. [CSV 및 A/B 절차](../build/scratch/r8/w13/REPORT-3.md#paired-run) |
| Animation park | 게임 프로세스 환경의 `MACROSS_PPU_ANIM_PARK_REPORT=1`. `[ppu-anim-park]`의 `wall_ms`, `parks`, QPC 범위를 `intervals.csv`에 맞춘다. 기본 러너의 `--env`는 actor/action 접두어만 허용하므로 park 변수는 `run_framegen.launch`의 `extra_env`에 전달한다. [계측과 집계](../build/scratch/r8/w14/REPORT-3.md#main-animation-park-accounting) |
| RSX 캡처 | `--render-capture`로 빌드. 성능 동선을 먼저 측정한 뒤 고부하 구간에서 실행 폴더의 `stderr.log` 옆에 빈 파일 **`render_cap`** 생성. `[render-capture] start` → `frame=` → `wrote`에서 `.rscap` 확인. 기본 4프레임, 최대 384 MiB. 파일을 제거하면 다음 캡처를 재무장한다. |

```powershell
New-Item -Path (Join-Path $run 'render_cap') -ItemType File
python -B tools/tests/replay_field_capture.py $capture --strict --output $replayOut
```

`$run`은 실행 폴더, `$capture`는 `wrote`에 찍힌 `.rscap`, `$replayOut`은 별도 출력 폴더다. RSX 리플레이는 wall·thread cycles를 분리 기록한다. [캡처·리플레이](../build/scratch/r8/w12/REPORT-5.md)

## R10 · 리플레이 절감, 실게임 이득 없음

- **입력** — m24-lto 4K `run-field-4k-20261125-013904`의 캡처 `107931`(61.7 FPS, 15.6 ms)과 `114892`(51.9 FPS, 16.2 ms).
- **채택 `6eddb7c`** — RAWN vertex cache fill을 불변 16-byte 정렬 슬롯에 넣고 persistently mapped 2 MiB upload slab을 공유한다. 마지막 cache 참조가 사라지면 기존 GPU fence 경로로 회수한다. 용량을 넘으면 기존 단독 resource를 쓴다.
- **4K 리플레이** — 4프레임당 committed vertex upload **1,835 → 5**, **2,376 → 4**. RAWN cache store QPC **24.298 → 0.541**, **35.992 → 0.379 ms/replay frame**, 명령·바인딩 바이트·픽셀 일치. 첫 콜드 프레임의 미기록 read 한 건은 양쪽이 같은 fallback을 탔고 시간 집계는 이후 3프레임 기준이다. 리플레이의 캐시 워밍업 비용 때문에 이 수치를 실게임 FPS로 환산할 수 없다. [W12 R10](../build/scratch/r8/w12/REPORT-6.md)
- **미채택** — write-watch reset 통합은 두 번째 캡처에서 recording을 0.719 ms/frame 늘렸다. 1×1 exposure readback과 flush는 warm 리플레이에서 각각 약 0.015, 0.05 ms/frame이었다. FIFO wall–CPU 차이를 GCM 락, 드라이버/API, 스케줄링 중 하나로 특정할 근거는 확보하지 못했다. [FIFO 대기 분석](../build/scratch/r8/w12/REPORT-6.md)
- **4K 실게임** — `m25-lto` `run-field-4k-20261125-230619`(렌더러 `6eddb7c` 확인). 겹치지 않는 5초 창 기준 최저 **49.5**, 중앙값 **69.5 FPS**, 60 미만 46개 중 10개. 같은 방식의 m24는 최저 53.3, 중앙값 71.7. 렌더 CPU 중앙값 **12.38 → 12.27 ms/frame**으로 차이가 측정 오차 안이다. 리플레이 절감은 캐시 생성 비용이 큰 리플레이 조건에서만 나타난 것으로 본다. 하위 창은 대부분 렌더 스레드 CPU 15–18 ms/frame이고, 225초 창만 게스트 작업 대기 5.6 ms/frame이었다.
- **종료 판정** — 최저 60 기준 미달, 라운드 이득 0.5 ms/frame 미만. 규칙대로 최적화 라운드를 종료했다.

빌더 pin은 `7d8df7c`다. 두 후보는 `build/tests/Macross-real60/<후보>/`에 있고 같은 렌더러 소스와 5개 companion 해시를 사용한다. 각 빌드는 exit 0, `game_started: false`, ThinLTO 객체 100개, 게스트 진입점 26,746개를 검증했다. `m25-lto`는 652.33초, peak working set 3.87 GiB였다. `m25-instr`는 428.04초, 2.54 GiB였다. 트레이너는 G8 probe와 실험용 animation worker를 빼고 `single` 카운터 99개를 사용한다. `spurs_job.c`는 계측 없이 ThinLTO에 포함된다.

| 후보 | 산출물 | SHA-256 |
| --- | --- | --- |
| `m25-lto` | `Macross30Recompiled.exe` | `74d90eaf5dd5aaefc444579e082520932bbc8c9e78c4f9cc52a8416d82f21fb2` |
| `m25-lto` | `Macross30Recompiled.pdb` | `b6316320855f386c4267ae5e2daec35d948290a8f95c6e3ac21c78b6c8082ee1` |
| `m25-lto` | `Macross30Recompiled.map` | `e6cb0844c508072fd7171caaa3303f868134fef5390c03234d7bad6f7fbd8fd5` |
| `m25-instr` | `Macross30Recompiled.exe` | `37be0eeb4e77ba61c68ce0f39485c989bfc73e8e0b66e1590c61bbe2f35c1df0` |
| `m25-instr` | `Macross30Recompiled.pdb` | `bff50701040ec4883b8e5bdd7c949d68bbc6181612eecac7bd8fb37792fdaa4f` |
| `m25-instr` | `Macross30Recompiled.map` | `67f660457b3b52522b40c0fc0ecd677edffe7e0fdb7f1356ae43fc87df32cf37` |

## 최종 PGO · 미실행

2026-11-25 사용자 결정으로 PGO 학습을 하지 않고 `m25-lto`를 최종 빌드로 둔다. `m25-instr`는 같은 소스의 trainer로 남아 있어 나중에 아래 절차를 그대로 쓸 수 있다.

최종 채택 소스와 일치하는 trainer에서 대표 필드 전투를 한 번 플레이하고 정상 종료한다. R10 경로를 선택하는 `--render-capture`는 양쪽 빌드에 전달하며, 학습 중에는 `render_cap`을 만들지 않는다. `--pgo-profile`은 실행 폴더에 `ppu-<PID>.profraw`를 쓴다. 새 프로파일은 소스가 일치하는 trainer 이름과 함께 적용한다. [병합 절차](../build/scratch/r8/w15/REPORT-2.md#field-training-and-profile-application)

```powershell
# $trainer, $final은 후보 이름, $raw, $merged, $llvm은 경로로 설정
# 일치하는 trainer가 없을 때 빌드
python -B tools/build/build_real60_candidate.py $trainer --mode instrumented --jobs 3 --gameplay-rumble --render-capture
# 사용자 승인 후 학습 실행
python -B tools/diag/run_real60.py --build $trainer --label pgo-train-field --step measured --seconds 60 --render-scale 3 --pgo-profile
# 정상 종료 후 새 실행 폴더의 ppu-*.profraw를 $raw로 지정
python -B tools/build/pgo_candidate.py merge --profdata "$llvm/llvm-profdata.exe" --out $merged $raw
python -B tools/build/build_real60_candidate.py $final --mode pgo --jobs 3 `
    --gameplay-rumble --render-capture --profile $merged --training-build $trainer
```

- **최종 빌드** — `m25-lto`(EXE `74d90eaf…fd5`, 위 R10 표). PGO 학습 run과 profdata는 없다.
- **남은 동작 확인** — root 이동이 있는 클립, 특수 AI, 프레임 대기 Lua 장면의 짝비교와 F9 진동 저장 후 재실행 확인. 최종 수치와 유지할 후보를 기록한 뒤 산출물을 정리한다.
