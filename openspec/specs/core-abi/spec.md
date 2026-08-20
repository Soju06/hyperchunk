# core-abi Specification

## Purpose

Normative SSOT for the core library boundary: the pure compute library
contract, the region-granularity C ABI, what the core owns (arena/SoA and
the batch scheduler), and the consumers (CLI/FFM/Rust FFI). Rationale,
numbers, and decision history are in [context.md](context.md).

## Requirements

### Requirement: Pure compute core

`core/` SHALL be a pure computation library (ADR-003 D1, reaffirmed by
ADR-005): it MUST NOT contain file I/O, networking, or player/entity/
inventory/tick-loop state. Consumers pass buffers in and receive buffers
out; region file writing lives in the CLI/consumer layer. The core MUST
build with no third-party dependencies: it links against libc, libm, and
pthreads only.

#### Scenario: Dependency audit

- **WHEN** the built core library is inspected (`ldd` / link line)
- **THEN** no dependency beyond libc/libm/pthreads appears

#### Scenario: State symbol audit

- **WHEN** core sources and the public header are searched for socket,
  player, entity, or tick-loop lifecycle symbols
- **THEN** none exist

### Requirement: Region-granularity ABI only

The public C ABI SHALL expose region-granularity batch entry points only
(ADR-003 D2). Node-level density-function entry points MUST NOT be declared
in the public header: the 18.5%-of-chunk-time boundary-cost cliff is
prevented at the API surface, not by convention.

#### Scenario: Public header audit

- **WHEN** the public header is reviewed
- **THEN** it declares region/batch-level entry points and no per-node or
  per-sample functions

### Requirement: Core owns the arena allocator and the batch scheduler

The arena/SoA allocator and the batch scheduler are part of the core, not
the consumer (ADR-003 D3). A consumer MUST NOT need to implement its own
chunk scheduling to obtain full performance; scheduling policy is selected
through the library contract ([scheduler](../scheduler/spec.md)).

#### Scenario: New FFI consumer

- **WHEN** a new consumer (e.g. a Rust server) integrates the core
- **THEN** it submits region batches and receives completed buffers without
  reimplementing allocation or scheduling

### Requirement: FFI consumers and bridge technology

Supported consumer surfaces are: the CLI (bench, parity verification, region
output), a Fabric-side Java bridge using FFM (JEP 454, final in Java 25,
ADR-006 D4; Phase 3, not yet started), and Rust FFI (planned; Pumpkin/
Valence-class servers). Every bridge MUST preserve the region-granularity
boundary invariant; a bridge that crosses the FFI per node or per sample
MUST NOT be added.

#### Scenario: Bridge granularity review

- **WHEN** a bridge implementation is proposed
- **THEN** its FFI crossings are per region batch, and per-node crossing
  designs are rejected
