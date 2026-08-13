# hcviz — bench timeline → GIF/MP4 race renderer

Measurement and visualization are fully separated: benches emit raw per-chunk
captures; hcviz re-draws them from small `timeline.json` files in seconds,
locally, with every visual constant coming from a theme file. Nothing here
touches core/bench code.

Pixel spec SoT: `/home/ubuntu/tmp/gif-mock-v5-race.png` (race frame) and
`gif-mock-v4-endcard.png` (endcard, margins unified to PAD=44). The shipped
`themes/ivory.json` encodes those values.

## Dependencies

python3 with Pillow + numpy + PyYAML (jsonschema optional, used by `validate`
when present), and `ffmpeg` on PATH. All present on claw's system python.

## Quickstart

```bash
cd tools/viz
./bin/hcviz render demo/race-b6.yaml --out demo/out/race-b6.gif --out demo/out/race-b6.mp4
./bin/hcviz still  demo/race-b6.yaml -t 3.7 --out /tmp/frame.png   # feedback loop
./bin/hcviz still  demo/race-b6.yaml -t endcard --out /tmp/card.png
./bin/hcviz validate demo/timelines/*.json
```

`still -t` accepts seconds, `end` (final race frame), or `endcard`.
`--theme/--layout/--view/--fps` override the race.yaml on any render/still.

## timeline.json (input schema)

One file per benchmarked system. Formal schema: `schema/timeline.schema.json`
(JSON Schema 2020-12); `hcviz validate` checks it plus semantics (duplicate
chunks, `t_done_ms` past the wall).

```json
{
  "system": "vanilla-26.2",
  "display_name": "Vanilla",
  "stage": "hc-e6/zen5",
  "wall_s": 11.9,
  "chunks": [{"cx": 0, "cz": 0, "t_done_ms": 8123.4}],
  "meta": {"seed": 1234567890, "workers": 31, "probe_interval_ms": 50}
}
```

- `t = 0` is **generation start** (boot/setup excluded); the panel timer
  freezes at `wall_s`.
- `cx/cz` are absolute chunk coords; the race region defaults to r.0.0
  (`cx,cz ∈ [0,31]`) and can be overridden via `meta.region {x0,z0,w,h}`.
  Grid × `panel.chunk_px` must equal `panel.size` (32 × 11 = 352).
- `meta.synthetic: true` + `meta.pattern` mark generated (non-measured)
  timelines; the `synth` subcommand always sets them.

### hyperchunk converter (real capture)

The bench's `HC_BENCH_TIMELINE=<path>` env dumps a waterfall v1 file
(absolute CLOCK_MONOTONIC ns; see `bench/hyperchunk_bench.c` wf_dump). One
FREE run on claw:

```bash
HC_BENCH_TIMELINE=/mnt/scratch/bench/viz1/<date>/tl-free-1.txt \
  ./build-bench-o2/bench/hyperchunk-bench --seed 1234567890 --region 0 0 \
  --repo . --threads 20 --policy free --isa avx2 --sha ni \
  > /mnt/scratch/bench/viz1/<date>/free-1.json
# exit 0 + "pass":true in stdout JSON == canonical PASS (golden/SHA256SUMS own-v1)

./bin/hcviz convert tl-free-1.txt --out hc.json \
  --system hyperchunk-free --display-name hyperchunk --stage hc-e6/zen5 \
  --result free-1.json                        # --result adds seed/canonical/pass
```

`--event` picks what "chunk done" means:

| event       | source        | meaning                                    |
|-------------|---------------|--------------------------------------------|
| `complete`  | max(own `C.w5`, deco `E.t1` ±1) (default) | last substantive block write — terrain + every feature/structure placement that can reach the chunk (deco write window is source-enforced ±1; light/serialize never write blocks). Excludes the pp drain's sparse shape-fixes (no heightmap/tile effect) |
| `strict`    | max(`complete`, pp `P.t1` ±1) | bit-final content incl. pp shape-fixes. Needs P records (VIZ-2+ captures). pp is a row-major serial sweep over every target ±1, so this collapses reveal into the pp window — audit use |
| `serialize` | `S.t1`        | chunk in final serialized form; FREE serializes in a tail burst, so most reveal lands in the last few % of the wall |
| `deco`      | own deco `E.t1` | own decoration done — ignores neighbor deco writes into this chunk (debug/comparison) |
| `chain`     | `C.w5`        | noise/surface/carvers done — earliest; terrain shape final, tree/ore deco pending |

Timeline `t0 = setup_end` mark, `wall_s = proc_end − setup_end` (gen wall +
replay-load, ~1% above the bench JSON's `gen_wall_ns`).

### Vanilla / C2ME capture — procedure fixed, execution is a follow-up

The B-6 probe (`b6_run.sh`: per-chunk `execute if loaded … run say HCB_<cx>_<cz>`
against a forceloaded region, driver-clock confirmation) already yields
per-chunk completion windows; it only needs a high-frequency poll (50–100 ms)
and a per-chunk TSV. Full procedure, script draft, observer-load validation
gate and error marking (`meta.probe_interval_ms`, one-sided overestimate):
`capture/vanilla-c2me-probe.md`. The probe measures FULL-status promotion —
for vanilla that is genuinely an end staircase (B-6 TSV: 0 until 11.4 s), so
expect the real reveal to differ from the synthetic scan narrative. The
schema above is the contract.

## race.yaml (render composition)

See `demo/race-b6.yaml` (commented) and `examples/`. Fields:

- `theme` — theme file (JSON/YAML), relative to the yaml.
- `layout` — `race3` | `race4` | `single` | `vs2` (vs2 outlines chunks whose
  tile content differs between the two panels once both are revealed).
- `view` — `reveal` (chunk appears at `t_done_ms`) | `heat` (completion-time
  ramp paper→accent).
- `fps`, `hold_s` (all-done hold), `header.title/sub`.
- `endcard` — `enabled`, `hold_s`, `wordmark`, `headline` (`auto` =
  `baseline_wall / fastest_wall` → "N.N× faster"), `sub`, `baseline_panel`.
- `panels[]` — `name`, `accent` (named theme accent or RGB), `timeline`,
  `tile` (png) or `mca` (auto-rendered+cached into `tiles/`),
  `normalize_wall_s` (uniformly rescale a real timeline's clock to a wall
  measured elsewhere — shape stays measured; recorded in meta).

Timer format follows the mock: `0.89s` under 1 s, else one decimal (`3.7s`).
Race runs at 1× real time; each panel freezes its timer/progress at its wall.

## Terrain tiles

`hcviz tile r.0.0.mca --out tile.png` renders a 352×352 top view: Anvil
parsing reuses `tools/golden/mca.py`; the surface comes from serialized
`Heightmaps` (MOTION_BLOCKING top, OCEAN_FLOOR for water depth), colored by a
muted category palette (`terrain` block in the theme) with x-gradient slope
shading. Works on vanilla, C2ME, and hyperchunk region files.
B-6 mca inputs live in `/mnt/scratch/bench/b6-3way/2026-08-12/raw-art/`;
hyperchunk's own full region: `build-bench-o3/full_region_r.0.0.mca`.

## Performance

Full demo (375 frames, 1200×598, GIF+MP4): ~10 s on claw. Stills: <1 s.
