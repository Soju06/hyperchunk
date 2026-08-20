# hyperchunk brand spec

Owner-approved 2026-08-19 (banner crop v17). Source of truth for visual identity
across README, demo GIF, banners, and any future site/social assets.

## Principle

**The measured artifact is the visual.** Terrain art is always real generator
output (demo tiles from B-6/VIZ-4/VIZ-5 measured runs), never stock or
hand-drawn. UI chrome stays neutral; color comes from the data.

## Wordmark

- Lowercase `hyperchunk`, always. Never capitalized, never camel-cased in prose
  or lockups (code identifiers follow code conventions: `libhyperchunk`, `hc_`).
- Font: **Space Grotesk** (weight ~640 variable) for wordmark and headlines.
- Body/captions: **Inter** (Regular; SemiBold for emphasis).
- Tagline (canonical): `Bit-exact vanilla Minecraft worldgen in pure C`.

## Mark

2×2 chunk grid with the last cell snapping in at a half-cell offset — "the
moment a chunk lands in exactly the right place" (= bit-exact). Geometry
(canonical, from `assets/brand/logo-mark.svg`): three settled cells at
(0,0), (0,1), (1,1); the fourth at (1+0.5, -0.5) in cell units; cell:gap ratio
28:8; corner radius 4/28 of cell. Single color via `currentColor` — ink on
paper themes, paper on dark.

## Color tokens

| Token | ivory theme | dark theme | Role |
|---|---|---|---|
| `bg`     | `#F6F2EA` (246,242,234) | `#0E0C0B` (14,12,11) | Background paper |
| `ink`    | `#28241F` (40,36,32)    | `#EEEBE4` (238,235,228) | Wordmark, mark, headlines |
| `sub`    | `#68625A` (104,98,90)   | `#A09A92` (160,154,146) | Taglines, captions |
| accents  | terrain palette (see `tools/viz/themes/ivory.json`) | same | Data panels only |

Ivory is the primary theme — GIF, endcard, and banner share it so the artifacts
read as one set. Dark exists for `prefers-color-scheme: dark` surfaces only.

## Banner (README hero)

- 1600×410. Identity block left (mark 60px at x=92, wordmark 70px,
  tagline Inter 24), terrain art right, art width 790px.
- Art = demo tile `tools/viz/demo/tiles/hyperchunk-free.png` (measured r.0.0),
  nearest-neighbor zoom ×7, crop (0,160), water fraction ~1/3 land-dominant.
- Fade: 200px smoothstep alpha ramp starting 60px into the art, composited as
  a **linear-light lerp to the exact bg color** (sRGB-space fades produce a
  gray haze band — do not regress this).
- Art grade: saturation ×1.12, water lifted ×1.10+8, then 5% (ivory) / 12%
  (dark) blend toward bg.
- Generator (pixel-exact reproducible): `tools/viz/brand_banner.py`
  (`--theme ivory|dark`). Fonts expected in `~/.local/share/fonts/brand/`.

## Voice & copy rules

- Engineering register; no marketing superlatives.
- Claims follow the public-bench caveats in
  `.hermes/notes/bench/B-6-3way-public.md` §0 (REPLAY vs C2ME phrasing,
  stage attribution for multipliers).
- Visual surfaces keep text minimal: one-line captions, measured numbers only.

## Files

- `assets/brand/banner.png`, `assets/brand/banner-dark.png` — README hero pair
- `assets/brand/logo-mark.svg` — mark, `currentColor`
- `assets/race-b6.gif` — demo GIF (all-measured, VIZ-5)
- `tools/viz/brand_banner.py` — banner generator
