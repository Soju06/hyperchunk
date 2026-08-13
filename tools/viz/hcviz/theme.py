"""Theme loading: colors, fonts, spacing — all from a JSON/YAML file."""

from __future__ import annotations

import json
from pathlib import Path

from PIL import ImageFont


class ThemeError(Exception):
    pass


def _load_data(path: Path):
    text = path.read_text()
    if path.suffix in (".yaml", ".yml"):
        import yaml

        return yaml.safe_load(text)
    return json.loads(text)


class Theme:
    def __init__(self, data: dict, path: Path):
        self.data = data
        self.path = path
        self._font_cache = {}

    @classmethod
    def load(cls, path) -> "Theme":
        p = Path(path).expanduser()
        if not p.is_file():
            raise ThemeError(f"theme file not found: {p}")
        return cls(_load_data(p), p)

    def get(self, *keys, default=None):
        node = self.data
        for k in keys:
            if not isinstance(node, dict) or k not in node:
                if default is not None:
                    return default
                raise ThemeError(
                    f"theme {self.path.name}: missing key '{'.'.join(keys)}'"
                )
            node = node[k]
        return node

    def color(self, *keys) -> tuple:
        v = self.get("colors", *keys)
        return tuple(int(c) for c in v)

    def accent(self, key, index: int = 0) -> tuple:
        """Resolve a panel accent: explicit RGB, named accent, or cycle fallback."""
        if isinstance(key, (list, tuple)):
            return tuple(int(c) for c in key)
        accents = self.get("colors", "accents", default={})
        if isinstance(key, str):
            k = key.lower()
            if k in accents:
                return tuple(int(c) for c in accents[k])
        cycle = self.get("colors", "accent_cycle")
        return tuple(int(c) for c in cycle[index % len(cycle)])

    def font(self, role: str, size: int) -> ImageFont.FreeTypeFont:
        key = (role, size)
        if key in self._font_cache:
            return self._font_cache[key]
        spec = self.get("fonts", role)
        font_dir = Path(self.get("fonts", "dir")).expanduser()
        path = font_dir / spec["file"]
        if not path.is_file():
            raise ThemeError(f"font not found: {path}")
        font = ImageFont.truetype(str(path), size)
        wght = spec.get("wght")
        if wght is not None:
            try:
                font.set_variation_by_axes([wght])
            except OSError:
                pass  # static font fallback: file has no variation axes
        self._font_cache[key] = font
        return font
