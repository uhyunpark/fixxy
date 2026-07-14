# Meta-Plugin Project

> 이 문서에는 **구현 중 검색으로 찾을 수 없는 것**만 담는다.
> API 시그니처, biquad 계수 공식, 순환 버퍼 구현 등은 필요할 때 찾아본다.
> 여기 남는 것: 제약 / 결정 / 함정 / 어디를 파야 하는지.

## 목표

자연어 요청으로 DSP 그래프를 구성/재구성하는 DAW 플러그인.
선행 조건: EQ, Compressor, Saturator, Delay, Spectral processor를 직접 구현해 노드 라이브러리 확보.

---

## 제약: 오디오 스레드 (설계를 결정한다)

`processBlock()`은 44.1kHz/128샘플 기준 2.9ms마다 호출. 이 안에서 금지:
동적 할당, mutex lock, 파일 I/O, 네트워크, 로깅, 예외, 시스템 콜.

→ **LLM은 오디오 스레드에 진입 불가.** 스레드 분리가 강제된다.

```
[UI]              [백그라운드]                    [오디오]
프롬프트  →  LLM → DSP 정의 생성 → 검증  →  lock-free 그래프 스왑
              (수백 ms ~ 수 초)              (2.9ms 데드라인)
```

LLM은 실시간으로 소리를 만들지 않는다. 실시간 DSP 엔진을 **오프라인에서 구성**한다.

---

## 결정 사항

- **스택**: JUCE + CMake/Ninja/Clang. Python(numpy/scipy/sounddevice)으로 DSP 선(先)프로토타이핑.
- **메타 플러그인 접근**: 노드 그래프(A)로 MVP → Faust JIT(B) 하이브리드로 확장.
  - A: LLM이 코드가 아닌 **그래프 JSON** 생성. 스키마 검증 가능, 크래시 리스크 낮음. 표현력은 노드 라이브러리에 종속.
  - B: LLM이 Faust 소스 생성 → libfaust JIT. 표현력 무제한, 컴파일 에러 자가수정 가능. 발산 위험 → 검증 필수.
- **레퍼런스 코드는 읽기만** (AGPL). 논문 → 직접 구현 → 대조.
- **EQ 차별화 축**: DSP 품질(matched filter)과 기본 워크플로우 UI는 입장료 — ZL 동급까지만.
  차별화 투자는 **자연어 조작 레이어**에: 의도("보컬이 harsh해") + 애널라이저 컨텍스트 →
  LLM이 BandParams JSON 배치, 대화형 반복. smart:EQ류와의 차이 = 커브 통계가 아닌 의도 해석.
  이건 메타 플러그인의 프롬프트→파라미터 매핑(제품 IP)을 EQ 1노드로 선행 검증하는 것.

---

## 제품 구조 — 효과기별 개별 플러그인 먼저

ZL Audio와 동일한 전략: 메타 플러그인으로 바로 가지 않는다.

- 효과기(EQ, Compressor, Limiter, ...)마다 **독립 플러그인 타겟**을 만든다.
  `src/dsp/`의 노드 라이브러리는 모든 타겟이 공유.
- 각 제품은 **개별적으로** pluginval / DAW 로드 검증을 통과해야 한다.
  통합은 그 다음 — 노드가 메타 플러그인 그래프에서 실제로 조합 가능한지는
  그때 확인한다.
- UI는 지금 전부 **제네릭/자동 생성** (`GenericAudioProcessorEditor` 등).
  제품별 UI 폴리싱은 하지 않는다 — 지금 목표는 DSP 노드 검증이지 제품 완성도가 아니다.
- CMake: 타겟마다 반복되는 설정(`juce_add_plugin` 옵션, 링크, 워닝 플래그)은
  헬퍼 함수로 묶는다. 타겟 추가 = 소스 목록 + 제품명 + PLUGIN_CODE만 새로 씀.

---

## 함정 (모르면 시간 날림)

- **파라미터 스무딩** 안 하면 지퍼 노이즈.
- **오버샘플링** 안 하면 비선형 처리가 앨리어싱으로 싸구려 소리. 이걸 빼면 새추레이션 튜닝 전부 무의미.
- **필터 계수 실시간 변조** 시 biquad 불안정 → ZDF/TPT(SVF) 구조. (Zavalishin / Wishnick)
- **레이턴시 보고** 누락 시 트랙 정렬 깨짐 (룩어헤드/FFT 사용 시).
- **상태 직렬화**에 생성된 DSP 정의까지 포함해야 프로젝트 재로드가 됨.

---

## 구현 순서

1. Gain — 플러그인 골격, 파라미터 스무딩 (완료)
2. Parametric EQ — biquad/SVF, 실시간 계수 갱신
   - EQ 밴드 1개 (peaking, TPT-SVF) — 완료, Week 2, 커밋됨
   - → 다음: 멀티밴드 EQ 제품화
3. Compressor — 엔벨로프 팔로워, 니, 사이드체인, GR 미터 (다음 제품)
4. Saturator + Oversampling — 앨리어싱 확인 및 제거
5. Delay / Chorus — 딜레이 라인, 보간, 피드백, 모듈레이션
6. Spectral Processor — STFT 파이프라인 (→ Soothe 계열)
7. 노드 그래프 엔진 — 1~6을 조합 가능한 노드로 리팩터링
8. Lock-free 그래프 스왑
9. LLM 레이어 → MVP
10. Faust JIT 통합

각 단계: 논문 → Python 프로토타입(소리 확인) → C++ 구현 → ZL 대응 리포와 대조

**제품 단위는 단계와 별개다.** 각 효과기는 독립 플러그인 제품으로 먼저 출시하고
검증한다 (아래 "제품 구조" 참고). 메타 플러그인 통합은 그 다음이다.

---

## 난제 3가지

### 1. Lock-free 그래프 스왑

- 백그라운드에서 새 DSP를 **완전히** 준비 (할당 + prepare 완료)
- `std::atomic<DspGraph*>` CAS 교체. 오디오 스레드는 블록 시작 시 포인터 1회 읽기
- 교체 시 10~50ms 크로스페이드 (없으면 클릭)
- 낡은 그래프는 가비지 큐 → 백그라운드에서 delete
  (**오디오 스레드에서 delete = 할당 = 금지**)

### 2. 생성 DSP 안전 검증 (활성화 전, 오프라인)

NaN/Inf, DC 오프셋, 출력 레벨 상한, 임펄스/사인 스윕 응답 유한성, CPU 예산.
**최종 라인에 하드 리미터 — 필수.**

### 3. 프롬프트 → 파라미터 매핑 (제품의 IP)

시스템 프롬프트에 오디오 엔지니어링 용어 사전 + 노드 라이브러리 스펙 + few-shot.

---

## 복각 원리

### 디지털 알고리즘 (Soothe 등) — 블랙박스 시스템 식별

측정으로 내부 구조를 역추론한다. 귀가 아니라 테스트 신호로.

| 측정 | 알아내는 것 |
|---|---|
| 리포트 레이턴시 | FFT 사이즈 / 오버랩 |
| 화이트노이즈 in/out delta | 대역 해상도, 로그/선형 분할 |
| 단일 사인 레벨 스윕 | 임계 곡선 — 절대 임계 vs **주변 대역 평균 대비 상대 임계** |
| 사인 on/off | GR 시간 응답 (attack/release 시상수) |
| 인접 두 사인 | 스무딩/누설 커널 폭 |
| Delta 모드 | 제거되는 성분 직접 청취 |

Soothe 계열의 핵심: **주변 대역보다 튀어나온 만큼만 감쇠.**

추론되는 파이프라인:
`STFT → 대역별 에너지 → 스펙트럼 평활화(포락선) → 초과분 → 초과분 비례 감쇠 → 시간 스무딩 → 재합성`

### 아날로그 — 비선형 시스템 식별

- **화이트박스**: WDF / state-space. 회로를 실제로 푼다. 비선형 소자는 Newton-Raphson.
- **블랙박스**: DC 스윕 → 전달 곡선 → 웨이브셰이퍼 LUT. 하모닉 분석. 볼테라/하머스타인. 신경망(NAM).
- **그레이박스 (현실적)**: 회로 구조만 참고, 각 블록은 근사 후 실측 피팅.
  ```
  in → [입력 트랜스: 저역 포화 + HF 롤오프]
     → [게인단: 비대칭 tanh, 레벨 의존]
     → [톤 스택: biquad]
     → [출력 트랜스: 히스테리시스]
     → [노이즈 플로어]
  ```

### "느낌"의 물리적 정체

| 느낌 | 정체 |
|---|---|
| 따뜻함 | 낮은 차수 짝수 배음(2차), HF 롤오프 |
| 펀치 | 레벨 의존 시상수, 과도응답 오버슛 |
| 글루 | 프로그램 의존 릴리즈 |
| 두께 | 저역 트랜스포머 포화 + 위상 회전 |
| 살아있음 | 노이즈 플로어, 부품 오차, 채널 불일치 |
| 숨쉼 | 히스테리시스 (출력이 직전 상태에 의존) |

디지털이 차가운 이유: 대칭, 무기억, 시불변, 무노이즈.
복각 = **의도적 결함 주입**:

1. 비대칭성 — `tanh(x + bias)` → 짝수 배음
2. 히스테리시스 — 순수 함수가 아닌 상태 기계
3. 주파수 의존 비선형 — 저역 더 포화
4. 레벨 의존 필터링 — 크게 때리면 고역 롤오프
5. 프로그램 의존 시상수
6. 불완전성 — 노이즈, 파라미터 편차

> 메타 플러그인 관점: 위 요소들이 그대로 파라미터화된 노드가 된다.
> `asymmetry`, `hysteresis_amount`, `hf_rolloff`, `program_dependency` → LLM이 조합.

---

## 레퍼런스: ZL Audio (github.com/ZL-Audio)

zsliu98의 오픈소스 JUCE 플러그인. 구현 순서와 거의 1:1로 대응.

| 리포 | 대응 |
|---|---|
| ZLEqualizer | 다이내믹 EQ, SVF |
| ZLCompressor | 컴프레서 |
| ZLInflator | 디스토션/새추레이션 |
| zldsp_fft | FFT |
| ZLSpectrumEqualizer | 스펙트럴 |
| ZLSplitter | peak/steady, M/S, transient 분리 |

**AGPL-3.0 (바이럴 카피레프트).** 코드 사용 시 소스 공개 의무. 읽고 배우는 용도로만.

**빌드 시스템은 참조하지 않는다.** ZL의 템플릿/CMake를 fork하면 (a) 라이선스가 상용화 시점에 발목을 잡고 (b) 빌드가 블랙박스가 돼서 나중에 Faust/LLVM 링킹을 붙일 때 막힌다. ZL은 **DSP 코드 대조용으로만** 본다.

---

## 논문 / 자료

- Giannoulis, Massberg, Reiss. *Digital dynamic range compressor design — A tutorial and analysis*. JAES (2012)
- Martin Vicanek. *Matched Second Order Digital Filters* (2016) / *Matched One-Pole Digital Shelving Filters* (2019)
- Aaron Wishnick. *Time-Varying Filters for Musical Applications*. DAFx (2014) — 계수 실시간 변조 시 불안정
- Vadim Zavalishin, *The Art of VA Filter Design* (무료 PDF)
- Will Pirkle, *Designing Audio Effect Plugins in C++*
- Ross Bencina, *Real-time audio programming 101* — 오디오 스레드 규칙
- musicdsp.org / Faust 문서 / DAFx proceedings

---

## 빌드 시스템 — 직접 소유한다

남의 템플릿을 fork하지 않는다. 최소 구성으로 시작하고, 필요할 때만 늘린다.

```
lpngsound/
├── CMakeLists.txt      # JUCE 서브모듈 + 플러그인 타겟들 (lpng_add_plugin 헬퍼)
├── external/JUCE/      # submodule
├── src/
│   ├── dsp/             # 공유 노드 라이브러리 (SvfPeakNode 등, header-only)
│   └── plugins/
│       └── eq/          # EQ 제품 (PluginProcessor.h/.cpp)
├── proto/              # Python 프로토타입
└── CLAUDE.md, PROJECT.md
```

CI, 인스톨러, 코드 서명, 유니버설 바이너리는 전부 나중. 지금은 로컬 빌드 → DAW 로드만 되면 된다.
pamplejuce의 CMakeLists를 **읽고 이해**하는 건 좋지만, fork하지 말고 필요한 부분만 직접 쓴다.

---

## 다음 액션

1. ~~JUCE 서브모듈 + 최소 CMakeLists 직접 작성 → 빌드 통과~~ (완료)
2. ~~Gain 플러그인 → DAW 로드 → pluginval 통과~~ (완료)
3. ~~Python DSP 프로토타이핑 환경 세팅 (`proto/`)~~ (완료)
4. ~~EQ 밴드 1개(peaking) 구현, 멀티 타겟 빌드로 분리~~ (완료, Week 2)
5. 멀티밴드 EQ로 확장 → 독립 제품(LpngEQ)으로 pluginval 통과
6. Compressor 제품 시작
