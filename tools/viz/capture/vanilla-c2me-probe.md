# 바닐라/C2ME 청크별 실측 타임라인 캡처 절차 (VIZ-2 §4 — 문서 확정, 실행은 후속)

B-6 러너(`~/benchmarks/b6-3way/2026-08-12/b6_run.sh`)의 프로브를 그대로 고빈도화한다.
기존 프로브는 이미 청크별이다: 미확인 청크마다
`execute if loaded <cx*16> 64 <cz*16> run say HCB_<cx>_<cz>` 를 콘솔 FIFO 로 밀고
서버 로그의 `HCB_*` 라인으로 확인한다 (POLL_S 기본 3s, 누적 카운트만 TSV 기록).

## 측정 시맨틱

- 프로브가 잡는 것은 **FULL-status 승격** (서버가 청크를 쓸 수 있게 되는 외부 관측
  가능한 유일한 완료 시점). 바닐라는 full 승격을 끝에 몰아넣는 계단이므로
  (B-6 TSV: 11.4s까지 0 → 11.9s에 1024) 실측 리빌도 계단일 것이 예상된다 —
  그것이 정직한 그림이다. sub-full 단계 진행은 모드 훅 없이는 관측 불가.
- t_done(청크) = 그 청크의 HCB 라인이 처음 확인된 폴 사이클의 드라이버 시각
  (`now()` 기준, t0 = forceload 직전). 오차는 **단방향 과대**:
  ≤ POLL_S + 명령 틱 정렬(50ms) + 콘솔/로그 왕복 지연.

## 변경점 (b6_run.sh 폴 루프 — 초안)

```bash
POLL_S="${HC_POLL_S:-0.1}"          # 3 → 0.1 (50~100ms 급)
CHUNKS_TSV="$ART/chunks-$MODE-$RUNID.tsv"   # 신규: 청크별 완료 기록
: > "$CHUNKS_TSV"
cp /dev/null "$RUN_DIR/.done_prev"
while true; do
    comm -23 "$RUN_DIR/.all" "$RUN_DIR/.done" | while IFS=_ read -r _ CX CZ; do
        printf 'execute if loaded %d 64 %d run say HCB_%d_%d\n' \
            $((CX*16)) $((CZ*16)) "$CX" "$CZ"
    done >&3
    sleep "$POLL_S"
    { grep -oE 'HCB_[0-9]+_[0-9]+' "$LOG" || true; } | sort -u > "$RUN_DIR/.done"
    TREL=$(awk -v a="$(now)" -v b="$T0" 'BEGIN{printf "%.3f", a-b}')
    comm -13 "$RUN_DIR/.done_prev" "$RUN_DIR/.done" | while IFS=_ read -r _ CX CZ; do
        printf '%s\t%s\t%s\n' "$TREL" "$CX" "$CZ"      # 청크별 t_done
    done >> "$CHUNKS_TSV"
    cp "$RUN_DIR/.done" "$RUN_DIR/.done_prev"
    ...   # 기존 누적 TSV/종료 판정 그대로
done
```

변환: `chunks-*.tsv` → timeline.json (스키마는 기존 계약 그대로) —
`t_done_ms = TREL*1000`, `wall_s = 공개 wall` (t1 시각; 프로브 과대오차 포함),
`meta.synthetic` 없음, **`meta.probe_interval_ms = POLL_S*1000`**,
`meta.probe = "forceload+if-loaded full-status"`, `stage = "hc-e6/zen5"`.

## 관측자 부하 검증 게이트 (필수)

프로브 자체가 피측정 서버 메인 스레드에 부하를 준다 (사이클당 미확인 청크 수만큼
명령 파스 — 최악 1024개/100ms ≈ 메인 스레드 수 % 수준). 고빈도 캡처를 공개 수치와
연결하기 전에:

1. 같은 날 인터리브로 POLL_S=3 (B-6 레퍼런스) 3런 vs POLL_S=0.1 3런.
2. wall(t1) 중앙값이 B-6 대역 안에서 일치해야 캡처 유효. 어긋나면 0.2 → 0.5 로
   백오프, 또는 적응형(확인 수가 변하는 동안만 고빈도, 정체 구간은 0.5s)으로 전환.
3. 타임라인에는 실제 사용한 POLL_S 를 meta 에 그대로 기록.

## 오차 표기 (타임라인 + 프레임)

- 타임라인: `meta.probe_interval_ms` (스키마에 이미 존재) + 단방향 과대오차임을
  노트에 병기.
- 프레임: 렌더러의 하단 마이크로 캡션(현재 synthetic 패널 표기)을 probe 패널로
  일반화 — `meta.probe_interval_ms` 가 있는 패널은 `probe ±0.1s` 꼬리를 붙인다.
  (실측 타임라인이 생기기 전까지는 죽은 코드라 구현하지 않음 — 캡처 태스크에서
  캡션 로직과 함께 붙일 것. 카피 최소 원칙: 캡션은 계속 한 줄.)

## 주의 (B-6 계승)

- 서버 벤치 프로토콜 함정(B-1 노트)·quiet 규율(tick freeze + gamerules)·프로브
  self-check(희생 청크)·PRE census — b6_run.sh 그대로 유지.
- `say` 는 확인된 청크가 프로브 집합에서 빠지므로 청크당 ~1-2회만 발생 — 로그
  폭주 없음. 명령 flood 는 위 검증 게이트가 잡는다.
- 무대 표기: hc-e6 실행이면 `hc-e6/zen5`. 다른 무대와 섞지 말 것.
