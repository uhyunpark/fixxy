# Issues

미해결 문제 기록. 해결되면 삭제하고, 교훈이 남으면 PROJECT.md 함정으로 옮긴다.

## ANALYZER-SCALE — 애널라이저가 EQ 게인 축에 매핑되어 항상 빈 화면
- 증상: Logic/모든 호스트에서 스펙트럼이 전혀 안 그려짐 (오디오는 정상 통과)
- 원인: AnalyzerView가 CoordMap(±30dB, EQ 게인용)으로 빈당 레벨(−40~−90dB)을 매핑 → 전부 바닥 clamp (CoordMap.h:16-17, AnalyzerView.h:107-139)
- 픽스 방향: 애널라이저 전용 dB 창(예: 0~−90dB) 분리 + FFT 창 coherent gain 보상. 실패 테스트 먼저: 브로드밴드 노이즈 → 디스플레이 매핑 통과 후 바닥 위 점 개수 assert (현재 0개)
- 상태: 픽스 적용, Logic 시각 확인 대기 (2026-07-16). AnalyzerView에 CoordMap과 분리된 애널라이저 전용 dB 창(0~-90dB) 추가, AnalyzerEngine 윈도우 스케일링 2/N→4/N으로 Hann coherent gain(0.5) 보상. analyzer_engine_test 확장: old EQ 창(±30dB)에서 0/1705(48kHz)·0/853(96kHz) 포인트만 바닥 위 → 신설 애널라이저 창에서 1705/1705·853/853로 통과. pluginval strictness 5 SUCCESS, auval SUCCEEDED. 소유자가 Logic에서 실제로 스펙트럼이 그려지는지 확인 후 항목 종료.

## EQ-AFFORDANCE — 첫 사용자에게 밴드 생성법이 보이지 않음
- 증상: 기본 상태(전 밴드 off)에서 빈 커브만 보여 고장으로 오인. 더블클릭 제스처/On 체크박스의 시인성 부족
- 픽스 후보: 밴드 0개일 때 힌트 텍스트, 마우스오버 유령 핸들, On 토글 시인성 개선
- 상태: UX 개선 대기. UI 전반 평가(휴리스틱 스크린샷 리뷰) 별도 논의 중
