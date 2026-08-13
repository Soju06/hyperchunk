"""Panel layouts: canvas geometry computed from theme spacing constants."""

from __future__ import annotations

LAYOUT_PANELS = {"single": 1, "vs2": 2, "race3": 3, "race4": 4}


class LayoutError(Exception):
    pass


class Layout:
    def __init__(self, theme, kind: str, n_panels: int):
        if kind not in LAYOUT_PANELS:
            raise LayoutError(
                f"unknown layout '{kind}' (want one of {sorted(LAYOUT_PANELS)})"
            )
        want = LAYOUT_PANELS[kind]
        if n_panels != want:
            raise LayoutError(f"layout '{kind}' needs {want} panels, got {n_panels}")
        self.kind = kind
        pad = theme.get("spacing", "pad")
        gap = theme.get("spacing", "gap")
        top = theme.get("spacing", "top")
        bot = theme.get("spacing", "bot")
        size = theme.get("panel", "size")
        self.pad, self.gap, self.top, self.bot, self.panel_size = pad, gap, top, bot, size
        self.width = 2 * pad + n_panels * size + (n_panels - 1) * gap
        self.height = top + size + bot
        self.panel_xy = [(pad + i * (size + gap), top) for i in range(n_panels)]
        self.diff_overlay = kind == "vs2"
