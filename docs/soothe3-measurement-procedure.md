# soothe3 블랙박스 측정 절차 (Stage 0)

목적: soothe3를 소스 없이 측정만으로 복각하기 위한 수치 타깃 확보.
Logic Pro에서 프로브 WAV를 soothe3에 통과시켜 바운스 → `soothe3_measure_analyze.py`가 분석.

- 프로브 입력: `proto/out/soothe3_kit/in/`  (먼저 `soothe3_measure_gen.py` 실행)
- 바운스 저장: `proto/out/soothe3_kit/out/`
- 파일명 규칙: **`<베이스>__<프리셋>.wav`**  (예: `noise_white__soft_detail50.wav`)
  - `<베이스>` = in/ 파일 이름 그대로 (`noise_white`, `sine_staircase_1k`, ...)
  - `__` 는 언더바 **두 개**. 분석기가 이걸로 프리셋을 파싱함.

---

## 0. 공통 바운스 설정 (모든 바운스 동일)

- [ ] Logic: 각 in/ WAV를 **자기 트랙 하나씩**에 임포트 (SR 44.1 kHz 유지).
- [ ] 프로젝트 SR = **44100 Hz**, 트랙에 리샘플/타임스트레치 금지.
- [ ] **Offline(오프라인) 바운스**, **Dither 끄기**, **32-bit float**, **Normalize 끄기**.
- [ ] 바운스 길이 = 소스 길이와 동일 (꼬리 자동연장 off).
- [ ] soothe3 외 다른 플러그인/게인/팬 전부 bypass, 채널 게인 0 dB.
- [ ] 소스는 전부 -6 dBFS 이하(헤드룸 ≥6 dB) → soothe3가 약간 게인해도 클립 없음.

## soothe3 공통 파라미터 (특별히 명시된 것 외 전부 기본값)

- [ ] **Depth = 100%** (soothe3 init 기본값). *UI에서 실제 기본값 확인 후 그 값으로 고정하고 여기 적어둘 것* → 실제값: `______%`. 모든 바운스에서 동일하게.
- [ ] Delta 모드: 명시된 delta 행 외에는 **꺼짐**.
- [ ] Quality/모드: 아래 latency 표 측정 시 외에는 **기본 Quality** 로 고정.
- [ ] 밴드/노드: 기본 상태(수동 노드 추가 금지). soft/hard·Sharpness·Attack·Release만 행별로 변경.

용어 매핑 (soothe3 버전에 따라 라벨 다를 수 있음 — owner가 확인):
- **soft / hard** = 리덕션 캐릭터 토글.
- **Detail(=Sharpness)** = 주파수 선택폭 노브. `detail0/50/100` = 이 노브를 0/50/100%로.
- **Attack / Release** = 게인 리덕션 시상수 노브.

---

## 1. 바운스 매트릭스 (총 19개)

각 행: soothe3 설정 → 저장 파일명 → 왜 이 측정인가.

| # | 저장 파일명 | soothe3 설정 | 왜 이 측정인가 |
|---|---|---|---|
| 1 | `passthrough_ref__noplugin.wav` | **soothe3 트랙에서 제거** (체인에 아무것도 없음) | PDC 기준선 0. host 정렬 오프셋 확인 |
| 2 | `passthrough_ref__bypassed.wav` | soothe3 삽입, **Bypass ON** | 삽입만으로 생기는 레이턴시(=보고 레이턴시) 측정. Logic PDC가 이미 보정했으면 0, 아니면 잔차가 보임 |
| 3 | `noise_white__soft_detail50.wav` | soft, Detail 50 | 백색소음 in/out 차 → 밴드 분해능 + 엔벨로프 스무딩 폭 |
| 4 | `noise_white__hard_detail50.wav` | hard, Detail 50 | soft/hard 간 밴드 분해능 차이 비교 |
| 5 | `sine_staircase_1k__soft.wav` | soft, Detail 기본 | 레벨 계단 → 임계 성격. soft가 레벨 무관 감쇠면 **상대(RELATIVE) 임계** |
| 6 | `sine_staircase_1k__hard.wav` | hard, Detail 기본 | hard가 고정 레벨 이상에서만 걸리면 **절대(ABSOLUTE) 임계** |
| 7 | `sine_staircase_4k__soft.wav` | soft, Detail 기본 | 다른 주파수(4k)에서 임계 성격 재확인 |
| 8 | `sine_staircase_4k__hard.wav` | hard, Detail 기본 | 4k에서 soft/hard 임계 대비 |
| 9 | `resonance_in_noise__soft_detail50.wav` | soft, Detail 50 | 소음 위로 솟는 3k 톤 → **초과량(exceed)→리덕션 전달곡선** (핵심 메커니즘, 머니 플롯) |
| 10 | `burst_3k__soft.wav` | soft, Attack/Release **기본값** (값 기록: A`___` R`___`) | 온/오프 버스트 → GR attack/release 시상수 |
| 11 | `burst_3k__soft_slowrel.wav` | soft, Release **최대(극단값)**, Attack 기본 | release 극단에서 시상수 재측정 → 노브→ms 매핑 |
| 12 | `pair_sweep__detail0.wav` | soft, Detail **0** | 인접 톤쌍 → 주파수 커널 폭 (가장 넓음) |
| 13 | `pair_sweep__detail50.wav` | soft, Detail **50** | Detail→커널폭 매핑 중간점 |
| 14 | `pair_sweep__detail100.wav` | soft, Detail **100** | 커널 폭 (가장 좁음) |
| 15 | `resonance_in_noise__soft_detail50_delta.wav` | soft, Detail 50, **Delta ON** | 제거된 성분만 캡처 → 리덕션 절대 기준(제거량 검증) |
| 16 | `mix__delta.wav` | 아무 실제 믹스 스니펫(owner 선택) 트랙에 soothe3, **Delta ON** | 실제 소재에서 soothe3가 실제로 무엇을 빼는지 정성 확인 |
| 17 | `notch_probe__detail0.wav` | soft, Detail **0** | 3k 단일 톤+광대역 베드 → soothe가 파는 노치의 **폭 직접 측정**(커널 가장 넓음) |
| 18 | `notch_probe__detail50.wav` | soft, Detail **50** | 노치 폭 중간점 |
| 19 | `notch_probe__detail100.wav` | soft, Detail **100** | 노치 폭 (커널 가장 좁음) |

**#17–19은 pair_sweep(#12–14)를 대체하는 게 아니라 보완한다.** pair_sweep은 두 톤에만 에너지가 있어 톤 사이 간격의 노치 모양(=커널 폭 oct)을 볼 수 없고, 톤이 1500–6000 Hz로 움직여 절대주파수 의존성과 뒤섞인다. 그래서 pair_sweep은 "간격 상호작용 강도(selectivity 지문)"만, notch_probe는 광대역 베드에 찍힌 노치의 FWHM로 **커널 폭(oct/Hz)을 직접** 준다. Detail→커널폭 매핑의 확정값은 #17–19에서 나온다.

주의(#10, #11): attack/release는 **온셋(톤 켜짐)에서 attack**, **오프 구간의 노이즈 베드에 남는 GR 회복에서 release**를 읽음. 그래서 in/ 파일에 -50 dBFS 소음 베드를 깔아둠 — 베드 트랙 볼륨 건드리지 말 것.

#16은 in/ 짝이 없으므로 자동 분석 대상 아님(정성 참고용). 파일명에 `__delta` 유지.

---

## 2. Latency 수동 기록표 (바운스 아님 — UI/host 값 눈으로 읽기)

soothe3 Quality/모드를 바꿔가며, Logic의 플러그인 레이턴시 표시(또는 soothe3 UI의 latency 표시)를 읽어 적는다.

| Quality 모드 | 보고 레이턴시 (samples) | (ms) |
|---|---|---|
| Normal | `______` | `______` |
| High | `______` | `______` |
| Ultra | `______` | `______` |
| Low-latency (있으면) | `______` | `______` |

→ 이 값으로 **FFT 크기/오버랩** 역산 (레이턴시 ≈ FFT window - hop, 또는 window/2 성분).
#2 바운스의 실측 오프셋과 대조.

---

## 3. 바운스 후

```
./proto/.venv/bin/python proto/soothe3_measure_analyze.py
```

- out/ 에 있는 파일만 자동 분석, 없는 건 `[TODO]` 로 건너뜀 → **점진적으로 바운스해도 됨**.
- 섹션별 수치 + 플롯(`proto/out/soothe3_kit/plots/`) + 맨 끝 **TARGET TABLE**(마크다운) 출력.
- 이 TARGET TABLE이 C++ 복각의 수치 스펙 초안이 됨.

메모: soothe3 버전/기본값이 위 가정과 다르면(특히 Depth 기본, soft/hard·Detail 라벨) 실제값을 표 옆에 적어두면 분석 해석이 정확해짐.
