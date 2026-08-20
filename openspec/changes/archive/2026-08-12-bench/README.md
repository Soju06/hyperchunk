# bench — archived notes (2026-08-12)

hyperchunk 청크 생성기의 성능·결정론 검증 벤치 캠페인(B-1~B-6) 노트 묶음. 로컬 3-way 벤치(B-1: 바닐라 15.8s / Fabric+C2ME 6.5s / FREE 2.74s, 5.76x)에서 출발해 워터폴·프로파일 분석(B-2), 이론 하한 모델(B-3), hc-e6(Zen5) 무대 적격성과 최적화 종결 판정(B-4 OPEN → B-5 CONDITIONAL, K_chain 3.25–4.58 형식화)을 거쳐 공개용 재실측(B-6)으로 마무리했다.
최종 공개 수치(hc-e6, 3런 중앙값): 바닐라 11.9s / C2ME 3.7s / REPLAY 3.155s / FREE 0.894s — FREE는 바닐라 대비 13.3x(측정-오차 공제 하한 ≥12.5x), C2ME 대비 4.14x(하한 ≥3.3x). 결정론은 hyperchunk만 12/12런 canonical-일치(바닐라 581–591/1024, C2ME 758–804/1024 청크가 런마다 상이).
hosting.md는 벤치 무대 조사 기록 — Hetzner Cloud CCX는 AVX-512 부재로 부적격 판정, 베어메탈은 Robot 전용서버 필요.
관련 커밋: a8d6237..4cb17e9 (2026-08-06~2026-08-12)
