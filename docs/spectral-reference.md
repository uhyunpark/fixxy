# Spectral Reference Guide — soothe3 계열, 작동 원리와 우리 구현 경로

## 1. soothe3 개요

- **제품 정체**: oeksound, 2026-05-20 출시. "dynamic resonance suppressor" — FFT로 전 주파수를
  동시에 분석하고, 주변 대역 대비 튀어나온 resonance만 초과량에 비례해 감쇠한다.
  **부스트 없음 (감쇠 전용)**.
- **soothe2 무상 업그레이드**: 2026-02-18 이후 구매자에게 grace period 적용.
- **soothe2에서 이어지는 기능**: soft(adaptive/상대 threshold) / hard(fixed) 모드,
  attack/release, sharpness(notch 날카로움 = 주파수축 커널 폭), selectivity(감지 민감도),
  delta 모드/band listen, oversampling/quality, 외부 sidechain, 레이턴시 대략 2-5ms
  (SR·모드 의존).
- **soothe3의 변화**:
  - **Detail** = sharpness + selectivity 통합 (단일 노브).
  - **Tilt** = Detail/Attack/Release 세 파라미터의 **주파수 의존 스케일**
    (저역/고역에서 다르게 작동하도록 기울임).
  - **Quality** = Normal/High/Ultra 3단계 — 기존 별도 oversampling 컨트롤 폐지, 품질 프리셋으로 흡수.
  - **low-latency 모드**: 기본 샘플레이트에서 0샘플, 고샘플레이트에서 ~1ms.
  - **linear-phase 모드**: low-latency와 별도로 존재 (선형 위상이 필요하면 레이턴시를 더 씀).
  - 노드 최대 8개 / 밴드 셰이프 8종, sidechain 모드, 최대 9.1.6채널 및 채널셋 링크.
  - soft 모드는 거의 완전히 **level-independent** (입력 레벨이 바뀌어도 감쇠 특성이 거의 불변).

## 2. 경쟁 지형

경쟁 구도를 가르는 축은 두 개다.

1. **감쇠 전용 vs 양방향**
2. **자기 포락선(self-envelope) vs 외부 sidechain vs 학습(learned profile)**

| 제품 | 접근 |
|---|---|
| **soothe3** | 감쇠 전용, 자기 포락선 상대 threshold (+ soothe3부터 sidechain 모드로 외부 SC도 흡수) |
| **Gullfoss** | **양방향**(부스트+감쇠) 톤 밸런싱. 이상적 스펙트럼 프로파일과 비교, 매크로 노브로 조작 |
| **sonible smart 시리즈** | 소스를 **학습**해서 프로파일 기반으로 처리 |
| **TDR Nova** | 이산 6밴드 dynamic EQ — 우리 `SvfBandNode` 다이내믹 EQ와 같은 계열. soothe는 이걸 "밴드 수를 FFT bin 수만큼 늘린" 극한으로 볼 수 있다 |
| **Trackspacer** | 외부 sidechain 스펙트럼 모양대로 감쇠 (스펙트럴 덕킹). soothe3의 sidechain 모드가 이 기능을 흡수 |

**우리 타겟**: 감쇠 전용 + 자기 포락선 상대 threshold. (soothe3의 core soft 모드와 동일 좌표.)

## 3. 작동 원리 — CONFIRMED vs INFERRED

라벨을 유지한다: CONFIRMED는 매뉴얼/공식 자료에 명시된 것, INFERRED는 레이턴시 실측·공개
지식으로 역산한 것. 구현 전 Stage 0/1에서 반드시 실측으로 갱신한다.

### CONFIRMED
- FFT 기반으로 **전 주파수를 동시에 분석**하고, 인접 대역에 영향을 주지 않는 감쇠 공식을 쓴다.
- **soft 모드 = adaptive 상대 threshold** — soothe의 상징적 개념이 공식 문서로 확인된다.
  (절대 threshold가 아니라 **주변 대역 평균 대비 상대적 초과량**을 감쇠한다.)
- 모드(soft/hard, low-latency/linear-phase, quality 단계)별로 **레이턴시 프로파일이 다르다**.
- Detail 노브는 tighter(좁고 정밀) ↔ broader(넓고 부드러움) 축으로 작동한다.

### INFERRED
- 레이턴시 수치(2-5ms @ 44.1kHz)로 FFT 창 크기를 역산하면 대략 **1024-2048 샘플** 창 크기와
  정합한다.
- 내부 구현 후보: **포락선 추정** → 후보는 주파수축 median filter / cepstral liftering /
  octave-boxcar smoothing → bin별 초과량 계산 → 초과량에 비례한 게인.
- **sharpness/selectivity** = 주파수축 게인 스무딩 커널의 폭(=Detail의 전신).
- **attack/release** = 시간축 시상수 (컴프레서 탄도와 동일 범주).
- **musical noise 억제**의 정체 = **시간축·주파수축 이중 스무딩**. soothe의 "투명함"은
  여기서 나온다고 추정.
- 결론: soothe의 비밀은 골격(FFT + 상대 threshold + 비례 감쇠, 전부 공개 지식)이 아니라
  **포락선 추정 방식과 이중 스무딩의 튜닝**에 있다.

## 4. 추론 파이프라인

```
STFT → 포락선(스펙트럼 평활화) → 상대 threshold 대비 초과량 →
초과분 비례 감쇠(게인) → 시간·주파수 이중 스무딩 → OLA 재합성
```

## 5. 우리 자산 이전표

| 구분 | 항목 | 비고 |
|---|---|---|
| **이전됨** | `CompressorNode` 탄도 (`tauToAlpha`, decoupled RtT, quadratic knee) | bin별 시간축 스무딩에 그대로 적용 가능 |
| **이전됨** | 다이내믹 EQ의 상대 게인 아키텍처 | 개념 동형 — 기준이 static gain에서 "이웃 포락선"으로 바뀔 뿐 |
| **이전됨** | `AnalyzerEngine`의 주파수축 스무딩 (boxcar prefix-sum) | 포락선 추정 후보 중 하나 |
| **신규** | OLA 재합성 | `AnalyzerEngine`은 display-only, IFFT 없음, 메시지 스레드에서 동작 — 오디오 스레드 STFT 프레이밍은 처음 |
| **신규** | 레이턴시 보고 | 지금까지 모든 노드가 latency 0이었다. STFT 도입이 첫 실전 사례 |

부수 참고: bin별 게인 곱은 다이내믹 EQ의 IIR 재-bake 방식과 다르다 — 여기는 매 프레임 bin
게인을 직접 곱한다.

## 6. 단계 계획

- **Stage 0** — 블랙박스 실측. 경로 A: 데모로 직접 측정. 경로 B: 공개 지식 + 자체 측정 기준.
- **Stage 1** — STFT 항등 프로토. COLA sum==1 검증, passthrough null <1e-10, 레이턴시 공식 검증.
- **Stage 2** — 포락선 + 상대 초과 + 비례 감쇠. 노이즈 위 사인 감쇠량을 공식과 대조, 톤은 통과하는지 확인.
- **Stage 3** — 시간축 탄도 이식. 시상수 라벨이 실측과 일치하는지, musical noise 지표가 줄어드는지 확인.
- **Stage 4** — C++ `SpectralNode`. 사전 할당, `setLatencySamples` 정확도, pluginval.
- **Stage 5** — 독립 제품 + delta UI + 실믹스 청취.
- **Stage 6** — 자연어(NL) 레이어 / sidechain 모드.

## 7. 함정

- **musical noise / 스펙트럴 홀**: 이중 스무딩(시간·주파수)이 유일한 방어선.
- **COLA 위반**: 만족하지 않으면 warble이 생긴다. Stage 1에서 반드시 수치로 검증.
- **레이턴시 재보고**: STFT 노드는 `setLatencySamples` 필수. 모드(quality/low-latency/linear-phase)
  전환 시 레이턴시가 달라지면 호스트에 재보고해야 한다(PDC 재협상). 지금까지 모든 노드가
  latency 0이었으므로 처음 겪는 경로.
- **스테레오 스펙트럴 링킹**: 초기 구현은 공통 게인맵(L/R 동일)으로 시작.
- **"핑크노이즈 갭" / 이음새 버그**: delta 모드로 실믹스를 직접 들어보는 것이 유일한 검증
  수단 — 스펙트럴판 "이음새 버그"에 해당한다.
- **FFT 크기 트레이드오프**: 512/1024/2048 — 해상도 대 시간 응답 대 레이턴시. quality 모드로
  노출할 가능성.
- **CPU**: bin 수 × 오버랩 × 프레임률. grouped-band(Bark/ERB) vs per-bin 중 먼저 고민,
  SIMD는 그 다음, GPU는 비권장.

## 8. 출처

- oeksound.com/plugins/soothe3
- oeksound.com/manuals/soothe3
- oeksound.com/manuals/soothe2
- production-expert.com — soothe3 upgrade 기사
- soundonsound.com — soothe3 뉴스
- musictech.com — soothe2 리뷰
- oeksound — sidechain latency support 페이지
- DSPRelated — Spectral Envelope Extraction
- CCRMA/JOS — Cepstral Windowing
- MSU — Suppression of Musical Noise
- IEEE — Cepstral Smoothing of Spectral Filter Gains
- theproducerschool — soothe2 vs gullfoss
- gearnews — soothe alternatives
- KVR — 비교 스레드
