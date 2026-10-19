# Issues

미해결 문제 기록. 해결되면 삭제하고, 교훈이 남으면 docs/architecture.md 함정으로 옮긴다.

## SPECTRAL-HOP-BURST — hop 콜백에 FFT 작업 집중, 소버퍼에서 예산 ~90%
- 실측(2026-08-13, -O3): hop 콜백 ~500–650µs, 스테이지 분해 결과 82%가 옥타브-median 포락선(FFT는 11%)
- 처방 적용(2026-08-14): ① median을 증분 정렬 창으로 교체(비트 동일, 커널 3.3–3.45×), ② FFT를 vDSP double로 교체(4.8×, 픽스처 정밀도 향상). hop 중앙값 397→197µs, 버퍼 32 대기 p99.9 = 예산의 20.4%
- 상태: 코드 반영 완료, Logic 실기 확인 대기. 확인되면 이 항목 삭제

## SPECTRAL-TRANSPARENCY — 화이트 노이즈 투명성 0.37dB (soothe3 실측 −0.14dB)
- Stage 3 대조 실험으로 확정: 탄도(시간 스무딩)로는 원리적으로 못 닫는 갭 — 이상적 무한 평균기도 0.335dB 바닥. 원인은 Stage 2 포락선 추정(프레임별 median)의 Jensen 편향
- 처방(선택): 포락선 추정 자체를 시간축으로 스무딩 (Stage 2 개선 항목). 0.37dB는 청감상 미미해 v1 비블로킹
- 상태: 기록만 (2026-08-11). C++ 이후 실믹스 청취에서 거슬리면 착수

## EQ-AFFORDANCE — 첫 사용자에게 밴드 생성법이 보이지 않음
- 증상: 기본 상태(전 밴드 off)에서 빈 커브만 보여 고장으로 오인. 더블클릭 제스처/On 체크박스의 시인성 부족
- 구현됨, Logic 시각 확인 완료 (2026-07-18):
  - P0-1 빈 상태 힌트: 밴드 0개일 때 커브 중앙에 "Double-click to add a band" 문구 (DraggerLayer.h paint) — 밴드 하나라도 켜지면 즉시 사라짐
  - P0-2 마우스오버 유령 핸들: 빈 영역 위에서 findNearestDisabledBand + 더블클릭과 동일한 freq/gain 매핑으로 반투명(alpha 0.35) 핸들 미리보기 (DraggerLayer.h mouseMove/updateGhost) — 클릭을 가로채지 않음, 실제 핸들 위/mouseExit에서 사라짐
  - P0-3 슬로프 비활성화: 선택 밴드 타입이 Lowpass/Highpass가 아니면 slopeBox.setEnabled(false) (EqEditor.h). 밴드 선택 전환·타입 콤보 변경·호스트 오토메이션(APVTS 리스너 + AsyncUpdater로 메시지 스레드 마샬링) 세 경로 모두 반영
  - P1-1 애널라이저 dB 축 라벨: 우측 가장자리에 0/-30/-60/-90 라벨 (AnalyzerView.h paint) — 좌측 ±24 EQ 게인 축과 위치·색상으로 구분, 그리드라인은 추가하지 않음
  - P1-2 On 체크박스 대비: 선택 밴드 색상으로 ToggleButton 틱/아웃라인 컬러링 (EqEditor.h rebuildAttachments), 밴드 선택 전환마다 갱신
- 남은 항목 (P1-3/P2, 보류): 드롭다운 ● 마커, 상시 아이콘 스트립, 값 직접 입력, PRE/POST 범례, CoordMap ±30 vs 그리드 ±24 정리
- 상태: 5개 P0/P1 픽스 확인 완료. 단 소유자 피드백 "전반적인 미감은 여전히 불만족" — 어포던스(기능)는 해소, 비주얼 디자인 방향은 별도 과제 (frontend-design 스킬로 디자인 스펙 → JUCE 구현 흐름 후보)

## DESIGN-DIRECTION — 장기 비주얼 아이덴티티: docs/design-reference.md ("Family" 스타일)
- 소유자 결정(2026-07-22): 플러그인 제품군의 장기 디자인/색감 기준은 docs/design-reference.md — 웜 라이트(크림 캔버스 + 스톤 표면), 헤어라인 인셋 보더(그림자 거부), 플랫, Inter 유틸리티 타이포, 의미론적 액센트
- 현 FixxyLookAndFeel(다크+앰버)과 충돌 — 단 의미 규칙(쿨=신호/웜=동작)은 유지한 채 토큰 값 교체로 전환 가능하게 설계돼 있음. 액센트 후보 매핑: 동작=Ember Orange(#ff3e00)
- 적응 필요: 데이터 시각화(스펙트럼/미터) 색을 라이트 캔버스 대비로 재조정. 일러스트는 빈 상태/프리셋 순간에만
- 시점: 미정("먼 일"). 착수 시 HTML 목업 → 토큰 교체 → 데이터 색 재검증 순
- **확정(2026-08-10): 다크 모드 폐기, 전 제품군 라이트로.** 목업 비교(docs/mockups/spectral-ui-mockup.html) 후 소유자 결정. 스펙트럴 신제품이 라이트로 데뷔 → EQ/컴프는 FixxyLookAndFeel 토큰 교체로 후속 이관. 라이트용 데이터 색 확정: 신호=딥 애저 #0b61c4, 웨이팅 커브=잉크 #1b1b1b, 리덕션/동작=엠버 #ff3e00 (목업에서 검증됨)
