"""timeline.json loading, semantic validation, and synthetic generators."""

from __future__ import annotations

import json
import random
from pathlib import Path

import numpy as np

SYNTH_PATTERNS = ("scan", "wave", "burst")


class TimelineError(Exception):
    pass


class Timeline:
    def __init__(self, data: dict, source: str = "<memory>"):
        self.data = data
        self.source = source
        self.system = data["system"]
        self.display_name = data["display_name"]
        self.stage = data.get("stage", "")
        self.wall_s = float(data["wall_s"])
        self.chunks = data["chunks"]
        self.meta = data.get("meta", {})

    @classmethod
    def load(cls, path) -> "Timeline":
        p = Path(path)
        try:
            data = json.loads(p.read_text())
        except json.JSONDecodeError as e:
            raise TimelineError(f"{p}: invalid JSON: {e}") from e
        errors = semantic_errors(data)
        if errors:
            raise TimelineError(f"{p}: " + "; ".join(errors[:5]))
        return cls(data, str(p))

    def region(self) -> tuple:
        """(x0, z0, w, h) — meta.region; else r.0.0 when all chunks fit in
        [0,31] (the standard race region), else the chunk bounding box."""
        r = self.meta.get("region")
        if r:
            return (r["x0"], r["z0"], r["w"], r["h"])
        xs = [c["cx"] for c in self.chunks]
        zs = [c["cz"] for c in self.chunks]
        if all(0 <= v < 32 for v in xs) and all(0 <= v < 32 for v in zs):
            return (0, 0, 32, 32)
        return (min(xs), min(zs), max(xs) - min(xs) + 1, max(zs) - min(zs) + 1)

    def t_done_grid(self) -> np.ndarray:
        """(h, w) float ms grid, +inf where no chunk completion was recorded."""
        return self._grid("t_done_ms")

    def t_stage1_grid(self):
        """(h, w) float ms grid of the optional stage-1 series (see
        meta.stage1_event), +inf where a chunk carries no t_stage1_ms.
        None when no chunk carries the series (single-stage timeline)."""
        if not any("t_stage1_ms" in c for c in self.chunks):
            return None
        return self._grid("t_stage1_ms")

    def _grid(self, field: str) -> np.ndarray:
        x0, z0, w, h = self.region()
        grid = np.full((h, w), np.inf, dtype=np.float64)
        for c in self.chunks:
            gx, gz = c["cx"] - x0, c["cz"] - z0
            if 0 <= gx < w and 0 <= gz < h and field in c:
                grid[gz, gx] = c[field]
        return grid

    def scaled(self, wall_s: float) -> "Timeline":
        """Uniformly rescale all completion times so the run ends at wall_s."""
        f = wall_s / self.wall_s
        data = dict(self.data)
        data["wall_s"] = wall_s

        def _scale(c):
            out = {"cx": c["cx"], "cz": c["cz"], "t_done_ms": round(c["t_done_ms"] * f, 3)}
            if "t_stage1_ms" in c:
                out["t_stage1_ms"] = round(c["t_stage1_ms"] * f, 3)
            return out

        data["chunks"] = [_scale(c) for c in self.chunks]
        meta = dict(self.meta)
        meta["time_scaled_from_wall_s"] = self.wall_s
        data["meta"] = meta
        return Timeline(data, self.source + f" (scaled to {wall_s}s)")


def semantic_errors(data) -> list:
    """Checks beyond JSON Schema: duplicates, times past wall, bounds."""
    errors = []
    if not isinstance(data, dict):
        return ["root is not an object"]
    for key in ("system", "display_name", "wall_s", "chunks"):
        if key not in data:
            errors.append(f"missing required field '{key}'")
    if errors:
        return errors
    wall = data["wall_s"]
    if isinstance(wall, bool) or not isinstance(wall, (int, float)) or wall <= 0:
        errors.append("wall_s must be a positive number")
        return errors
    chunks = data["chunks"]
    if not isinstance(chunks, list) or not chunks:
        errors.append("chunks must be a non-empty array")
        return errors
    region = None
    meta = data.get("meta")
    if isinstance(meta, dict) and isinstance(meta.get("region"), dict):
        r = meta["region"]
        if {"x0", "z0", "w", "h"} <= set(r):
            region = (r["x0"], r["z0"], r["w"], r["h"])
    wall_ms = float(wall) * 1000.0
    seen = set()
    outside = 0

    def _is_int(v):
        return isinstance(v, int) and not isinstance(v, bool)

    for i, c in enumerate(chunks):
        if not isinstance(c, dict) or not {"cx", "cz", "t_done_ms"} <= set(c):
            errors.append(f"chunks[{i}]: needs cx, cz, t_done_ms")
            continue
        if not (_is_int(c["cx"]) and _is_int(c["cz"])):
            errors.append(f"chunks[{i}]: cx/cz must be integers")
            continue
        key = (c["cx"], c["cz"])
        if key in seen:
            errors.append(f"chunks[{i}]: duplicate chunk {key}")
        seen.add(key)
        t = c["t_done_ms"]
        if isinstance(t, bool) or not isinstance(t, (int, float)) or t < 0:
            errors.append(f"chunks[{i}]: t_done_ms must be a number >= 0")
        elif t > wall_ms * 1.001 + 1.0:
            errors.append(
                f"chunks[{i}]: t_done_ms {t:.1f} exceeds wall_s ({wall_ms:.1f} ms)"
            )
        elif "t_stage1_ms" in c:
            s1 = c["t_stage1_ms"]
            if isinstance(s1, bool) or not isinstance(s1, (int, float)) or s1 < 0:
                errors.append(f"chunks[{i}]: t_stage1_ms must be a number >= 0")
            elif s1 > t:
                errors.append(
                    f"chunks[{i}]: t_stage1_ms {s1:.1f} exceeds t_done_ms ({t:.1f})"
                )
        if region and not (
            region[0] <= c["cx"] < region[0] + region[2]
            and region[1] <= c["cz"] < region[1] + region[3]
        ):
            outside += 1
        if len(errors) >= 20:
            errors.append("... (truncated)")
            break
    if outside:
        errors.append(f"{outside} chunks outside meta.region {region}")
    return errors


def schema_available() -> bool:
    try:
        import jsonschema  # noqa: F401

        return True
    except ImportError:
        return False


def schema_errors(data, schema_path: Path) -> list:
    """Validate against the JSON Schema file (needs the jsonschema package)."""
    try:
        import jsonschema
    except ImportError:
        return []
    schema = json.loads(Path(schema_path).read_text())
    validator = jsonschema.validators.validator_for(schema)(schema)
    return [
        f"schema: {'/'.join(str(p) for p in e.absolute_path) or '<root>'}: {e.message}"
        for e in validator.iter_errors(data)
    ]


def synth(
    pattern: str,
    wall_s: float,
    seed: int = 0,
    size: int = 32,
    system: str = None,
    display_name: str = None,
    stage: str = "",
    workers: int = None,
) -> dict:
    """Deterministic synthetic timeline (marked meta.synthetic=true).

    scan  — spawn-out ring growth with organic jitter (vanilla-like)
    wave  — diagonal wavefront (c2me-like)
    burst — random parallel completion (throughput-bound pool)
    """
    if pattern not in SYNTH_PATTERNS:
        raise TimelineError(f"unknown pattern '{pattern}' (want {SYNTH_PATTERNS})")
    rng = random.Random(seed)
    c0 = (size - 1) / 2.0
    keyed = []
    for cz in range(size):
        for cx in range(size):
            if pattern == "scan":
                d = max(abs(cx - c0), abs(cz - c0))
                key = d + rng.uniform(-1.7, 1.7)
            elif pattern == "wave":
                key = (cx + cz) + rng.uniform(-2.5, 2.5)
            else:  # burst
                key = rng.random()
            keyed.append((key, cx, cz))
    keyed.sort()
    n = len(keyed)
    wall_ms = wall_s * 1000.0
    chunks = [
        {"cx": cx, "cz": cz, "t_done_ms": round(wall_ms * (i + 1) / n, 3)}
        for i, (_, cx, cz) in enumerate(keyed)
    ]
    meta = {
        "synthetic": True,
        "pattern": pattern,
        "seed": seed,
        "region": {"x0": 0, "z0": 0, "w": size, "h": size},
    }
    if workers:
        meta["workers"] = workers
    return {
        "system": system or f"synthetic-{pattern}",
        "display_name": display_name or pattern,
        "stage": stage,
        "wall_s": wall_s,
        "chunks": chunks,
        "meta": meta,
    }
