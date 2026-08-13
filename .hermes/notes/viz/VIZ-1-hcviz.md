# VIZ-1: hcviz — 벤치 타임라인 → GIF/MP4 렌더러

2026-08-13, claw. `tools/viz/` 신설 — 측정(벤치)과 시각화의 완전 분리.
픽셀 스펙 SoT: `/home/ubuntu/tmp/gif-mock-v5-race.png` + `gif-mock-v4-endcard.png` (마진 PAD=44 통일).

## 산출물

| 항목 | 경로 |
|---|---|
| 패키지 | `tools/viz/hcviz/` (theme/timeline/layout/config/render/video/mca_tile/convert/cli) |
| CLI | `tools/viz/bin/hcviz` (`render/still/validate/convert/synth/tile`) |
| 테마 (v5 값 전부) | `tools/viz/themes/ivory.json` — 코드에 디자인 값 하드코딩 없음 |
| JSON Schema | `tools/viz/schema/timeline.schema.json` (2020-12) + `hcviz validate` (schema+시맨틱) |
| 문서 | `tools/viz/README.md` (스키마 문서·race.yaml 레퍼런스·캡처 프로토콜) |
| 데모 구성 | `tools/viz/demo/race-b6.yaml` (합성/실측 표기 주석 포함) |
| **데모 GIF** | `tools/viz/demo/out/race-b6.gif` (1200×598 @25fps, 375프레임, ~0.43MB) |
| **데모 MP4** | `tools/viz/demo/out/race-b6.mp4` (1920×1080 lanczos+pad, x264 crf18) |
| **스틸 3장** | `tools/viz/demo/out/still-t{0.89,3.7,11.9}.png` |
| 벤치 raw (1런) | `/mnt/scratch/bench/viz1/2026-08-13/` (tl-free-claw-1.txt, free-claw-1.{json,err}) |

데모 out/ 산출물은 gitignore (재생성: `./bin/hcviz render demo/race-b6.yaml --out ...` ~10초).
타임라인 JSON·타일 PNG·yaml은 커밋 — 데모는 결정론적으로 재현 가능.

## 사용법 (요약 — 상세는 tools/viz/README.md)

```bash
cd tools/viz
./bin/hcviz render demo/race-b6.yaml --out out.gif --out out.mp4
./bin/hcviz still demo/race-b6.yaml -t 3.7 --out frame.png     # <1s 이터레이션
./bin/hcviz still demo/race-b6.yaml -t endcard --out card.png
./bin/hcviz validate demo/timelines/*.json
./bin/hcviz tile r.0.0.mca --out tile.png
# 테마/레이아웃/뷰는 race.yaml 또는 --theme/--layout/--view/--fps 플래그로 전환
```

race.yaml 예시는 `demo/race-b6.yaml`(race3)·`examples/single-hc.yaml`·`examples/vs2-vanilla-runs.yaml`.

## 실측 검증 (end-to-end)

- FREE 1런 (bench-o2, 20T, avx2/sha-ni, seed 1234567890): **canonical PASS**
  (own-v1 `2eb748…`, exit 0), gen wall 2131.7ms. `HC_BENCH_TIMELINE` 워터폴 v1
  (11,200라인) → `hcviz convert --event serialize` → 1024청크 timeline.json → 렌더 OK.
- 변환 규약: t0 = `setup_end` 마크, wall = `proc_end - setup_end` (= gen wall +
  replay_load ~1%). `--event serialize|deco|chain` — serialize(기본)는 최종 직렬화
  완료(S.t1), FREE 특성상 벽시계 마지막 몇 %에 몰림 (실제 동작 — 버그 아님).
  더 퍼진 리빌을 원하면 deco.
- 게이트 무회귀: ctest 37/37 PASS (build-release; df_x8 스킵은 평소대로 AVX-512 부재).
  viz는 core와 비연결 — core/bench 수정 0 (읽기만).

## 데모의 실측/합성 구분 (race.yaml 주석에도 명시)

- **hyperchunk 패널: 실측** claw 타임라인 (위 1런). `normalize_wall_s: 0.894`로
  벽시계만 B-6 hc-e6 수치로 균일 리스케일 — 리빌 *형상*은 실측, 시계는 환산.
  meta.time_scaled_from_wall_s에 기록됨.
- **바닐라/C2ME 패널: 합성** (scan/wave 패턴, `meta.synthetic=true`) — wall_s
  (11.9/3.7)만 B-6 실측. B-6의 프로브는 0.5s 누적 카운트뿐이라 청크별 데이터 없음.

## 목업 일치 검증

`still -t 3.7` vs v5 목업 구조 프로브 픽셀 대조: 타이틀/라벨/타이머 잉크 bbox·
BG/PAPER/LINE/트랙·액센트 3색 — 전부 동일 좌표·동일 RGB (지형 콘텐츠만 실제 mca
기반이라 상이 — 목업은 fbm 플레이스홀더였음). 엔드카드는 v4 비례 + PAD=44.

## 남은 TODO (후속 태스크)

1. **바닐라/C2ME 청크별 캡처** — 스키마는 `schema/timeline.schema.json`이 계약.
   서버측 프로브 필요 (ChunkStatus full 전이 타임스탬프 로깅 — Fabric 훅 등).
   B-6 방식(0.5s `execute if loaded` 폴링)은 누적 계단이라 부족. 포맷 가정 금지.
   주의: 바닐라는 full 승격이 끝에 몰리는 계단이라 (B-6 TSV: 11.4s까지 0 → 11.9s에
   1024) 실측 리빌도 스캔-블롭이 아닐 수 있음 — 캡처 후 mock 서사와 대조 필요.
2. race4 데모 (REPLAY 타임라인 실측 추가 — 구조는 동작 확인됨).
3. vs2 실사용: 바닐라 run1 vs run2 diff 하이라이트 동작 확인 (B-1 내용-비결정성이
   지형 타일에서 실제로 잡힘 — 육지 청크들에 아웃라인). 결정론 서사용 소재.
4. 오너 카피 승인 대기 사항 없음 — v5/v4 문구 그대로, 추가 카피 없음.

## 함정 (다음 세션용)

- `tools/golden/mca.py`를 importlib로 로드할 때 `sys.modules` 등록 필수
  (dataclass가 모듈명 조회 — 미등록 시 AttributeError).
- hyperchunk 자체 mca는 `build-bench-o3/full_region_r.0.0.mca` (full-region
  게이트 산물, 49MB stored-deflate). build-release에는 없음.
- Heightmap raw 값 = top_y + 1 − min_y → **top 블록 y = v − 65**, v==0 = 빈 칼럼.
  물 깊이 = MOTION_BLOCKING − OCEAN_FLOOR.
- 바닐라 26.2 mca도 zlib(id 2)라 `mca.py` 그대로 파싱됨 (vanilla-1/c2me-1 확인).
- GIF는 palettegen/paletteuse 단일패스(split 필터)로 0.43MB — README 임베드 가능.
- 의존성: 시스템 python3의 Pillow 12.2/numpy 2.4.6/PyYAML 6.0.1 + ffmpeg 6.x —
  신규 설치 0. jsonschema(4.10.3)는 validate에서 있으면 사용, 없어도 동작.
