# soothe3 UI 분석 → LpngSpectral v2 이식 항목

소스: 소유자 스크린샷 2장 (2026-08-12, Factory default + 밴드 6개 상태).
레이아웃 로직만 참고, 색/형태 트레이드드레스는 복제 금지 (AGPL/디자인 규칙 동일 적용).

## 이미 있는 것

depth / detail / attack / release 노브, mix / out 칩, bypass / delta 필,
스펙트럼 + 리덕션 표시, 레이턴시 보고.

## 스크린샷 요소 분해

### 1. 메인 패널 — 가중치 커브 (색 dot = "밴드")

soothe 용어로 **depth curve**. 오디오를 직접 거르는 EQ가 아니라
**"이 주파수 영역을 얼마나 세게/약하게 처리할지"의 가중치 커브**. 매뉴얼 표현: inverse EQ —
커브를 올리면 그 영역 억제가 강해진다.

| 요소 | 스크린샷 근거 | 우리 식 매핑 |
|---|---|---|
| 더블클릭으로 밴드 추가, 색상 dot 핸들 | 6개 dot, 선택 밴드는 halo | EQ의 DraggerLayer 패턴 재사용 |
| 밴드 편집 패널 (플로팅) | 번호 배지, 전원, 헤드폰(솔로), shape 8종, freq/depth/q 노브+값, focus, 삭제 | 하단 고정 패널로 재해석 가능 |
| shape 8종 | 1행: HP·저셸프·고셸프·LP / 2행: 벨·와이드벨·노치형·틸트 | BandResponse.h 공유 레시피로 커브 평가 (오디오 필터 아님 — dB 커브만 필요) |
| 밴드 depth ±범위 | −5.3 관측 (음수 = 그 영역 처리 약화) | 밴드별 gainDb, 부호 양방향 |
| 처리 범위 핀 | 좌측 빨강 pill(~100Hz), 우측 초록 pill(~16k) | 전역 HP/LP 경계 — 이 밖은 가중치 0 |
| 합성 커브 2겹 | 굵은 흰 커브(가중치 합성) + 가는 어두운 커브(실시간 리덕션) | 가중치 커브 + 기존 리본을 같은 패널에 오버레이 |
| 밴드 솔로(헤드폰) | 편집 패널 내 | 해당 밴드 가중치만 남기고 delta 청취 |

**DSP 결합점**: bin별 `reduction_db *= weight(f)`. weight = 합성 커브 dB → 배율 변환.
기존 SpectralNode 파이프라인에 bin별 곱 한 줄이 들어갈 자리는 이미 있음 (envelope→exceed 단).

### 2. 사이드바 접힘 메뉴 (stereo / tilt / scale)

| 그룹 | 컨트롤 | 의미 |
|---|---|---|
| stereo | L/R ↔ M/S 토글 | 처리 도메인 선택 |
| | link % | 좌우 리덕션 연동량 (현재 우리는 100% 고정 링크) |
| | focus 슬라이더 | 처리량을 한쪽 채널/사이드로 치우침 |
| tilt | detail / attack / release 각 2버튼 | 파라미터를 주파수축으로 기울임 (고역일수록 빠르게 등) |
| scale | max cut (off/dB) | 밴드당 리덕션 상한 |
| | wet trim (dB) | wet 경로 전용 게인 |

### 3. 크롬 (상단/하단 바)

| 요소 | 비고 |
|---|---|
| 프리셋 브라우저 + 핀 | = APVTS 스냅샷. 가중치 커브 생기면 프리셋 의미가 생김 |
| A/B + a→b 복사 | 상태 두 슬롯 |
| undo/redo | JUCE UndoManager |
| sc + 헤드폰 | 외부 사이드체인 입력 / 사이드체인 청취 |
| quality 드롭다운 + low latency (+자물쇠) | = 우리 "quality modes" 보류 항목 (N 전환, 레이턴시 재보고) |
| misc, 설정, 도움말, 툴팁 줄 | 툴팁 줄: 호버 컨트롤 설명 텍스트 — 저비용 고효과 |
| 그래프 우하단 −9 ▼ | 표시 스케일 선택 |

## 격차 요약 (기능 크기순)

1. **가중치 커브 + 밴드 시스템** — 이번 v2의 본체. EQ 플러그인의 밴드 UI·BandResponse 인프라를 최대 재사용.
2. **stereo 그룹** (M/S, link %, focus) — DSP 신규 (현재 링크 고정).
3. **scale 그룹** (max cut, wet trim) — 소형, DSP 결합 단순.
4. **tilt 그룹** — 파라미터의 주파수 의존화, 중형.
5. **soft/hard** — 측정으로 임계 성격 이미 확보 (soft=상대, hard=절대), 파라미터화만 남음.
6. **크롬** (프리셋/AB/undo/툴팁) — DSP 무관, UI 공사.
7. **quality/low-latency** — 기존 보류 항목과 동일 (N=2048/4096 + 재보고).

## 구현 순서 제안

- **P0**: 가중치 커브 + 밴드 시스템 (더블클릭·dot·편집 패널·범위 핀·bin 곱 결합·솔로).
  프리셋/AB는 이게 있어야 의미가 생기므로 뒤.
- **P1**: scale 그룹 → soft/hard → stereo 그룹.
- **P2**: 크롬(프리셋/AB/undo/툴팁), tilt, quality 모드.

## 관련 픽스 (분석 중 발견)

- bypass 중에도 리본이 계속 그려짐 → 꺼진 것처럼 안 보임. bypass 시 리본 숨김/감쇠 필요.
  (오디오는 정상: out=dry 정확, seam test로 고정됨.)
