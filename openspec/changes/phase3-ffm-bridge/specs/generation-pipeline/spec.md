# generation-pipeline Delta

## MODIFIED Requirements

### Requirement: Full pipeline, eleven stages

The implementation MUST cover the full vanilla chunk pipeline, all 11
statuses from `structure_starts` to `full` (noise, surface rules, carvers,
features, lighting, spawn, full promotion included), not a terrain-only
subset (ADR-002 D2). Every stage runs in the C core; there is no JVM in the
generation path, with one input-classification exception: structure starts
and structure template NBT are caller-provided batch inputs. A consumer MAY
supply vanilla-computed structure starts — including jigsaw-assembled piece
lists, whose assembly remains out of core scope per ADR-002 D4 — and the
core SHALL place supplied pieces bit-exactly while never computing jigsaw
assembly itself.

#### Scenario: Full-status region

- **WHEN** hyperchunk generates region r.0.0
- **THEN** every chunk reaches vanilla status `minecraft:full` and the
  full-region parity gate ([worldgen-parity](../worldgen-parity/spec.md))
  passes over the complete pipeline output

#### Scenario: Structure starts supplied as batch input

- **GIVEN** structure starts and templates captured from, or computed by,
  vanilla 26.2 code for the batch's padded chunk set
- **WHEN** a consumer passes them as caller-provided buffers to a region
  batch
- **THEN** the core places the supplied pieces during its features stage and
  the output satisfies the parity gates
