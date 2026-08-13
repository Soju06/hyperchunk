"""race.yaml loading — the render composition: panels, theme, layout, view."""

from __future__ import annotations

from pathlib import Path

import yaml

from .theme import Theme
from .timeline import Timeline

# Confirmed v5/v4 mockup copy. Overridable per race.yaml; do not add new copy.
DEFAULT_HEADER_TITLE = "1024 chunks, one race"
DEFAULT_HEADER_SUB = "same seed · same machine"
DEFAULT_ENDCARD_WORDMARK = "hyperchunk"
DEFAULT_ENDCARD_SUB = (
    "and the only one that generates the same world, bit for bit, every run"
)


class ConfigError(Exception):
    pass


class PanelSpec:
    def __init__(self, raw: dict, base_dir: Path, index: int):
        if "timeline" not in raw:
            raise ConfigError(f"panels[{index}]: 'timeline' is required")
        self.timeline_path = (base_dir / raw["timeline"]).resolve()
        self.tile_path = (base_dir / raw["tile"]).resolve() if raw.get("tile") else None
        self.mca_path = (base_dir / raw["mca"]).resolve() if raw.get("mca") else None
        self.name = raw.get("name")
        self.accent = raw.get("accent")
        # Honest rescale knob: keeps a real run's reveal *shape* while pinning
        # the wall to a number measured elsewhere. Recorded in timeline meta.
        self.normalize_wall_s = raw.get("normalize_wall_s")

    def load_timeline(self) -> Timeline:
        tl = Timeline.load(self.timeline_path)
        if self.normalize_wall_s:
            tl = tl.scaled(float(self.normalize_wall_s))
        return tl


class RaceConfig:
    def __init__(self, raw: dict, base_dir: Path, overrides: dict = None):
        overrides = overrides or {}
        self.base_dir = base_dir

        if overrides.get("theme"):
            # CLI override: resolve like any shell argument, from the CWD
            tp = Path(overrides["theme"]).expanduser()
            self.theme = Theme.load(tp)
        else:
            theme_path = raw.get("theme")
            if not theme_path:
                raise ConfigError("race config: 'theme' is required")
            tp = Path(theme_path).expanduser()
            self.theme = Theme.load(tp if tp.is_absolute() else base_dir / tp)

        self.layout = overrides.get("layout") or raw.get("layout", "race3")
        self.view = overrides.get("view") or raw.get("view", "reveal")
        if self.view not in ("reveal", "heat"):
            raise ConfigError(f"unknown view '{self.view}' (want reveal|heat)")
        fps = overrides.get("fps")
        self.fps = int(fps if fps is not None else raw.get("fps", 25))
        if self.fps < 1:
            raise ConfigError(f"fps must be >= 1, got {self.fps}")
        self.hold_s = float(raw.get("hold_s", 1.5))

        header = raw.get("header", {}) or {}
        self.header_title = header.get("title", DEFAULT_HEADER_TITLE)
        self.header_sub = header.get("sub", DEFAULT_HEADER_SUB)

        ec = raw.get("endcard", {}) or {}
        self.endcard_enabled = bool(ec.get("enabled", True))
        self.endcard_hold_s = float(ec.get("hold_s", 1.5))
        self.endcard_wordmark = ec.get("wordmark", DEFAULT_ENDCARD_WORDMARK)
        self.endcard_headline = ec.get("headline", "auto")
        self.endcard_sub = ec.get("sub", DEFAULT_ENDCARD_SUB)
        self.endcard_baseline = int(ec.get("baseline_panel", 0))

        panels = raw.get("panels")
        if not panels:
            raise ConfigError("race config: 'panels' is required")
        self.panels = [PanelSpec(p, base_dir, i) for i, p in enumerate(panels)]
        if not 0 <= self.endcard_baseline < len(self.panels):
            raise ConfigError(
                f"endcard.baseline_panel {self.endcard_baseline} out of range "
                f"for {len(self.panels)} panels"
            )

        tiles_dir = raw.get("tiles_dir")
        self.tiles_dir = (
            (base_dir / tiles_dir).resolve()
            if tiles_dir
            else Path(__file__).resolve().parents[1] / "tiles"
        )

    @classmethod
    def load(cls, path, overrides: dict = None) -> "RaceConfig":
        p = Path(path).expanduser().resolve()
        if not p.is_file():
            raise ConfigError(f"race config not found: {p}")
        raw = yaml.safe_load(p.read_text())
        if not isinstance(raw, dict):
            raise ConfigError(f"{p}: not a mapping")
        return cls(raw, p.parent, overrides)
