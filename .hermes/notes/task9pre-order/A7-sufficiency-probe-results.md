# A7 — Sufficiency probe evidence (Task 9-pre, 2026-07-31, final bundles)

Captured output of `python3 tools/golden/order_probe.py golden/stages/seed1234567890 golden/stages-alt/seed1234567890`
against the committed bundles (primary manifest sha256 ad98232487d09f9d..., alt cfa4760cd81591e3...).
Interpretation in tools/golden/NOTES.md §"Sufficiency probe results".

```
A: golden/stages/seed1234567890 — 81 applications, threads ['Worker-Main-1']
B: golden/stages-alt/seed1234567890 — 81 applications, threads ['Worker-Main-1', 'Worker-Main-2', 'Worker-Main-3', 'Worker-Main-4']

[1] decoration seeds: 81 common chunks, 0 seed mismatches
    orders differ (first divergence at seq 0: A=(-1, -1) B=(-1, 1))

[1b] order.snapshots coherence
    A: 99 snapshots, 07 invariant holds, torn by stage: none
    B: 99 snapshots, 07 invariant holds, torn by stage: {'11_full': 1, '09_light': 4}

[2] grid chunks: 07_features dump vs distance-2 order prefix
    c.-1.-1: dump same, prefix DIFF -> ok (order diff, same outcome)
    c.-1.0: dump DIFF, prefix DIFF -> OK
    c.-1.1: dump DIFF, prefix DIFF -> OK
    c.0.-1: dump DIFF, prefix DIFF -> OK
    c.0.0: dump DIFF, prefix DIFF -> OK
    c.0.1: dump DIFF, prefix DIFF -> OK
    c.1.-1: dump DIFF, prefix DIFF -> OK
    c.1.0: dump DIFF, prefix DIFF -> OK
    c.1.1: dump DIFF, prefix DIFF -> OK

[3] concrete spill-over evidence
    c.-1.0: 2191 differing blocks, 434 on chunk borders
    neighbors decorated before it in A: [(-1, -1)]
    neighbors decorated before it in B: [(-1, -1), (-1, 1)]
    flipped neighbors: [(-1, 1)]
      local (11, -46,15)  world (  -5, -46,  15)  A=minecraft:deepslate[axis=y]  B=minecraft:deepslate_redstone_ore[lit=false]
      local (12, -46,15)  world (  -4, -46,  15)  A=minecraft:deepslate[axis=y]  B=minecraft:deepslate_redstone_ore[lit=false]
      local (11, -45,15)  world (  -5, -45,  15)  A=minecraft:deepslate[axis=y]  B=minecraft:deepslate_redstone_ore[lit=false]
      local ( 5, -28,15)  world ( -11, -28,  15)  A=minecraft:deepslate[axis=y]  B=minecraft:deepslate_iron_ore
      local ( 6, -27,15)  world ( -10, -27,  15)  A=minecraft:deepslate[axis=y]  B=minecraft:deepslate_iron_ore
      local ( 5, -20,14)  world ( -11, -20,  14)  A=minecraft:deepslate[axis=y]  B=minecraft:tuff
      local ( 6, -20,14)  world ( -10, -20,  14)  A=minecraft:deepslate[axis=y]  B=minecraft:tuff
      local ( 7, -20,14)  world (  -9, -20,  14)  A=minecraft:deepslate[axis=y]  B=minecraft:tuff

PROBE PASSED: bundles coherent with per-chunk-order-only variation
```
