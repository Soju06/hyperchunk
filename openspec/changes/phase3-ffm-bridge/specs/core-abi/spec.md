# core-abi Delta

## ADDED Requirements

### Requirement: Batch facade sufficiency

The public facade header SHALL declare a complete region-batch consumer
surface: initialization from caller-provided 26.2 reference data (datapack
worldgen JSON closures, structure starts and template NBT as buffers),
region batch execution with scheduling policy selection (FREE or
REPLAY(manifest), per the [scheduler](../../../../specs/scheduler/spec.md)
library contract), retrieval of serialized chunk payloads and the in-memory
region image, and error reporting sufficient for a consumer to decide
vanilla fallback at configuration granularity. A consumer MUST be able to
drive a full region generation through the facade alone, without including
internal `core/src` headers.

#### Scenario: Facade-only region generation

- **WHEN** the CLI parity path regenerates region r.0.0 using only public
  facade includes
- **THEN** the full-region parity gate passes unchanged

#### Scenario: Init failure is reported, not guessed

- **GIVEN** worldgen input data containing a construct the core cannot
  interpret
- **WHEN** facade initialization runs
- **THEN** it fails with an error message identifying the rejected input,
  and no partially-initialized configuration is usable

## MODIFIED Requirements

### Requirement: FFI consumers and bridge technology

Supported consumer surfaces are: the CLI (bench, parity verification, region
output), a Fabric-side Java bridge using FFM (JEP 454, final in Java 25,
ADR-006 D4; Phase 3, contract in
[fabric-bridge](../../../../specs/fabric-bridge/spec.md)), and Rust FFI
(planned; Pumpkin/Valence-class servers). Every bridge MUST preserve the
region-granularity boundary invariant; a bridge that crosses the FFI per
node or per sample MUST NOT be added. A bridge MUST bind only symbols
declared in the public facade header; internal `core/src` headers are not a
consumer surface.

#### Scenario: Bridge granularity review

- **WHEN** a bridge implementation is proposed
- **THEN** its FFI crossings are per region batch, and per-node crossing
  designs are rejected

#### Scenario: Bridge binding audit

- **WHEN** a bridge's native binding layer is reviewed
- **THEN** every bound symbol is declared in the public facade header
