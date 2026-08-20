# gh-setup — archived notes (2026-08-20)

GH-1 커뮤니티/협업 세팅 완료 노트 — codex-lb 운영 시스템의 .github 구조를 hyperchunk 현실로 재작성해 이식했다: CONTRIBUTING/SECURITY/PR 템플릿/이슈 폼 3종(bug·parity_mismatch·feature)/CODEOWNERS/dependabot/ci.yml/stale.yml(60d/14d) + stdlib Python 커밋 컨벤션 검증기.
CI는 fresh clone 실측 기반 서브셋: 전체 37개 테스트 중 추적 데이터만으로 도는 25개 선택 — core 패리티 본체(스테이지 덤프·리전 대조)는 구조적으로 로컬 전용임을 CONTRIBUTING과 ci.yml에 명시했다.
전 히스토리 187 커밋 분석으로 커밋 컨벤션을 13-타입 닫힌 집합으로 형식화했고, 커밋 전 적대 리뷰 제기 12건 중 10건 확정·전부 반영(blocker 0).
관련 커밋: 3dfd313 (2026-08-20)
