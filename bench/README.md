# bench — P2-0 베이스라인 측정 하네스

풀 리전(r.0.0, 1024청크) 생성을 `hyperchunk-verify` 와 동일 시맨틱으로
돌리면서 스테이지별 시간을 계측한다. **매 실행이 canonical 페이로드
해시를 golden/SHA256SUMS 와 대조**하므로, 수치가 찍혔다는 것 자체가
그 빌드가 비트 패리티를 유지한다는 뜻이다 (불일치 = exit 1, 수치 무효).

## 재현 (1줄)

```
bench/run_bench.sh
```

기본: `bench-o2` 프리셋(-O2 + Release + -g), 20스레드, 3회 반복,
seed 1234567890. 결과 JSONL 은 `bench/results/`(gitignore), 요약은 stdout.

변형:

```
bench/run_bench.sh -p bench-o3            # -O3 비교
bench/run_bench.sh -t 1 -n 1              # 단일 스레드 (프로파일 귀속용)
SEED=1234567890 bench/run_bench.sh -n 5   # 반복 수 조정
python3 bench/summarize.py results/A.jsonl results/B.jsonl   # 구성 비교
```

## 계측 모델

- **체인 스테이지** (04 noise / 07 surface / 08 carvers + nc_init/beard):
  워커 스레드별 `CLOCK_THREAD_CPUTIME_ID` 누적 합 → VM steal 에 강건한
  **비중** 판단. 처리량은 체인 페이즈 wall.
- **직렬 페이즈** (09 features, 라이트 08/09/flush/final, postprocess,
  serialize, sha256): 단일 스레드 wall 누적.
- **하네스 오버헤드** (리플레이 manifest 파싱 `replay_load`, postprocess
  마크 대조 `pp_verify`)는 분리 버킷 — 생성 비용(`gen_wall_ns`)에서 제외.
- 프로세스 반복은 셸 루프 (in-process 반복은 BE/틱 레코더 등 상태 누적
  때문에 하지 않는다).

## 주의

- 이 VM(claw)은 CPU 토폴로지 오보고(22c/1t 표기, L3 352MiB) — **절대치는
  참고치**. 스테이지 비중, 핫스팟 순위, 전/후 배율 같은 상대 비교만 유효.
- FP 플래그(`-ffp-contract=off -fno-fast-math`)는 프리셋이 아니라 루트
  CMakeLists 고정 — `HC_OPT_LEVEL` 은 O-레벨만 바꾼다. 새 O-레벨을 쓰려면
  `BUILD=build-bench-oX scripts/parity_gate.sh` 와
  `scripts/check_no_fma.sh build-bench-oX/core/libhyperchunk.a` 를 통과시켜라.
