# P3-0 notes: owner decisions and run record

## Run record

Proposal, design, spec deltas, and tasks were produced in one delegated run
(2026-08-20). The run hit an upstream 503 rate limit during its final commit
step; artifacts were complete and the controller verified and committed them.
Validation after salvage: `openspec validate phase3-ffm-bridge --strict` ->
"Change 'phase3-ffm-bridge' is valid" (rc=0).

## Decisions needing the owner before the affected tasks start

1. **Task-set size.** The recommended design requires core work before any
   bridge code: a public batch facade (the header currently exports only
   `hc_version`/`hc_abi_version`), region-addressing generalization beyond
   r.0.0, multi-noise biome assignment, and buffer-fed structure starts
   replacing the `fopen` paths. That is a substantial core campaign inside a
   phase whose headline is "the Fabric bridge". Approve the full scope, or cut
   Phase 3 to a narrower MVP.

2. **Structures via a live JVM (Decision 5).** Assembled villages require the
   bridge to drive vanilla `createStructures`/`createReferences` at pregen
   time and pass the result in as batch input. This amends the "no JVM in the
   generation path" framing at spec level (the generation-pipeline delta says
   so explicitly). Accept the amendment, or accept demo-only served worlds
   without assembled structures.

3. **Second golden capture region.** Claiming assembled-structure parity in
   general needs a capture region that actually contains a village, through
   the `golden:` process, owner-reviewed. Schedule it inside Phase 3 or defer
   the claim.

4. **Interactive scope (Decision 2).** MVP is explicit pregen; on-demand
   region batching is deferred because a full-region batch on typical
   hardware implies a multi-second stall at region boundaries. Ahead-of-player
   prefetch is listed as a stretch task. Confirm pregen-only MVP, or fund the
   prefetch work as in-scope.

5. **UNVERIFIED items.** Interception-point and structure-driving facts are
   verified to signature level only from local sources; design.md marks them
   UNVERIFIED. They resolve during Task 3/4 implementation or an owner review
   round, whichever comes first.
