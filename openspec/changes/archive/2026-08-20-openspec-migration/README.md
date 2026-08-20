# openspec-migration — archived change (2026-08-20)

GH-2: 리포 SSOT를 openspec으로 전환한 작업의 완료 노트. DECISIONS.md의 ADR 9건을 7개 capability spec/context + project.md로 완전 이관(적대 검증 missing 0)하고, .hermes 노트·플랜 95파일을 changes/archive 15개 폴더로 git mv했으며, AGENTS.md를 신설하고 README/CONTRIBUTING/템플릿 참조를 치환했다.
검증: openspec validate --specs --strict 7/7 PASS, 클레임 펜스·링크 103개 무결, ctest 37/37 green (코드 무변경).
이 전환 자체가 첫 archived change다. 상세 매핑 테이블·검증 원문은 GH-2-openspec-migration.md.
관련 커밋: 93675a7..HEAD (2026-08-20)

## Language migration (2026-08-20, same day)

All live openspec docs (project.md, 7 capability spec.md+context.md pairs)
and AGENTS.md were translated from Korean to English for the public repo.
Archived change notes under changes/archive/ stay in Korean as historical
records. The context.md "Decision history" append-only rule was exempted
once for this language migration; entry anchors (ADR numbers, dates,
Superseded-by markers) were preserved verbatim. Verification: zero Hangul
in live docs, openspec validate --specs --strict 7/7, claim-fence literals
intact, all relative links resolve, 9/9 ADR references present.
