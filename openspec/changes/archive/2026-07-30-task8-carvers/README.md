# task8-carvers — archived notes (2026-07-30)

MC 26.2 카버 스테이지(06_carvers) 포팅을 위한 서버 바이트코드 정찰 노트 묶음 — applyCarvers 오케스트레이션(A1), WorldCarver 베이스(A2), cave/canyon 카버(A3/A4), CarvingContext+aquifer(A5), CarvingMask/ProtoChunk/heightmap(A6), RNG/Mth/value providers(A7)를 javap 기반 1:1 재구성.
salvage 런의 13,023-diff 근본 원인은 RNG가 아니라 replaceables 태그의 exact-match 조회가 deepslate/water 등 property-less 이름을 놓친 것 — `[`-prefix 블록 단위 매칭(tag_mark_block) 한 수정으로 전체 발산 종결.
최종 검증: 18/18 ctest, 9/9 청크 0 diff, check_no_fma·ASan+UBSan PASS. 뮤테이션 프로브 16건으로 게이트 커버리지를 실측했고(canyon 경로는 seed 1234567890에서 100% golden-blind), 적대 리뷰로 7건 인-트리 수정 — shipped-data parity 이탈 0 확인.
관련 커밋: b268dc0..e9c4a61 (2026-07-30)
