# GH-1 — 커뮤니티/협업 세팅 완료 노트 (codex-lb 운영 시스템 이식)

2026-08-20. Base: 5c87c5b. Reference: `~/projects/codex-lb/.github` (read-only, 미수정).

## 산출물

| 파일 | 내용 |
|---|---|
| `.github/CONTRIBUTING.md` | dev setup, 레이아웃, 커밋 컨벤션, 머지 게이트 체크리스트, 불변식 테이블 (9행), 로컬-골든 테스트 구분 |
| `.github/SECURITY.md` | private advisory 전용, supported = main, best-effort (솔로) |
| `.github/PULL_REQUEST_TEMPLATE.md` | summary / ADR 관련성 / 게이트 체크리스트 / scope audit / perf 표 (stage+machine+mode 강제) |
| `.github/ISSUE_TEMPLATE/{bug_report,parity_mismatch,feature_request,config}.yml` | YAML 폼 3종 + blank issue 차단 |
| `.github/CODEOWNERS` | `* @Soju06` |
| `.github/dependabot.yml` | github-actions 생태계만, weekly, `chore(ci)` prefix (컨벤션 적합) |
| `.github/workflows/ci.yml` | build-test / no-fma / sanitizers / commitlint / ci-required 집계 |
| `.github/workflows/stale.yml` | 60d stale → 14d close, exempt `pinned`/`security` |
| `scripts/ci/check_commit_msgs.py` | 커밋 컨벤션 검증기 (stdlib Python, Node 없음) |
| `README.md` | `## Contributing` 섹션 추가 (Prior art와 License 사이) — 그 외 무변경 |

## codex-lb에서 이식한 것 vs 배제한 것

**이식 (구조/톤, 내용은 이 프로젝트 현실로 재작성):**
- CONTRIBUTING 구조: ToC → setup → layout → 컨벤션 → 머지 게이트 명시적 체크리스트. codex-lb의 "게이트를 문서에 박제" 접근 그대로, 게이트 내용만 hyperchunk 것 (zero-warning, ctest, no-FMA, sanitizer, parity, golden 불가침, ADR append-only)으로.
- PR 템플릿: 섹션형 + 체크박스 + "해당 없으면 삭제" 가이드 주석. OpenSpec 섹션 → ADR 관련성 섹션으로 치환. Simplicity 섹션 → scope audit(golden/·DECISIONS.md 불가침)으로 치환.
- 이슈 폼: preflight 체크박스 + dropdown + render 지정 textarea 패턴. account_quota(도메인 특화 폼) 자리에 parity_mismatch — 이 프로젝트의 "제품 본질" 폼.
- CI: concurrency cancel, SHA-핀 액션 (`checkout@3d3c42e5…` = v7.0.1, `stale@4391f3da…` = v11, codex-lb와 동일 핀), `persist-credentials: false`, 최소 permissions, job별 timeout, **`CI Required` 집계 job** (branch protection이 단일 체크만 요구하면 되는 패턴; needs-JSON python 판정 그대로).
- stale: actions/stale 워크플로 (probot 아님), 파라미터만 60d/14d·exempt pinned/security로 조정. codex-lb의 needs-info 게이팅(7d/7d)은 솔로+저트래픽에 과하므로 미채택.
- dependabot: github-actions 주간. codex-lb의 uv/bun/docker 생태계는 해당 없음.

**배제 (지시대로):** OpenSpec 워크플로, release-please/beta-release 일체(pre-1.0, 패키지 레지스트리 없음), simplicity-budgets, docs.yml, windows-startup, CodeRabbit 언급, pnpm/uv/Node 툴체인, pr-labeler, CODE_OF_CONDUCT 참조(파일이 없으므로 링크하지 않음), Discussions 링크(활성 미확인 → config.yml에서 생략, 아래 설정 항목 참조).

## CI 설계 — 실측 기반 서브셋

**방법**: fresh clone(추적 파일만) → build → 전체 ctest 실측. 결과 37개 중 24 PASS + 1 SKIP(df_x8, AVX-512 부재) + 12 FAIL(전부 `GOLDEN/SETUP ERROR`). 이후 6-에이전트 조사로 37개 전 테스트의 소스 레벨 근거(file:line) 확보 — 실측과 전건 일치.

**정직 조항 판정**: 서브셋은 비어있지 않다. 25개 중 24개가 실질 검증(rng/perlin/df 골든, reference/ 632 JSON(추적 728파일 중; 나머지는 structure NBT 95 등) 로드 포함 df_cones/df_x4 등 코어 커널 게이트 포함). CI는 의미 있다 — 단 core 패리티의 본체(스테이지 덤프·리전 대조)는 구조적으로 로컬 전용이며, CONTRIBUTING과 ci.yml 주석 양쪽에 명시했다.

**Job 구성** (ubuntu-latest, push main + PR, concurrency cancel):

1. **build-test**: `cmake -S . -B build` (default preset; `-Wall -Wextra -Werror` 무조건) → `ctest -E "$CTEST_EXCLUDE" --no-tests=error`. LABELS가 리포에 없으므로 지시된 폴백대로 `-E` 앵커드 정규식. 실측 드라이런: 25개 선택, 100% PASS (1.7s).
2. **no-fma**: default preset → `--target hyperchunk` → `scripts/check_no_fma.sh`. 호스트 CPU 무관(컴파일은 항상, 실행은 cpuid 뒤). 드라이런: PASS, 80,763 insn 검사, avx512 TU 4,939 insn 비공허 확인.
3. **sanitizers**: `check_sanitizers.sh`가 **전체 ctest를 하드코딩**하므로 스크립트 직접 실행 불가 → 동일 환경 재현(`--preset asan-ubsan`, `halt_on_error=1`, `--no-tests=error`) + 동일 서브셋. 사유를 ci.yml 주석과 CONTRIBUTING에 기록. 풀-스위트 sanitizer는 로컬 머지 게이트로 유지. 드라이런: 25/25 PASS (3.6s).
4. **commitlint**: PR 전용, fetch-depth 0, `--no-merges` + `origin/$GITHUB_BASE_REF..head.sha` 범위(이벤트 base.sha는 PR 생성 시점 동결 — 최신 base 팁 사용)를 `set -euo pipefail` 하에 `check_commit_msgs.py --stdin`으로. 검증기 자체의 `--self-test`를 선행 실행.
5. **ci-required**: needs 집계 (skipped 허용 — push에서 commitlint 스킵).

**함정 처리**: `region_out_roundtrip`/`region_out_residuals`는 `FIXTURES_REQUIRED region_mca` — setup인 `region_out`만 -E로 빼면 소비자 둘이 **fixture 없이 실행돼 입력 부재로 FAIL**한다 (둘 다 gitignored `golden/*.mca`도 읽으므로 어차피 fresh clone 불가). 셋을 함께 제외해야 하며, 그렇게 했다 (드라이런 25개 선택으로 확인). ※ 초안은 "ctest가 -E에도 fixture setup을 자동 재추가"라고 썼으나 적대 리뷰가 실증 반박 — 자동 추가는 `-R` 양성 선택에서만 일어난다 (ctest 3.28.3/4.4.2 재현). 결론(3종 동반 제외)은 불변, 메커니즘만 정정.

### 서브셋 테이블 (test → 데이터 의존 → in/out)

분류: `none`(파일 안 읽음), `tracked-only`(추적 파일만), `gitignored`(로컬 전용 골든 필요), `dep-on-excluded`(제외 테스트의 fixture 의존). 근거 file:line 전문은 조사 저널(세션 로컬)에 있고, 아래 요약으로 충분히 재검증 가능 — 실측(fresh clone ctest)과 전건 일치.

| Test | 분류 | 데이터 | CI |
|---|---|---|---|
| `rng_golden` | tracked-only | golden/rng/xoroshiro·lcg_seed1234567890.txt | IN |
| `arena` | none | — | IN |
| `sched` | none | — | IN |
| `chunk` | none | — | IN |
| `perlin_golden` | tracked-only | golden/rng/perlin_seed1234567890.txt | IN |
| `df_eval` | none | — | IN |
| `md5` | none | — | IN |
| `json` | none | — | IN |
| `nbt` | none | — | IN |
| `nbt_read` | gitignored | golden/structures/*.starts.nbt ×4 | IN* |
| `noise_fork` | tracked-only | golden/rng/fork_….txt | IN |
| `noise_octaves` | tracked-only | golden/rng/octaves_….txt | IN |
| `noise_blended` | tracked-only | golden/rng/router_….txt | IN |
| `noise_normal` | tracked-only | golden/rng/octaves_….txt | IN |
| `biome_zoom` | tracked-only | golden/rng/surface_….txt | IN |
| `surface_unit` | tracked-only | reference/noise+overworld JSON, golden/rng/surface_….txt | IN |
| `beard_math` | tracked-only | golden/rng/beard_compute.txt | IN |
| `router_slots` | tracked-only | reference/** JSON, golden/rng/router_….txt | IN |
| `df_cones` | tracked-only | reference/** JSON | IN |
| `df_x4` | tracked-only | reference/** JSON (AVX2 없으면 exit 77 SKIP) | IN |
| `df_x8` | tracked-only | reference/** JSON (AVX-512 없으면 exit 77 SKIP) | IN |
| `noise_stage` | gitignored | golden/stages/** 04 덤프 | OUT |
| `surface_stage` | gitignored | golden/stages/** 05 덤프 | OUT |
| `carvers_unit` | tracked-only | golden/rng/mth_sin_table.txt | IN |
| `carvers_stage` | gitignored | golden/stages/** 06 덤프 | OUT |
| `features_rng` | tracked-only | golden/rng/jdk_sincos·wgr_gaussian.txt | IN |
| `jdk_log` | tracked-only | golden/rng/jdk_log.txt | IN |
| `features_walk` | gitignored | golden/stages/** 07 덤프 + features-trace | OUT |
| `light_stages` | gitignored | golden/stages/** 08–09 덤프 + features-trace | OUT |
| `spawn_full` | gitignored | golden/{stages,stages-alt}/** 10–11 덤프, features-trace | OUT |
| `region_out` | gitignored | features-trace 페이로드, region-ref/, golden/*.mca | OUT |
| `region_out_roundtrip` | dep-on-excluded | region_out fixture 산출 + golden/*.mca | OUT |
| `region_out_residuals` | dep-on-excluded | region_out fixture 산출 + golden/*.mca | OUT |
| `full_region` | gitignored | region-ref/ 1024청크, region-ref-margin/, structures/ | OUT |
| `free_region_golden` | gitignored | 위와 동일 세트 | OUT |
| `free_region_own` | gitignored | 위와 동일 세트 (check_free_vs_replay.sh 경유) | OUT |
| `sha256` | none | — (KAT 인라인; SHA-NI 부재 시 sw-only) | IN |

\* `nbt_read`는 fresh clone에서 **공허 PASS** — 없는 골든 파일을 per-file skip하고 무조건 exit 0 (`test_nbt_read.c:29-34,63-65`, 설계 의도). CI에 포함하되 ci.yml 주석과 CONTRIBUTING에 공허성을 명시. tests/ 수정은 펜스 밖이라 SKIP_RETURN_CODE 전환은 하지 않음 (후속 후보).

## 커밋 컨벤션 스펙 (관찰 → 형식화)

전 히스토리 187 커밋 분석 (전수가 `type[(scope)][+type[(scope)]]:` 문법에 부합, merge/plain 0건):

```
subject   = component *( "+" component ) ": " text
component = type [ "(" scope ")" ]
type      = feat|fix|perf|test|bench|viz|brand|docs|notes|chore|golden|build|ci   (닫힌 집합)
scope     = [a-z0-9][a-z0-9_./+-]*   (자유 토큰 — 권장 목록만 문서화)
text      = 비어있지 않음, EN/KR 동급 (히스토리 ~52% 한글)
```

판단 근거:
- **scope를 닫지 않은 이유**: 히스토리에 57개 distinct scope, 슬래시 하위스코프(`light/pp`), 괄호 안 `+`(`blocks+cli`), 태스크명(`task10`)까지 존재. enum 강제는 실제 스타일과 충돌.
- **`build`·`ci` 추가** (지시 목록 11종 + 2): `build`는 히스토리 7건으로 실사용 타입, `ci`는 본 태스크의 지시 커밋(`ci: …`)과 dependabot prefix(`chore(ci)`)가 요구. 
- **legacy 타입 미채택**: `wip`(10)·`hygiene`(2)·`refactor`(2)·`diag`(1)·`ref`(1)는 형식화에서 제외, CONTRIBUTING에 legacy로 명기. 전 히스토리 검증 시 이 16건만 NONCONFORMING — 예상대로이며 CI는 PR 범위만 검사하므로 무해.
- 검증기 exempt: `Revert "`, `Reapply "` (도구 생성 포맷; git ≥2.43 revert-of-revert가 Reapply). **Merge는 텍스트 exempt가 아니라 `git log --no-merges`로 상류 필터** — 손으로 쓴 `Merge …` 비머지 subject의 우회를 봉쇄 (적대 리뷰 반영, 아래).

**검증 결과 (지시 항목 3)**: `--self-test` PASS (good 11/bad 8 벡터), **최근 20 실제 subject 전건 PASS** (`--no-merges` 파이프 기준), 합성 bad subject 8종(무접두·대문자 scope·wip 타입·`: ` 누락·빈 text·refactor·빈 subject·수기 Merge) 전건 FAIL.

## 자가 검증 결과

1. 워크플로 YAML: `yaml.safe_load` green ×2, **actionlint PASS** (로컬 설치본).
2. 이슈 폼 YAML: safe_load green ×4, name/description/body 키 확인 (required 필드 bug 6·parity 8·feat 3).
3. 검증기: 위 절.
4. CI 서브셋 실측 교차검증: fresh clone에서 3개 job 명령 그대로 드라이런 — build-test 25선택/100% PASS, no-fma PASS(비공허), sanitizers 25/25 PASS. 근거 테이블 위.
5. diff scope: `.github/**`(신규), `scripts/ci/`(신규), `README.md`(Contributing 섹션만), 본 노트. core/cli/bench/tools/golden/DECISIONS.md/기존 README 섹션 무변경. codex-lb 무변경.

## 리포 설정 필요 항목 (controller 처리용 — 파일로는 불가)

1. **라벨 생성**: `bug`, `enhancement`, `parity`, `triage`, `dependencies`, `stale`, `pinned`, `security`. 이슈 폼·dependabot·stale이 참조 — 없는 라벨은 GitHub이 조용히 무시한다.
2. **Branch protection / ruleset**: `main`에 required check = **`CI Required`** 단일 (집계 job이 나머지를 커버). 
3. **Private vulnerability reporting 활성화** (Settings → Security). SECURITY.md와 이슈 폼 contact 링크가 전제.
4. **Discussions**: 현재 비활성 가정으로 config.yml에서 링크 생략. 활성화하면 `config.yml` contact_links에 추가할 것.
5. Dependabot version updates는 파일만으로 활성 — 별도 설정 불요.

## 적대 리뷰 결과 (커밋 전 4-리뷰어 × 검증 2단)

리뷰어: spec 준수(0건) / 사실검증 / Actions 시맨틱 / 검증기 공격. 제기 12건 → 확정 10건(전부 반영, blocker 0) / 기각 2건(라벨 부재 — controller 체크리스트가 이미 수습). 확정분:

1. **[사실] ctest fixture "자동 재추가" 주장 허위** — `-E` 제외는 존중되고, 자동 추가는 `-R` 양성 선택에서만. 결론(3종 동반 제외) 유효, 메커니즘 서술만 ci.yml/노트에서 정정.
2. **[사실] features_walk/light_stages/spawn_full은 features-trace도 필요** — fresh clone에서 실제로 제일 먼저 부딪히는 파일이 `features-trace/.../03_biomes.biomes.txt`. ci.yml 주석·CONTRIBUTING 표·노트 표 3곳 정정.
3. **[사실] "per-test file:line evidence in note" 과대 포인터** — 노트에는 요약 표. ci.yml 문구를 "evidence table"로 정정.
4. **[사실] legacy 타입 목록에 `ref` 누락** — CONTRIBUTING에 추가.
5. **[사실] "reference/ 728 JSON" 혼동** — 728 추적 파일 = 632 json + 95 nbt + 1 txt. 노트·CONTRIBUTING 정정.
6. **[검증기] `Reapply "…"` (git ≥2.43) false-reject** — exempt 추가 + self-test 벡터.
7. **[검증기] 빈 subject (`--allow-empty-message`) false-accept** — `if s` 가드 제거.
8. **[검증기/Actions] 수기 `Merge …` subject가 텍스트 exempt로 우회** — ci.yml `--no-merges` + exempt에서 `Merge ` 제거 + CONTRIBUTING 로컬 명령 동기화.
9. **[Actions] commitlint 파이프 pipefail 부재** — git log 실패가 빈 stdin으로 조용히 PASS되던 채널. `set -euo pipefail`.
10. **[Actions] `base.sha` 동결 스테일** — PR이 최신 main을 머지하면 동결 base 범위가 메인라인 커밋을 재검사. `origin/$GITHUB_BASE_REF..head`로 교체.

## 함정 기록

- **ctest fixture와 `-E`**: setup만 제외해도 소비자는 그대로 실행돼 fixture 부재로 FAIL한다 (자동 재추가는 일어나지 않음 — `-R` 양성 선택에서만). 어느 쪽이든 소비자까지 함께 제외해야 CI가 선다.
- **`nbt_read` 공허 PASS**: "fresh clone에서 PASS = 추적 데이터로 검증됨"이 아니다. per-file skip + 무조건 exit 0 설계.
- **`check_sanitizers.sh`를 CI에서 그대로 부르면 안 됨**: 전체 ctest 하드코딩 → 골든 부재로 12 FAIL. 환경 재현 + 서브셋으로 대체 (스크립트 수정은 로컬 게이트 약화라 배제).
- **stale/checkout 핀은 dependabot이 관리**: SHA 핀 + `# vX` 주석 포맷 유지해야 dependabot이 갱신 가능.
