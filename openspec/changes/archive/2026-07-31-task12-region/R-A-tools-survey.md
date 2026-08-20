# R-A: tools/golden survey for Task 12 (region serialization gate)

All paths repo-relative to /home/ubuntu/projects/hyperchunk. All line numbers from files as of
commit 4bebe8e (working tree clean). Everything below verified by reading the code and by live
runs against the local golden region on 2026-07-31, except items marked UNVERIFIED.

## 1. tools/golden/mca.py — public API (195 lines, read-only library, no CLI)

Docstring line 10: "Not a writer. The golden baseline is captured from vanilla, never
synthesized." Python side only reads; the C side (Task 12) is the only writer.

### Region reading

- `SECTOR = 4096` (line 20).
- `ChunkEntry` dataclass (lines 25–33), fields exactly:
  - `index: int` — 0..1023, `index = (cx & 31) + (cz & 31) * 32` (comment line 27)
  - `x: int` — chunk x within region = `i % 32`; `z: int` = `i // 32` (line 72)
  - `timestamp: int` — epoch seconds from the header timestamp table (offset SECTOR + i*4, `>I`)
  - `compression: int` — 1 gzip, 2 zlib, 3 none, 4 lz4, 127 custom (line 31)
  - `raw: bytes` — compressed payload as stored
  - `payload: bytes` — decompressed NBT payload
- `read_region(path) -> dict[int, ChunkEntry]` (line 54): returns only PRESENT chunks
  (offset-table entry with sector_off==0 and sector_count==0 is skipped, lines 62–65).
  Files shorter than 2*SECTOR return `{}` (line 58).
- Anvil framing it decodes (lines 66–70) — this is the framing our C writer must emit:
  - `off_raw = >I at i*4`; `sector_off = off_raw >> 8`; `sector_count = off_raw & 0xFF`
  - chunk base = `sector_off * SECTOR`; `length = >I at base` (length COUNTS the compression
    byte); `comp = blob[base+4]`; compressed data = `blob[base+5 : base+4+length]`
    i.e. `length - 1` bytes.
- `_decompress` (line 36): id 2 → `zlib.decompress`, 1 → `gzip.decompress`, 3 → raw,
  4 → `lz4.block.decompress` (only if pip `lz4` installed, RuntimeError otherwise),
  anything else → RuntimeError. 26.2 default is zlib id=2 (docstring line 6); the golden
  region is 100% id=2 (verified, see §6).

### NBT parsing — `parse_nbt(payload) -> (root_name, root_dict)` (line 139)

Root tag must be 10 (Compound) else RuntimeError (lines 143–144). Root name is read as a
normal length-prefixed string; in the golden chunks it is the EMPTY string, i.e. payloads
begin `0x0A 0x00 0x00` (verified on chunk (3,5)).

Value representation (`_Reader.payload`, lines 109–136):

| tag | python value | notes |
|---|---|---|
| 1/2/3/4 Byte/Short/Int/Long | `int` (signed, `>b/>h/>i/>q`) | tag WIDTH IS LOST in parsed form |
| 5 Float | `float` (f4 widened to double) | indistinguishable by type from Double |
| 6 Double | `float` | |
| 7 ByteArray | `bytes` (raw) | NOT a list — distinguishable from Int/LongArray |
| 8 String | `str`, utf-8 with `errors="replace"` | mojang modified-UTF-8 quirks would be silently replaced |
| 9 List | python `list` | element tag `etag` read then i4 count; **etag is DISCARDED** — an empty list is a plain `[]` with no record of its element type (empty TAG_End list vs empty TAG_Compound list are indistinguishable after parse). Count n=0 → `[]`. |
| 10 Compound | python `dict` | keys inserted in FILE ORDER → **key order IS preserved** (Py3.7+ insertion-ordered dict). Verified: chunk (3,5) root keys in order: Status, zPos, block_entities, yPos, LastUpdate, structures, InhabitedTime, xPos, Heightmaps, sections, isLightOn, block_ticks — vanilla HashMap order survives parsing. |
| 11 IntArray | `list[int]` (signed 32) | collapses with List-of-Int by type |
| 12 LongArray | `list[int]` (signed 64) | collapses with List-of-Long by type |

Implication for Task 12: byte-level gating never goes through parse_nbt (it hashes raw
payload bytes); parse_nbt is only the DIAGNOSTIC layer, and its type collapses (Float vs
Double, Long vs Int, LongArray vs List) mean `nbt_diff` can miss pure tag-width mismatches
that the byte gate will still catch. Trust the byte gate; use nbt_diff only to localize.

- `TAG_NAMES` dict (line 80) — id→name for error messages only.
- NO pretty-printer, NO `__main__`/CLI in mca.py, and NO payload-extraction CLI anywhere in
  tools/golden (checked file list). Extraction recipe in §3.

### `mask_last_update(payload) -> bytes` (line 155)

- Pattern (line 152): `_LAST_UPDATE = b"\x04\x00\x0aLastUpdate"` — TAG_Long (0x04) +
  big-endian name length 0x000A + ASCII `LastUpdate`. Uses `payload.find` (FIRST occurrence
  only); replaces the following 8 value bytes with `b"\x00" * 8`. Not found → payload
  returned unchanged.
- Caveats: (a) first-occurrence substring search — a byte-identical pattern occurring
  earlier inside string/array data would be masked instead (never happens in the golden;
  hash is stable — but our C writer should keep root LastUpdate emission position vanilla-
  identical anyway); (b) only the root LastUpdate exists in 26.2 chunk NBT (empirically;
  cross-checked by canonical hash matching SHA256SUMS).
- Equivalent C-side rule for the gate: emit LastUpdate as TAG_Long named "LastUpdate";
  the gate zeroes its 8 value bytes on BOTH sides (or we emit 0 and mask golden).

### `nbt_diff(a, b, path="", out=None, limit=200)` (line 164)

Recursive structural diff returning `["<path>: <a!r> != <b!r>", ...]` strings, capped at
`limit`. Dict keys iterated over `sorted(set(a)|set(b))` with "missing in A/B" lines; type
mismatch reported as `type X != Y` and recursion stops; lists compared element-wise ONLY
when lengths equal (length mismatch is a single line, no element detail); leaves via `!=`.

## 2. tools/golden/compare_regions.py

CLI modes (`main`, lines 94–102):
1. `compare_regions.py A.mca B.mca` — layered comparison, exit 0/1 (2 on usage error).
2. `compare_regions.py --canonical-hash F.mca` — prints `<sha256>  <path>`, exit 0.

Canonical hash recipe (`canonical_hash`, lines 31–37) — EXACT:
```
h = sha256()
for i in sorted(present chunk indices):        # ascending index order
    h.update(struct.pack(">I", i))             # 4-byte big-endian index
    h.update(mask_last_update(chunks[i].payload))  # decompressed payload, LastUpdate value zeroed
h.hexdigest()
```
Region header, timestamp table, sector layout and compression framing are all EXCLUDED —
stable under resaves that only move sectors (docstring lines 17–19). This means Task 12's C
writer can hit the canonical gate WITHOUT reproducing vanilla's zlib encoder output or
sector allocation; raw-file equality would additionally require both.

`compare(pa, pb)` (lines 40–91) reports, in order: `raw_file_bytes_equal`, present counts,
`chunks_only_in_A/B`, `header_timestamps_differ n/m`, `payloads_differ n/m`,
`canonical_payloads_differ n/m`, then for the first ≤5 payload-differing chunks an
`nbt_diff` (limit 10) tagged `(masked by canonicalization)` when the chunk is not in the
canonical-diff set, then `canonical_sha256_A/B`. Verdicts: exit 1 "regions differ beyond
save-time metadata" iff only_a/only_b/canon_diff nonempty; else exit 0 ("worldgen-identical;
differences are save-time metadata only" when payload/ts/raw differ, else "bit-identical").

## 3. Payload extraction recipe (VERIFIED by running it)

```python
import sys
sys.path.insert(0, "/home/ubuntu/projects/hyperchunk/tools/golden")
from mca import read_region, mask_last_update
chunks = read_region("/home/ubuntu/projects/hyperchunk/golden/seed1234567890_r.0.0.mca")
cx, cz = 3, 5                        # chunk coords within region, 0..31 each
i = (cx & 31) + (cz & 31) * 32       # YES: index = x + z*32 (mca.py line 27; x=i%32, z=i//32 line 72)
payload = chunks[i].payload          # decompressed NBT bytes, starts 0x0A 0x00 0x00
with open(f"/tmp/chunk_{cx}_{cz}.nbt", "wb") as f:
    f.write(mask_last_update(payload))   # canonical (LastUpdate-masked) form for byte-diffing
```
Verified for (3,5): index 163, compression 2, payload 80,935 bytes, Status `minecraft:full`,
root name `''`. Drop `mask_last_update` to keep the raw payload.

## 4. Golden tracking facts

- `golden/seed1234567890_r.0.0.mca` is **NOT committed** — `.gitignore` has `golden/*.mca`
  ("raw dumps stay local"). Local copy exists: 8,671,232 bytes, mtime Jul 28. Tracked under
  `golden/` (git ls-files): `SHA256SUMS`, `features-trace/{FORMAT.md,SHA256SUMS,seed…/order.manifest,order.snapshots}`,
  `rng/*.txt` (10 files), `stages/{FORMAT.md,seed…/order.manifest,order.snapshots}`,
  `stages-alt/{SHA256SUMS,seed…/order.manifest,order.snapshots}`. Raw stage dump .txt files
  are local-only, hash-pinned in the SUMS files.
- `golden/SHA256SUMS` format: `<sha256hex><TWO spaces><path relative to golden/>`, file kept
  sorted by path (`sort -k2`), 319 lines. The region entries (lines 1–2):
  ```
  fb861036b565a9c053e206d2a4add7b8ea8aed165f46331f68c0047be7a9bfd3  seed1234567890_r.0.0.mca
  ea3fd98c40cb83260ed9e7c61f5e627ec2e5a101869d8e2474ed28785a4127ec  seed1234567890_r.0.0.mca#canonical-payload
  ```
  `#canonical-payload` is a pseudo-path suffix convention: the canonical_hash (§2) of the
  same file. BOTH re-verified today against the local .mca (sha256sum + --canonical-hash
  both match ⇒ local golden is exactly the pinned baseline).
- Generation: `tools/golden/make_golden.sh` (lines 182–193) — boots pinned vanilla 26.2
  server, seed 1234567890, `-Dmax.bg.threads=1`, tick freeze + gamerules, forceloads all
  1024 chunks of region (0,0), polls `region_stats.py` until `status minecraft:full 1024`,
  copies the .mca to golden/, then updates SHA256SUMS in place: `grep -v "seed${SEED}_r.0.0.mca"`
  (removes BOTH old lines — note this substring also matches the `#canonical-payload` line),
  appends the two fresh lines, `sort -k2`. To add new entries by hand: same two-space
  format, re-sort by field 2.
- Other SUMS writers (same convention): `make_stage_dumps.sh` (line 248: primary bundle →
  `golden/SHA256SUMS` with prefix `stages/seed$SEED`; alt → `golden/stages-alt/SHA256SUMS`;
  line 260: skips SUMS update entirely under custom `HYPERCHUNK_DUMP_DIR`), and
  `make_feature_trace.sh` (own `golden/features-trace/SHA256SUMS`; first verifies grid dumps
  against the primary SUMS and aborts without writing on mismatch).

## 5. region_stats.py / semantic_compare.py (skim)

- `region_stats.py <r.mca>`: prints `present <n>`, one `status <Status> <count>` per
  distinct root `Status`, one `compression <id> <count>`. Missing file → `present 0`, exit 0.
  Useful as a smoke test on OUR emitted region file too.
- `semantic_compare.py A.mca B.mca` (exit 0 iff content-identical) / `--hash F.mca`:
  serialization-insensitive layer for when the byte gate fails and we need "different
  content vs different encoding". `semantic_form` decodes: `sections[].block_states` →
  4096 `Name[k=v,...]` strings via `decode_paletted` (bits = `max(4, (len(palette)-1).bit_length())`,
  missing `data` key → all `palette[0]`); `sections[].biomes` → 64 entries, min_bits=1;
  `Heightmaps.*` → 256 9-bit values (`unpack`: MC 1.16+ packing, entries never span longs,
  `per_long = 64 // bits`, low bits first within each long); `block_ticks`/`fluid_ticks`
  sorted by key `(x, y, z, i, t, p)` (lowercase field names — tick compounds use keys
  `x,y,z,i,t,p`); `PostProcessing` → per-section sorted; `LastUpdate` dropped. Hash =
  sha256 over `str(i)` + compact sorted-keys JSON per chunk, ascending index.

## 6. Misc facts Task 12 needs

- Golden region live stats (verified): `present 1024`, `status minecraft:full 1024`,
  `compression 2 1024` — every chunk zlib, all full.
- Length-field semantics for our writer (from mca.py read path): 4-byte BE `length` at
  sector base includes the 1 compression byte; stored compressed bytes are `length - 1`;
  offset-table entry packs `(sector_off << 8) | sector_count`, timestamps at 4096+i*4.
  (Vanilla-side padding/sector-count rounding rules are Task 12 bytecode work — not covered
  by these tools; UNVERIFIED here.)
- The gate wording "byte-exact on decompressed chunk payloads, LastUpdate masked" is
  EXACTLY `canonical_hash` (§2): match `ea3fd98c…27ec` and the gate is green; raw-file hash
  `fb8610…bfd3` additionally pins zlib encoding + sector layout (non-goal unless Task 12
  says otherwise).
- Directory conventions: probe scripts → `tools/golden/experiments/` (currently
  `sequential_probe.sh`); research notes → `.hermes/notes/task12-region/R*.md` with a final
  `A-…-handoff.md` (pattern from task10-light/task11-spawnfull). `tools/golden/work/`,
  `libs/`, `logs/` are gitignored scratch.
- `tools/golden/NOTES.md` lines 125–160: bundle coherence rules (07+ dumps not regenerable;
  regeneration replaces a bundle wholesale) — relevant only if Task 12 ever re-captures the
  golden .mca: doing so REWRITES both SHA256SUMS region lines and invalidates any byte
  gates pinned to the old canonical hash.
