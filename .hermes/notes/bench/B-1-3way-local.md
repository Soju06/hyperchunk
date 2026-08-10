# B-1: 3-way 로컬 벤치 — 바닐라 26.2 vs Fabric+C2ME vs hyperchunk

2026-08-10, 이 VM (claw). HEAD 31048b3. 측정·분석 원본: `~/benchmarks/b1-3way/2026-08-10/`
(results.jsonl, 서버 로그, 스레드 census, 런별 r.0.0.mca 15개 — vanilla run1만 보존-패치 이전이라 없음, 러너 b1_run.sh).

## 결론 (3-way 표)

작업: 시드 1234567890, 오버월드, r.0.0 풀 커버 1024청크 (+서버 요구 마진). 같은 머신,
같은 JDK 25.0.3, 같은 힙(-Xms2G -Xmx8G). 수치는 정밀-폴링 런(0.5–1s) 중앙값.

| 서버 | 생성 구간 | cps | vs 바닐라 | vs FREE |
|---|---|---|---|---|
| 바닐라 26.2 dedicated (워커 19) | **15.8s** (15.2–16.8, n=3) | 65 | 1.00x | 5.8x 느림 |
| Fabric 0.19.3 + C2ME 0.4.2-alpha.0.35 (워커 12) | **6.5s** (5.4–8.4, n=3) | 158 | 2.43x | 2.4x 느림 |
| hyperchunk REPLAY (20스레드, 골든 순서 재생) | **8.00s** | 128 | 1.97x | — |
| hyperchunk FREE (20스레드, 자체 순서) | **2.74s** | 373 | **5.76x** | 1.00x |

- hyperchunk 수치는 재측정 없이 `bench/results/20260810T024300Z·024313Z` (gen_wall_ns 3런 중앙값) 그대로.
- **FREE는 C2ME 대비 2.37x, 바닐라 대비 5.76x.** REPLAY(비트-재현 모드)는 바닐라보다 1.97x 빠르지만
  C2ME보다는 느리다 (6.5 vs 8.0s) — 단, C2ME는 아래 §4처럼 런마다 다른 월드를 낸다는 조건부.
- 참고: 바닐라 `-Dmax.bg.threads=1` 34.3s → 기본(워커 19) 대비 스케일링이 2.2x뿐 (의존성-파면 한계).
  FREE는 bg.threads=1 바닐라 대비 12.5x. (bg1 런들에서 Worker-Main 3스레드 관측 — 플래그가
  gen 풀을 조이지만 이름 매칭 기준 완전 단일은 아님.)

## 1. 모드 스택 선택 근거 (실가용성 조사, 2026-08-10)

계획 "Paper+C2ME+Chunky"는 성립 불가(C2ME는 Fabric 전용)여서 태스크 규칙대로 조사 후 결정:

- **C2ME 26.2 존재** → 1순위 **Fabric+C2ME** 채택. Modrinth `c2me-fabric`: `0.4.2-alpha.0.35+26.2`
  (2026-08-02, 게임버전 `["26.2"]`, 의존성 없음). sha512 prefix `078618f4e45367ae…` 대조 일치.
  C2ME는 릴리스 채널이 상시 alpha임을 병기. 부팅 로그에서 21개 모듈 전부 활성 확인
  (rewrites-chunk-system, threading-lighting, opts-scheduling, opts-natives-math 등).
- Fabric loader 0.19.3 + installer 1.1.2 = 리포 핀 버전 그대로 (fetch_fabric.sh).
- **Chunky 미사용**: 1.5.3이 26.2 지원하나 Fabric API 의존이 추가됨 + 프리젠 트리거가 서버마다
  달라지는 것을 피하려고, 세 시스템 모두 골든 캡처와 동일한 **forceload 프로토콜**로 통일 (§2).
- **Paper 미병기**: fill.papermc.io에 26.2 빌드 존재 확인했으나 C2ME 존재 시 Fabric+C2ME가
  태스크 명시 1순위라 제외. (원하면 동일 러너에 모드만 바꿔 추가 가능.)

## 2. 측정 프로토콜

러너: `~/benchmarks/b1-3way/2026-08-10/b1_run.sh <vanilla|c2me> <runid>` — 세 시스템 공통.

1. 새 월드로 부팅 (server.properties는 make_golden_unified.sh 템플릿 + `sync-chunk-writes=false`).
2. quiet: `tick freeze` + gamerule 4종 (골든 캡처와 동일).
3. 프로브 자가진단: 희생 청크(블록 16000, r.31.31 — r.0.0 무영향) forceload 후
   `execute if loaded … run say` 응답 확인. (26.2는 부팅 후 스폰 청크를 유지하지 않아
   `~ ~ ~` 프로브는 항상 무음 — 이것 때문에 희생 청크가 필요.)
4. **t0**: `forceload add` 4커맨드 (256청크씩, 골든 캡처와 동일).
5. 폴링: 미확인 청크마다 `execute if loaded <bx> 64 <bz> run say HCB_<cx>_<cz>` — 조건 false면
   무음, full-로드되면 로그에 마커. 폴 간격 0.5–3s (런별 기록).
6. **t1**: 1024개 전부 확인된 첫 폴. **측정치 = t1−t0** (단측 과대평가 ≤ 폴 간격 + 명령 레이턴시).
7. save-all flush (**측정 제외**) → region_stats로 1024×minecraft:full 검증 → canonical/semantic hash → 월드 삭제.

측정창 의미: 서버들은 스트리밍 저장(생성 중 비동기 mca 쓰기)이 창 안에 겹침 — 억제 불가한
정상 동작. hyperchunk gen_wall은 serialize+sha256 포함, 최종 디스크 flush 제외. 대략 동치 스코프.

## 3. 전체 런 (results.jsonl)

| mode | run | gen_s | poll | load(1m,t0) | 비고 |
|---|---|---|---|---|---|
| vanilla | 1–3 | 18.5 / 18.6 / 18.5 | 3s | 2.9 / 5.6 / 13.3 | 3s 폴 과대 포함 |
| vanilla | 4 / 5 / 6 | **16.8 / 15.8 / 15.2** | 1 / 0.5 / 0.5s | 2.5 / 7.3 / 6.2 | 최종치 산출 |
| c2me | 1 | (25.8) | 3s | **18.1** | 부하 폭주 + 최초부팅(boot 26s) 이상치, 제외 |
| c2me | 2 / 3 | 9.4 / 9.4 | 3s | 5.6 / 9.3 | 3s 폴 과대 포함 |
| c2me | 4 / 5 / 6 | **6.5 / 5.4 / 8.4** | 1 / 0.5 / 0.5s | 3.7 / 6.6 / 8.0 | 최종치 산출 |
| c2me | p19-1 | 6.7 | 1s | 4.3 | 병렬도 19 튜닝 — 이득 없음 |
| vanilla | bg1 | 34.3 | 1s | 2.8 | -Dmax.bg.threads=1 |
| vanilla | bg1grid / bg1grid2 | 35.5 / 36.6 | 1s | 6.1 / 7.3 | bg1 + 골든 시퀀스(3×3 그리드 선생성) |

- 바닐라 워커 19 (Worker-Main, 기본값 = cores−1, 20코어에서 7-캡 없음 확인).
  C2ME는 자체 풀 12 c2me-worker (기본 휴리스틱 min(cpus/1.3, (heap−0.5)/0.6)≈12).
  병렬도 19로 올려도 6.7s — 12가 병목이 아님.
- 폴 해상도가 수치를 좌우 (3s폴 c2me 9.4 → 0.5–1s폴 5.4–8.4, 중앙 6.5): 배율은 정밀-폴 런만 사용.

## 4. canonical 대조 — 불일치, 그리고 그 자체가 결과

기준: 골든 `golden/seed1234567890_r.0.0.mca` canonical = `a5963205…3c24`
(= REPLAY 게이트 canonical, 재확인 완료).

| 대조 | canonical | semantic (내용) |
|---|---|---|
| 바닐라(멀티스레드) vs 골든 | 불일치 | **521/1024 청크 상이** |
| 바닐라 run5 vs run6 (자기 자신) | 불일치 (4런 4해시) | **555/1024 상이** |
| 바닐라 bg1 vs 골든 | 불일치 | 446/1024 |
| 바닐라 bg1grid vs 골든 (골든 시퀀스 재현) | 불일치 | 450/1024 |
| **바닐라 bg1grid vs bg1grid2 (동일 프로토콜 반복)** | 불일치 | **418/1024 상이** |
| c2me run5 vs run6 | 불일치 (6런 6해시) | **764/1024 상이** |
| hyperchunk REPLAY ×3 | **a5963205… == 골든, 3/3 동일** | — |
| hyperchunk FREE ×3 | 2eb7485b… 3/3 동일 (자체 고정 순서) | — |

판정: **바닐라 26.2는 `-Dmax.bg.threads=1` + 동일 명령 시퀀스로도 자기 월드를 재현하지 못한다**
(bg1grid 반복 418/1024 상이). 차이의 형상(광석↔돌, moss, 하이트맵, block_ticks 길이,
BlockLight)은 데코 단계가 이웃 청크의 현재 블록을 읽고 3×3 창에 쓰는 구조와 정합 —
이웃 간 데코 완료 순서가 내용에 새겨지고, 그 순서는 타이밍 의존. 리포 자체 문서
(semantic_compare.py docstring, make_golden_unified.sh의 nosave 근거)와 일치하는 결론.

따라서 "바닐라 .mca == 골든" 형태의 비트-증명은 원리적으로 성립 불가 — 골든은
바닐라의 "정본 출력"이 아니라 **기록된 한 세션**이고, 그걸 비트-재현하는 건 REPLAY뿐.
"같은 작업"임은 구성으로 증명: 동일 시드·버전(같은 server-26.2.jar)·동일 forceload,
전 런 1024×minecraft:full, r.0.0.mca 크기 전부 8.66MB±0.3%.

3-way 관점 요약: **경쟁자 둘 다 런마다 다른 월드를 낸다 (바닐라 555, C2ME 764/1024).
hyperchunk만 REPLAY(골든 비트 일치)·FREE(자체 순서 비트 일치) 모두 결정적.**
(캐벳: 골든 하네스 프로토콜 자체(nosave+덤프 동기화)의 자기-재현성은 이번에 검증하지 않음.)

## 5. 캐벳

- 공유 VM (AMD 5900X 기반 20 vCPU, 62GB, 다른 에이전트 작업 상주·nice+). 런별 1분 load를
  표에 병기. 바닐라는 load 2.5–13에서 15.2–18.6s로 둔감, c2me는 분산 큼 (5.4–8.4s) —
  c2me 워커가 스레드 우선순위 4(기본)로 도는 것과 정합. 절대치는 참고용, 산출물은 배율.
- 폴링 단측 과대 ≤ 폴 간격: 최종치는 0.5–1s 런. 진짜 값은 표기치보다 최대 그만큼 작을 수 있음
  (배율이 우리 쪽에 보수적).
- 스폰이 r.0.0 내부: 부팅 시 스폰 선택이 full 1개 + proto 143개를 선생성 (pre-t0 디스크 census).
  경쟁자들만 ~0.1% 이득 — 역시 보수적 방향.
- 런당 1회 워밍업 없는 새 JVM/새 월드 (n=3 반복이 사실상 워밍업 통제). JIT 웜업은
  희생-청크 생성으로 일부 상쇄, C 바이너리인 hyperchunk엔 해당 없음.
- C2ME는 알파 채널 최신(26.2 릴리스 +6주). 기본 설정 사용 (p19 튜닝 무이득 확인).
- hyperchunk와 서버의 측정창은 §2 스코프에서 동치 근사 (양쪽 다 최종 flush 제외).

## 6. 재현

```
HC_POLL_S=0.5 bash ~/benchmarks/b1-3way/2026-08-10/b1_run.sh vanilla 7
HC_POLL_S=0.5 bash ~/benchmarks/b1-3way/2026-08-10/b1_run.sh c2me 7          # /tmp/b1-3way/{libs,fabric-template} 필요
HC_BG1=1 HC_GRID_FIRST=1 bash …/b1_run.sh vanilla bg1grid3                   # 검증 런 변형
python3 tools/golden/semantic_compare.py golden/seed1234567890_r.0.0.mca <mca>
```
