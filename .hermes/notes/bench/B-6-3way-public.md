# B-6: 공개용 3-way 재실측 — hc-e6 (Zen5) — 바닐라 26.2 vs Fabric+C2ME vs hyperchunk

2026-08-12, hc-e6. HEAD 7aaf6c7 (claw·hc-e6 동일 커밋, working tree clean).
B-1(claw, `B-1-3way-local.md`)의 프로토콜을 **그대로** 재사용, 무대만 교체 — 러너 diff는 경로 4줄뿐 (§2).
측정 원본은 hc-e6 `~/benchmarks/b6-3way/2026-08-12/` (러너 ART). claw로 회수한 보관본이 참조 기준:
`/mnt/scratch/bench/b6-3way/2026-08-12/` (raw-art = 서버 로그·런별 r.0.0.mca·progress·threads·config·
stage-census, hc-results = hyperchunk 런별 jsonl), 요약·러너 `~/benchmarks/b6-3way/2026-08-12/`
(results.jsonl, b6_run.sh, stage-census.txt, threads/progress census).

## 0. 결론 (4계열 표) — 공개 수치

작업: 시드 1234567890, 오버월드, r.0.0 풀 커버 1024청크. 같은 머신(hc-e6). 자바 두 계열은
같은 JDK 25.0.3·같은 힙(-Xms2G -Xmx8G); hyperchunk는 C 바이너리 (JVM 무관, 캐벳 6).
수치는 **3런 중앙값**, 바닐라/C2ME는 0.5s 정밀-폴.

| 계열 | 생성 구간 (n=3) | cps | vs 바닐라 | vs C2ME |
|---|---|---|---|---|
| 바닐라 26.2 dedicated (Worker-Main 31) | **11.9s** (11.9/11.9/11.9) | 86 | 1.00x | 0.31x |
| Fabric 0.19.3 + C2ME 0.4.2-alpha.0.35 (워커 12) | **3.7s** (3.7/3.6/3.7) | 277 | 3.22x | 1.00x |
| hyperchunk REPLAY 20T (골든 순서 재생) | **3.155s** (3.152–3.156) | 325 | 3.77x | 1.17x† |
| hyperchunk FREE 20T (자체 순서) | **0.894s** (0.888–0.900) | 1145 | **13.3x** | **4.14x** |

32T 병기 (SMT 전폭): REPLAY **3.091s** (3.85x), FREE **0.829s** (14.4x vs 바닐라).

- **배율 하한** (경쟁자 = 최소런 − 폴 간격 0.5s − 커맨드-레이턴시 여유 0.1s(2틱, 보수 가정치 —
  실측 아님), hyperchunk = 최대런): FREE vs 바닐라 **≥12.5x**, FREE vs C2ME **≥3.3x**.
  프로브 주입 부하(§2)는 추가 미공제 — 방향은 역시 경쟁자 과대(우리 유리), 캐벳 2.
- † REPLAY vs C2ME 1.17x는 측정-오차 밴드 안 (같은 공제로 하한 0.95x) — **우열 공개 클레임 금지**.
  올바른 공개 표현: "REPLAY는 C2ME-급 속도로 골든과 canonical-일치 재생" (§4의 결정론 대비가 본 클레임).
- claw(B-1) 대비 구도: 바닐라 15.8→11.9s (vCPU 20→32), C2ME 6.5→3.7s, FREE 2.74→0.894s.
  바닐라는 코어 1.6x에 1.33x만 스케일 (의존-파면 한계, B-1 bg1 관측과 정합). 단 FREE 배율
  5.76x→13.3x 확대에는 **무대-종속 기여가 섞여 있다**: hc-e6에서 hyperchunk는 AVX-512 백엔드로
  돈다 (jsonl `isa:"avx512"`; claw는 AVX2) + 32코어 스케일링 격차 + P2-8~11 누적 — 이 배율을
  임의 하드웨어의 기대치로 일반화하지 말 것 (캐벳 13).

## 1. 무대·버전 (공개 인용용 전문)

- **머신**: OCI `VM.Standard.E6.Flex`, 16 OCPU = AMD EPYC 9J45 (Zen5) SMT 32 vCPU, 64GB.
  Ubuntu 24.04.4, kernel 6.17.0-1018-oracle. **캠페인 창(06:31–07:06 UTC, 감도런 포함 ≈35분)
  steal 2틱(~0.02s)** — 스냅샷 3점을 `stage-census.txt`로 보관 (아카이브에서 재검증 가능).
  이 무대의 적격성은 B-4에서 실측: 벤치-중 steal 0틱, instructions 결정론 1.4e-5, 단일-코어
  고정-워크 사이클 밴드 ±3.5%. 런별 loadavg는 results.jsonl에 기록 (t0 기준 0.2–2.9 —
  직전 런 감쇠 꼬리, 캠페인 외 부하 없음).
- **Java**: OpenJDK 25.0.3+9 (Ubuntu 24.04 배포판, `openjdk-25-jre-headless`), 자바 두 계열
  `-Xms2G -Xmx8G` (B-1 동일 규칙), 그 외 JVM 플래그 없음.
- **server-26.2.jar**: sha1 `823e2250d24b3ddac457a60c92a6a941943fcd6a` — claw 원본에서 rsync 후
  hc-e6에서 재검증 일치. Fabric 템플릿 내장 server.jar도 동일 sha1 (비트 동일).
- **Fabric**: loader 0.19.3 (installer 1.1.2 산출물 = B-1 보존 템플릿 그대로 전송;
  fabric-server-launch.jar sha1 `e9bb49fb14d7b64227b1cec0ac96f6fdffa17bf9`). 부팅 로그
  "Loading Minecraft 26.2 with Fabric Loader 0.19.3", 33 mods.
- **C2ME**: `c2me-fabric-mc26.2-0.4.2-alpha.0.35.jar`, sha512 prefix `078618f4e45367ae…`
  (B-1 Modrinth 핀과 일치, 재다운로드 아님 — 버전 고정). 부팅 로그에서 c2me 모듈 21종 로드
  (rewrites-chunk-system, threading-lighting, opts-scheduling, opts-natives-math 등),
  "Global Executor Parallelism: 12" + 스레드 census `c2me-worker` 12개. 기본 설정 (config 무주입).
- **hyperchunk**: HEAD 7aaf6c7, gcc 13.3.0, preset `bench-o2`,
  `bench/run_bench.sh -n 3 -t {20|32} -m {replay|free} -l b6`. 20T = 공식 조건, 32T 병기.
  이 무대에서 noise 커널은 AVX-512 백엔드 (P2-10, 런타임 디스패치 — jsonl `isa` 필드 기록).
- **워커 정책 기록** (전 계열 기본값 그대로): 바닐라 Worker-Main **31** (= cores−1, 32 vCPU에서
  7-캡 없음 재확인), C2ME 자체 풀 **12** (힙-유래 기본 휴리스틱 min(cpus/1.3,(heap−0.5)/0.6)≈12 —
  32코어에서도 **우리가 정한 8G 힙 규칙이 워커 수를 결정**하므로, 이 캡이 유리-조건이 아님을
  hc-e6 위에서 감도런으로 실측: `globalExecutorParallelism=24` 강제(census로 c2me-worker 24개
  확인) 시에도 **3.7s — 무이득** (§3/§4 표기 p24, 아티팩트 라벨 c2me-p24-1). hyperchunk 20/32T 명시.

## 2. 측정 프로토콜 (B-1 §2 동일 — 커맨드 수준 전문)

러너: `b6_run.sh` = B-1 `b1_run.sh`에서 **경로 4줄만** 수정 (BASE `/tmp/b6-3way`,
ART `~/benchmarks/b6-3way/2026-08-12`, HC `~/hyperchunk`, vanilla jar 소스 = `$BASE/libs/server-26.2.jar`).

1. 새 월드 부팅 (server.properties: 골든 캡처 템플릿 — level-seed=1234567890, view/simulation-distance=2,
   difficulty=peaceful, `sync-chunk-writes=false` — 전 계열 동일).
2. quiet: `tick freeze` + gamerule 4종 (random_tick_speed 0, spawn_mobs/advance_weather/advance_time false).
3. 프로브 자가진단: 희생 청크(블록 16000, r.31.31) forceload → `execute if loaded … run say` 응답 확인.
4. **t0**: `forceload add` 4커맨드 (256청크씩 4분면, 골든 캡처와 동일 시퀀스).
5. 폴: 미확인 청크마다 `execute if loaded <bx> 64 <bz> run say HCB_<cx>_<cz>` — 폴 간격 **0.5s**.
6. **t1**: 1024개 전부 확인된 첫 폴. **측정치 = t1−t0**.
7. save-all flush (측정 제외) → `region_stats.py`로 1024×minecraft:full 검증 →
   `compare_regions.py --canonical-hash` + semantic_compare → 월드 삭제 (디스크 규율).

**측정창 정의와 오차 방향** — 바닐라/C2ME 측정치는 단측 과대평가, 성분 셋 전부 같은 방향
(경쟁자 시간을 부풀림 = hyperchunk 배율을 부풀림 = **우리에게 유리**):
① 폴 간격 0.5s — §0 하한에서 전액 공제. ② 커맨드 레이턴시(틱-경계 처리) — 실측 못함,
하한에서 2틱(0.1s) 보수 가정치로 공제. ③ 프로브 자체 부하 — 측정창 안에서 런당 ~7–24회 폴(계열별) ×
최대 1024개 `execute if loaded` 실행이 서버에 주입됨 — 공제 없음 (캐벳 2). 감도 실측:
C2ME 0.25s-폴 추가런 = **3.5s** (0.5s-폴 3.6–3.7 대비 −0.1~−0.2s) — ①의 실제 크기가 이
수준임을 확인 (②③은 양쪽 폴 조건에 공통이라 이 차분으로는 안 잡힘).

**hyperchunk 측정창**: 내부 계측 `gen_wall` (생성+serialize+sha256 포함, 폴 오차 없음).
**제외 항목도 대칭으로 공개**: 프로세스 전체(proc_wall, FREE 20T 중앙값 1.68s)에서 setup ~0.77s
(참조/골든 로드·DF 컴파일 — 자바의 boot 6.0–7.0s 제외와 대칭), REPLAY의 골든-순서 로드
~25ms, pp_verify(검증 전용)를 뺀 것이 gen_wall. 원자료 jsonl에 proc_wall_ns·setup_ns가
그대로 있어 재검산 가능. 자바 서버들은 스트리밍 저장(비동기 mca 쓰기)이 측정창 안에 겹침 —
억제 불가한 정상 동작으로, 양쪽 다 "생성+직렬화 포함, 부팅/셋업·최종 flush 제외"의 대략
동치 스코프 (B-1 §2 논거 그대로).

## 3. "같은 작업" 근거 (전 런)

- region_stats: 자바 8런(본 시리즈 6 + 감도 2) 전부 **1024×minecraft:full** (프로브 t1 판정과
  독립인 디스크 검증).
- r.0.0.mca 크기 (골든 8,675,328 B 대비): **본 시리즈 6런 −0.24%~+0.24%**;
  감도런 2건 −0.61%(0.25s-폴)/−0.52%(p24) — 전 8런 ±0.7% 이내.
- 스폰 선생성: 전 런 pre-t0 디스크 census 동일 — r.0.0 내 144청크 (full 1 + structure_starts 128
  + biomes 7 + carvers 5 + initialize_light 3). 부팅 창에서 실행 = 측정창 밖 → **경쟁자에게만
  유리한 머리출발** (배율 방향은 보수). 로드-상태 프로브 PRE census는 0/1024 (부팅 후 전부 언로드).
- hyperchunk 쪽 동일-작업 증거: 1024청크 전부를 측정창 안에서 처음부터 생성 (선생성 이득 없음),
  매 런 canonical 게이트 판정 (§4). **REPLAY와 FREE의 스테이지 래더 카운트가 전 12런 동일**
  (chain 1681 / decorated 1461 / postprocessed 1165 / emitted 1024 — jsonl `chunks` 필드):
  FREE가 작업을 건너뛰는 게 아니라 같은 래더를 다른 순서로 소화함. FREE의 판정 상수
  `#canonical-own-v1`은 자체 고정-순서 출력의 기록이므로 그 자체로는 자기-참조 — 바닐라-계급
  논거(바닐라 features는 단일-스레드 실행기라 비결정 축은 순서뿐, FREE는 충돌쌍을 직렬화한
  하나의 유효한 총순서를 고정 — P2-3 노트 §1.4)가 근거이고, 골든-순서와의 canonical-대조 증명(아래 정의)은
  REPLAY 열이 담당.

**"canonical-일치"의 정의** (공개 문서에 필수): raw .mca 파일의 sha256이 아니라, 저장-시각
필드(루트 `LastUpdate`, mca 헤더 타임스탬프 테이블)와 섹터 배치·압축 프레이밍을 제외한
**청크 페이로드 전 바이트**의 정규화 해시 (`tools/golden/compare_regions.py`). 골든의 헤더
타임스탬프는 캡처 당시 벽시계라 raw-비트 일치는 어떤 시스템도 원리적으로 불가.

## 4. 재현성 — 계열별 런간 diff (결정론 내러티브의 공개 근거)

기준 도구: `tools/golden/semantic_compare.py` (내용 비교), canonical hash (위 정의).
골든 canonical `a5963205…3c24`, FREE own-v1 `2eb7485b…84d6` (golden/SHA256SUMS, hc-e6에서 재검증).

| 계열 | 런간 canonical | 런간 내용 diff (semantic) |
|---|---|---|
| 바닐라 26.2 (3런) | 3런 3해시 (전부 상이) | 1v2 **591**, 2v3 **587**, 1v3 **581** /1024 |
| C2ME (5런 = 본 3 + 감도 2) | 5런 5해시 (전부 상이) | 1v2 **804**, 2v3 **771**, 1v3 **789**, 3v0.25s폴 **758**, 1vp24 **785** /1024 |
| hyperchunk REPLAY (20T×3 + 32T×3) | **6/6 == 골든 `a5963205…`** | canonical-일치 (diff 0) |
| hyperchunk FREE (20T×3 + 32T×3) | **6/6 == own-v1 `2eb7485b…`** | canonical-일치 (diff 0) |

참고 (런간 아님): 바닐라 run1 vs **골든** = 552/1024 — 골든이 "정본 출력"이 아니라 기록된
한 세션임을 재확인 (B-1 §4 결론).

판정: B-1(claw)의 발견이 hc-e6에서 재확인 — **바닐라(581–591/1024)·C2ME(758–804/1024)는 같은
시드·같은 명령 시퀀스로도 런마다 다른 월드를 낸다. hyperchunk만 REPLAY(골든 canonical-일치)·
FREE(자체 순서 canonical-일치) 모두 결정적, 두 스레드 수(20/32T)에서 각자 기준 해시와 일치 12/12 (모드별 6/6).**
기전은 B-1 §4 (데코가 이웃 청크의 현재 블록을 읽는 구조 → 이웃 간 완료 순서가 내용에 새겨짐).
C2ME diff가 바닐라보다 큰 것도 claw와 동일 경향 (764 → 758–804).

## 5. 캐벳 (전량)

1. **클라우드 VM** — 단독 점유 보장 아님. 단 캠페인 창(06:31–07:06Z) steal 2틱(~0.02s,
   `stage-census.txt` 3점 스냅샷 보관), B-4에서 이 무대의 적격성 실측 (§1). "베어메탈"은 아님.
2. **폴링 단측 오차 3성분** (§2 ①②③) 전부 **우리에게 유리한 방향** — ①은 하한에서 전액,
   ②는 보수 가정치(2틱)로 공제, ③(프로브 부하)은 공제 없음. 공개 자막에는 중앙값 사용 +
   하한(12.5x/3.3x) 각주 권고. 0.25s-폴 감도런으로 ①의 크기 확인 (−0.1~−0.2s).
3. **스폰 선생성 144청크** (§3) — 경쟁자에게만 유리 (보수 방향).
4. 바닐라 3런 전부 11.9s는 0.5s 폴 양자화의 소산 — 진값은 (11.4, 11.9] 어딘가. 런간 순수 분산이
   0이라는 뜻이 아님.
5. **C2ME는 알파 채널** (26.2 대응 최신 = 0.4.2-alpha.0.35, 릴리스 채널이 상시 alpha). 기본 설정.
6. 런마다 새 JVM/새 월드 — 3런 모두 동일한 콜드-스타트 조건 (런간 캐리오버 없음; 콜드-스타트
   분산은 중앙값으로 흡수). JIT 웜업은 부팅+희생청크 생성으로 일부만 상쇄 — 자바 계열에 불리할
   수 있는 방향이나, 서버 신규 기동 시나리오가 곧 실사용. C 바이너리인 hyperchunk엔 해당 없음
   (JDK·힙 조건도 자바 계열에만 적용).
7. **워커 수 조건**: hyperchunk 공식 20T (32 hw-thread 중 20; 32T 병기 시 FREE 0.894→0.829s =
   wall −7.3%). C2ME 12워커는 **우리 힙 규칙(-Xmx8G)의 힙-유래 캡** — 유리-조건 의심을 hc-e6
   위에서 직접 봉합: 24워커 강제 감도런(census 확인) **3.7s 무이득** (§1). 바닐라 31워커는
   순정 기본값.
8. REPLAY vs C2ME (1.17x)는 오차 밴드 내 (§0 †, 하한 0.95x) — 우열 클레임 금지.
9. 측정창 스코프: 내부 계측(hyperchunk) vs 외부 폴(자바) — §2의 동치 근사 논거. 완전 동일
   계측점은 원리적으로 불가 (서버는 생성 완료 신호를 노출하지 않음).
10. 자바 서버 무튜닝 (공정성 규율 — 힙만 B-1 규칙으로 통일). `sync-chunk-writes=false`는 전
    계열 동일 (골든 캡처 조건). 워커-감도런(p24)은 헤드라인이 아니라 캐벳 7의 검증 전용.
11. **부팅/셋업 제외의 대칭**: 자바 boot 6.0–7.0s 측정 제외 ↔ hyperchunk setup ~0.77s
    (참조 로드·DF 컴파일) 제외 (§2). end-to-end 체감(프로세스 기동 포함)은 양쪽 다 이보다 크다
    — jsonl `proc_wall_ns`(FREE 20T 중앙값 1.68s)로 재검산 가능.
12. 골든 하네스 프로토콜 자체의 자기-재현성은 B-1과 동일하게 이번에도 검증 범위 밖.
13. **무대-종속 배율**: hc-e6에서 hyperchunk는 AVX-512 백엔드 (claw는 AVX2). 13.3x/4.14x는
    이 무대(Zen5 32vCPU)의 수치이지 하드웨어-불변 상수가 아님 — 공개 시 무대 명기 필수.

## 6. GIF·공개 자막 권고 수치

- **주 수치 (자막)**: 바닐라 **11.9s** → C2ME **3.7s** → hyperchunk **0.89s** (FREE 20T).
  조건 한 줄: "같은 머신(Zen5 32vCPU)·같은 시드·같은 1024청크(r.0.0)·자바는 JDK 25".
- **배율 자막**: "**~13× vs 바닐라, ~4× vs C2ME**" (각주: 측정-오차 공제 하한 12.5× / 3.3×,
  무대 명기 — 캐벳 13). 32T까지 쓰면 "최대 14.4× (0.829s)" — 단 20T가 공식임을 병기.
- **REPLAY 자막**: "재현 모드(REPLAY)도 3.2s — C2ME-급 속도에서 골든과 canonical-일치".
  ("C2ME보다 빠름" 표현 금지 — 캐벳 8. "비트-일치"라는 표현을 쓰려면 §3의 canonical 정의를
  각주로 반드시 동반.)
- **결정론 자막**: "같은 시드를 두 번 돌리면: 바닐라 **581+/1024** 청크가 다름, C2ME **758+/1024**,
  hyperchunk **0** — 12/12런 (모드별 6/6, 각자 기준 해시 일치)". (숫자는 관측 최소값 —
  과장 없는 방향.)
- cps 표기 원하면: 86 → 277 → **1,145 chunks/s** (FREE 20T).

## 7. 재현

```
# hc-e6 (Zen5 무대). /tmp/b6-3way 스테이징: b6_run.sh + libs/server-26.2.jar (sha1 823e2250…)
#   + libs/c2me-fabric-mc26.2-0.4.2-alpha.0.35.jar (sha512 078618f4…) + fabric-template/ (B-1 보존분)
HC_POLL_S=0.5  bash /tmp/b6-3way/b6_run.sh vanilla <runid>
HC_POLL_S=0.5  bash /tmp/b6-3way/b6_run.sh c2me <runid>
HC_POLL_S=0.25 bash /tmp/b6-3way/b6_run.sh c2me <runid>-p025          # 폴-감도런
HC_POLL_S=0.5 HC_C2ME_CFG=/tmp/b6-3way/c2me-p24.toml bash /tmp/b6-3way/b6_run.sh c2me p24-<n>  # 워커-감도런
cd ~/hyperchunk && bench/run_bench.sh -n 3 -t 20 -m replay -l b6      # -t 32 / -m free 조합 동일
python3 tools/golden/semantic_compare.py <a.mca> <b.mca>              # 런간 diff
```

결과 원본: results.jsonl (자바 8런 전 레코드 — gen_s/boot_s/워커/로드/canonical/presave census),
hyperchunk `bench/results/20260812T06*-b6*.jsonl` (런별 gen_wall_ns·setup_ns·proc_wall_ns·isa·
chunks 래더·canonical·pass), stage-census.txt (steal 3점·버전 전량), 서버 로그·mca 전량 (§ 서두 경로).
사전 게이트: hc-e6 REPLAY 프리체크 PASS + claw ctest 스팟 (rng/perlin/free_region_golden 3/3).
