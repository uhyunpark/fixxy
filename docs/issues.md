# Issues

미해결 문제 기록. 해결되면 삭제하고, 교훈이 남으면 PROJECT.md 함정으로 옮긴다.

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
