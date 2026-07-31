# A0 — Order-manifest design decision (Task 9-pre synthesis)

Synthesis of A1–A6 (all claims bytecode-verified there; this note only combines
them). Decides WHAT the manifest records, WHERE the mod hooks, and WHY that is
sufficient for ADR-007 Tier-2 replay.

---

## 1. Granularity decision: per-chunk total order is the minimal sufficient record

The record is **one line per features-stage chunk application**, ordered by the
moment the decoration seed is set. Finer granularity (chunk, step,
feature-index) carries zero additional information:

1. **Within-chunk iteration is a pure function of (registries, seed, chunk pos,
   pre-features chunk state).** A2: the step×feature iteration order comes from
   `FeatureSorter` (memoized; comparator-/insertion-ordered containers only, no
   identity-hash iteration) plus registry `byId` order for structures; the RNG
   is re-seeded via `setFeatureSeed(decoSeed, index, step)` before *every*
   structure and *every* placed feature, so RNG consumption cannot leak between
   items. Nothing inside one chunk's `applyBiomeDecoration` call depends on
   scheduling.
2. **A chunk application is atomic on one thread.** A1: the whole
   `generateFeatures` body (primeHeightmaps → `applyBiomeDecoration` →
   `Blender.generateBorderTicks`) is synchronous — no interleaving *within* a
   chunk is possible, so there is nothing sub-chunk to record.
3. **Chunk applications are already totally ordered by vanilla.** A5: all
   worldgen step bodies of a dimension run inline on a single per-dimension
   `ConsecutiveExecutor` ("worldgen"), one task at a time, with happens-before
   between consecutive bodies — even at `max.bg.threads=255`. The manifest is
   therefore not a linearization we impose; it is the actual execution history.
   (And if Mojang ever parallelizes disjoint regions: disjoint ⇒ commute ⇒ any
   recorded serialization still replays bit-exactly.)
4. **The only cross-chunk channels are captured by that order.** A4: features
   write blocks/fluids into the 3×3 write window (`blockStateWriteRadius(1)`,
   `ensureCanWrite` silently drops outside), incrementally update the 4 FINAL
   heightmaps (pure function of blocks), append pending ticks / postprocessing
   / dummy-BE NBT, and read live neighbor ProtoChunks. Which values a chunk
   *reads* from a neighbor is decided exactly by whether that neighbor was
   decorated earlier — i.e. by the per-chunk order and nothing else.

The residual hazard A4 flags — warn-only block/height reads at distance 2..8
("Detected unsafe terrain read during worldgen") — is checked empirically per
golden run: the harness fails if that string appears in the server log (§4).

## 2. Hook point: `WorldgenRandom#setDecorationSeed` @RETURN, gated by `generateFeatures`

The RNG-determining instant for a chunk's decoration is the
`setDecorationSeed(levelSeed, cx*16, cz*16)` call at the top of
`ChunkGenerator.applyBiomeDecoration` (A2). Everything decorative that follows
derives from its return value. So the manifest line is emitted from an
`@Inject(at = @At("RETURN"))` on
`net.minecraft.world.level.levelgen.WorldgenRandom#setDecorationSeed(JII)J`,
which also hands us the *actual* seed vanilla computed (recorded in hex as a
consistency check — A3: the value is a pure function of (levelSeed, block
origin), so replay recomputes it and must match).

`setDecorationSeed` alone is not a sufficient discriminator: A2/A3 census found
a second caller, `NoiseBasedChunkGenerator.spawnOriginalMobs` (SPAWN stage,
Legacy-backed delegate). Therefore the capture is **armed** by a HEAD inject on
`ChunkStatusTasks#generateFeatures` (the sole worldgen decoration path, A1) —
which also supplies the dimension filter via `ctx.level()` and the expected
chunk pos — and **disarmed** on capture (one-shot) plus at generateFeatures
RETURN (belt-and-braces: an unconsumed armed record is written as an ERROR line
so a silent miss cannot masquerade as a clean manifest). The seed capture
cross-checks `blockX>>4 == armed.chunkX && blockZ>>4 == armed.chunkZ` before
consuming.

Recording scope: **every features application in the dump dimension** (spawn
area + dependency ring included), not just the dump grid — ring-chunk
decorations write into grid chunks (A4), so the replay needs their order too.

## 3. What Task 9's C replay must consume

- Decorate chunks in manifest `seq` order (single-threaded replay is a valid —
  indeed the actual — order).
- Snapshot semantics: each golden `07_features` dump is the chunk state at *its
  own* decoration completion (StageDumper runs in the step future). So a
  chunk's 07 dump = own decoration + spill-ins from every neighbor with a
  *smaller* seq only. The replay must snapshot at the same point, not after all
  decorations.
- Recompute `setDecorationSeed` per A3 (Xoroshiro128++ delegate; WorldgenRandom
  `nextLong` burns two xoro outputs) and assert equality with the manifest's
  seed column before placing anything.
- A3 cross-check vectors for seed 1234567890: chunk(0,0) → `0x499602d2`
  (degenerate: bx=bz=0 ⇒ deco == levelSeed), chunk(1,0) →
  `0x9e04405fc44f2242` (-7060597322161768638), chunk(0,1) →
  `0x11ff96e2ffdc5be2` (1296893547007966818).

## 4. Manifest format (v1)

Emitted as `golden/stages/seed<seed>/order.manifest` (and the same for
`stages-alt`). `#` header lines, then data lines in append order:

```
# hyperchunk features order manifest v1
# target_version <SharedConstants.getCurrentVersion().name()>   # "26.2"
# seed <level seed, decimal>
# dimension <dump dimension id>
# grid center=(<cx>,<cz>) radius=<r>            # dump grid, for reference; manifest records ALL chunks in the dimension
# hook net.minecraft.world.level.levelgen.WorldgenRandom#setDecorationSeed(JII)J@RETURN armed-by net.minecraft.world.level.chunk.status.ChunkStatusTasks#generateFeatures
# columns seq chunkX chunkZ decorationSeedHex thread nanos
<seq> <chunkX> <chunkZ> <decorationSeed as 16-digit lowercase hex, two's complement> <thread name> <System.nanoTime()>
```

- `seq` starts at 0, strictly increasing; assigned and appended under one
  global lock in the same critical section, so file order == seq order.
- `thread` records interleaving evidence (with `max.bg.threads=1` expect
  `Worker-Main-1` everywhere); `nanos` gives coarse timing for analysis only.
- ERROR lines (`# ERROR ...`) mean the harness detected an armed-but-unconsumed
  application; their presence fails the run.

## 5. Why not alternatives

- **`@WrapOperation` (MixinExtras)**: available at runtime (A6: loader 0.19.3
  nests mixinextras-fabric 0.5.4) but not on the mod's compile classpath;
  plain `@Inject` needs no new toolchain pieces and the arm/consume pair is
  equally precise.
- **Hooking `ChunkStep.apply` (existing dump mixin)**: fires at stage
  *completion*, after the RNG outcomes are already determined; also fires for
  all stages. Wrong instant, weaker semantics.
- **Recording (chunk, step, feature-index)**: strictly redundant per §1; would
  bloat the manifest (~thousands of lines per chunk) and imply the C side must
  *follow* a sub-chunk script instead of asserting its own deterministic
  iteration — the opposite of what Tier 2 proves.
