"""Frame compositor: race frames (reveal/heat views), endcard, frame iterator."""

from __future__ import annotations

import math

import numpy as np
from PIL import Image, ImageDraw

from .layout import Layout


def fmt_secs(t: float) -> str:
    """Timer/endcard seconds format per v5 mock: 0.89s / 3.7s / 11.9s."""
    return f"{t:.2f}s" if t < 0.9995 else f"{t:.1f}s"


class RenderError(Exception):
    pass


class _Panel:
    def __init__(self, spec, timeline, accent, tile_rgb, theme, view):
        self.label = spec.name or timeline.display_name
        self.accent = accent
        self.timeline = timeline
        self.wall_s = timeline.wall_s
        self.t_grid = timeline.t_done_grid()
        gh, gw = self.t_grid.shape
        psize = theme.get("panel", "size")
        cpx = theme.get("panel", "chunk_px")
        if gw * cpx != psize or gh * cpx != psize:
            raise RenderError(
                f"panel '{self.label}': {gw}x{gh} grid × {cpx}px != panel {psize}px"
            )
        self.cpx = cpx
        paper = np.array(theme.color("paper"), dtype=np.uint8)
        self.paper_px = np.broadcast_to(paper, (psize, psize, 3))
        if tile_rgb is None:
            tile_rgb = np.array(self.paper_px)
        self.tile = tile_rgb
        if view == "heat":
            base = np.array(theme.color(theme.get("heat", "ramp_from")), np.float64)
            acc = np.array(accent, np.float64)
            gamma = float(theme.get("heat", "ramp_gamma"))
            wall_ms = max(self.wall_s * 1000.0, 1e-9)
            norm = np.clip(self.t_grid / wall_ms, 0.0, 1.0) ** gamma
            norm[~np.isfinite(self.t_grid)] = 0.0
            cells = base + (acc - base) * norm[..., None]
            self.tile = (
                cells.repeat(cpx, axis=0).repeat(cpx, axis=1).round().astype(np.uint8)
            )

    def content_at(self, t_ms: float) -> np.ndarray:
        mask = self.t_grid <= t_ms
        mask_px = mask.repeat(self.cpx, axis=0).repeat(self.cpx, axis=1)
        return np.where(mask_px[..., None], self.tile, self.paper_px)


class Renderer:
    def __init__(self, cfg):
        self.cfg = cfg
        self.theme = t = cfg.theme
        self.panels = []
        for i, spec in enumerate(cfg.panels):
            timeline = spec.load_timeline()
            accent = t.accent(spec.accent or timeline.display_name, i)
            tile = self._resolve_tile(spec)
            self.panels.append(_Panel(spec, timeline, accent, tile, t, cfg.view))
        self.layout = Layout(t, cfg.layout, len(self.panels))
        self.race_end = max(p.wall_s for p in self.panels)
        self._diff_cells = self._compute_diff() if self.layout.diff_overlay else None
        self._base = self._build_base()
        self._endcard = None

    def _resolve_tile(self, spec):
        psize = self.theme.get("panel", "size")
        path = spec.tile_path
        if path is None and spec.mca_path is not None:
            from .mca_tile import tile_for_mca

            path = tile_for_mca(spec.mca_path, self.theme, self.cfg.tiles_dir)
        if path is None:
            return None
        img = Image.open(path).convert("RGB")
        if img.size != (psize, psize):
            img = img.resize((psize, psize), Image.NEAREST)
        return np.asarray(img, dtype=np.uint8)

    def _compute_diff(self):
        a, b = self.panels[0], self.panels[1]
        cpx = a.cpx
        gh, gw = a.t_grid.shape
        ta = a.tile.reshape(gh, cpx, gw, cpx, 3).astype(np.int16)
        tb = b.tile.reshape(gh, cpx, gw, cpx, 3).astype(np.int16)
        return np.abs(ta - tb).mean(axis=(1, 3, 4)) > 2.0

    # ------------------------------------------------------------- static ---

    def _build_base(self) -> Image.Image:
        t, lay = self.theme, self.layout
        img = Image.new("RGB", (lay.width, lay.height), t.color("bg"))
        d = ImageDraw.Draw(img)
        d.text(
            tuple(t.get("header", "title_xy")),
            self.cfg.header_title,
            font=t.font("headline", t.get("header", "title_size")),
            fill=t.color("ink"),
        )
        d.text(
            tuple(t.get("header", "sub_xy")),
            self.cfg.header_sub,
            font=t.font("body", t.get("header", "sub_size")),
            fill=t.color("subink"),
        )
        label_font = t.font("body_semibold", t.get("label", "size"))
        label_dy = t.get("label", "dy")
        size = lay.panel_size
        border = t.get("panel", "border_px")
        pg_dy, pg_h = t.get("progress", "dy"), t.get("progress", "height")
        for panel, (x, y) in zip(self.panels, lay.panel_xy):
            d.text((x, y + label_dy), panel.label, font=label_font, fill=t.color("ink"))
            if border:
                d.rectangle(
                    (x - border, y - border, x + size + border - 1, y + size + border - 1),
                    outline=t.color("line"),
                    width=border,
                )
            d.rectangle(
                (x, y + size + pg_dy, x + size - 1, y + size + pg_dy + pg_h - 1),
                fill=t.color("track"),
            )
        # VIZ-2: synthetic-panel micro-caption — single line in the empty band
        # below the progress track (y≈567..598); no v5-specced pixel moves.
        synth = [p.label for p in self.panels if p.timeline.meta.get("synthetic")]
        if synth:
            caption = (
                " · ".join(n.lower() for n in synth)
                + ": synthetic chunk timing · measured walls"
            )
            d.text(
                (lay.pad, lay.height - t.get("caption", "bottom", default=26)),
                caption,
                font=t.font("body", t.get("caption", "size", default=13)),
                fill=t.color("subink"),
            )
        return img

    # ------------------------------------------------------------- frames ---

    def race_frame(self, t_s: float) -> Image.Image:
        t, lay = self.theme, self.layout
        img = self._base.copy()
        d = ImageDraw.Draw(img)
        timer_font = t.font("headline", t.get("timer", "size"))
        done_font = t.font("body_semibold", t.get("timer", "done_size"))
        size = lay.panel_size
        timer_dy = t.get("timer", "dy")
        done_dx, done_dy = t.get("timer", "done_dx"), t.get("timer", "done_dy")
        pg_dy, pg_h = t.get("progress", "dy"), t.get("progress", "height")
        t_ms = t_s * 1000.0
        for panel, (x, y) in zip(self.panels, lay.panel_xy):
            img.paste(Image.fromarray(panel.content_at(t_ms)), (x, y))
            if self._diff_cells is not None:
                self._draw_diff(img, panel, x, y, t_ms)
            shown = min(t_s, panel.wall_s)
            label = fmt_secs(shown)
            ty = y + size + timer_dy
            d.text((x, ty), label, font=timer_font, fill=panel.accent)
            if t_s >= panel.wall_s:
                tw = d.textlength(label, font=timer_font)
                d.text(
                    (x + tw + done_dx, ty + done_dy),
                    "done",
                    font=done_font,
                    fill=t.color("subink"),
                )
            frac = min(t_s / panel.wall_s, 1.0)
            fill_w = round(size * frac)
            if fill_w > 0:
                d.rectangle(
                    (x, y + size + pg_dy, x + fill_w - 1, y + size + pg_dy + pg_h - 1),
                    fill=panel.accent,
                )
        return img

    def _draw_diff(self, img, panel, x, y, t_ms):
        t = self.theme
        outline = t.accent(t.get("diff", "outline"))
        w = t.get("diff", "outline_px")
        cpx = panel.cpx
        d = ImageDraw.Draw(img)
        revealed = panel.t_grid <= t_ms
        for gz, gx in zip(*np.nonzero(self._diff_cells & revealed)):
            x0, y0 = x + gx * cpx, y + gz * cpx
            d.rectangle((x0, y0, x0 + cpx - 1, y0 + cpx - 1), outline=outline, width=w)

    # ------------------------------------------------------------ endcard ---

    def endcard_frame(self) -> Image.Image:
        if self._endcard is not None:
            return self._endcard
        t, lay, cfg = self.theme, self.layout, self.cfg
        img = Image.new("RGB", (lay.width, lay.height), t.color("bg"))
        d = ImageDraw.Draw(img)
        ec = lambda k: t.get("endcard", k)

        walls = [p.wall_s for p in self.panels]
        hero_i = walls.index(min(walls))
        hero = self.panels[hero_i]
        headline = cfg.endcard_headline
        if headline == "auto":
            ratio = walls[cfg.endcard_baseline] / hero.wall_s
            headline = f"{ratio:.1f}× faster"

        d.text(
            tuple(ec("wordmark_xy")),
            cfg.endcard_wordmark,
            font=t.font("headline", ec("wordmark_size")),
            fill=t.color("ink"),
        )
        pad = lay.pad
        d.text(
            (pad, ec("headline_y")),
            headline,
            font=t.font("headline", ec("headline_size")),
            fill=hero.accent,
        )
        sub_y = ec("headline_y") + ec("headline_size") + ec("sub_gap")
        d.text(
            (pad, sub_y),
            cfg.endcard_sub,
            font=t.font("body", ec("sub_size")),
            fill=t.color("subink"),
        )
        foot_font = t.font("body_semibold", ec("footer_size"))
        fy = lay.height - ec("footer_bottom")
        fx = float(pad)
        for p in self.panels:
            name = p.label.lower()
            d.text((fx, fy), name, font=foot_font, fill=p.accent)
            fx += d.textlength(name, font=foot_font) + ec("footer_pair_gap")
            val = fmt_secs(p.wall_s)
            d.text((fx, fy), val, font=foot_font, fill=t.color("ink"))
            fx += d.textlength(val, font=foot_font) + ec("footer_gap")
        self._endcard = img
        return img

    # ---------------------------------------------------------- animation ---

    def frames(self, fps: int):
        """Full animation: race at 1x, hold, then endcard."""
        n_race = math.ceil(self.race_end * fps) + 1
        for i in range(n_race):
            yield self.race_frame(i / fps)
        final = self.race_frame(self.race_end)
        for _ in range(round(self.cfg.hold_s * fps)):
            yield final
        if self.cfg.endcard_enabled:
            card = self.endcard_frame()
            for _ in range(round(self.cfg.endcard_hold_s * fps)):
                yield card

    def frame_count(self, fps: int) -> int:
        n = math.ceil(self.race_end * fps) + 1 + round(self.cfg.hold_s * fps)
        if self.cfg.endcard_enabled:
            n += round(self.cfg.endcard_hold_s * fps)
        return n
