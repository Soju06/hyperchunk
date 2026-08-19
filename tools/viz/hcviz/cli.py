"""hcviz command line: render / still / validate / convert / synth / tile."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_VIZ_DIR = Path(__file__).resolve().parents[1]
_SCHEMA = _VIZ_DIR / "schema" / "timeline.schema.json"
_DEFAULT_THEME = _VIZ_DIR / "themes" / "ivory.json"


def _overrides(args) -> dict:
    return {
        "theme": getattr(args, "theme", None),
        "layout": getattr(args, "layout", None),
        "view": getattr(args, "view", None),
        "fps": getattr(args, "fps", None),
    }


def cmd_render(args) -> int:
    from .config import RaceConfig
    from .render import Renderer
    from .video import encode

    cfg = RaceConfig.load(args.race, _overrides(args))
    r = Renderer(cfg)
    total = r.frame_count(cfg.fps)
    print(
        f"render: {cfg.layout}/{cfg.view} {r.layout.width}x{r.layout.height} "
        f"@{cfg.fps}fps, race {r.race_end:.3f}s, {total} frames"
    )
    encode(
        lambda: r.frames(cfg.fps),
        r.layout.width,
        r.layout.height,
        cfg.fps,
        args.out,
        bg_rgb=cfg.theme.color("bg"),
    )
    for out in args.out:
        print(f"wrote {out}")
    return 0


def cmd_still(args) -> int:
    from .config import RaceConfig
    from .render import Renderer

    cfg = RaceConfig.load(args.race, _overrides(args))
    r = Renderer(cfg)
    if args.t in ("endcard", "end"):
        img = r.endcard_frame() if args.t == "endcard" else r.race_frame(r.race_end)
    else:
        img = r.race_frame(float(args.t))
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out)
    print(f"wrote {args.out}")
    return 0


def cmd_validate(args) -> int:
    from .timeline import schema_errors, semantic_errors

    failed = 0
    for path in args.files:
        p = Path(path)
        try:
            data = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"FAIL {p}: {e}")
            failed += 1
            continue
        errors = schema_errors(data, _SCHEMA) + semantic_errors(data)
        if errors:
            failed += 1
            print(f"FAIL {p}:")
            for e in errors[:10]:
                print(f"  - {e}")
        else:
            n = len(data["chunks"])
            xs = [c["cx"] for c in data["chunks"]]
            zs = [c["cz"] for c in data["chunks"]]
            synth = " synthetic" if data.get("meta", {}).get("synthetic") else ""
            print(
                f"OK   {p}: {data['system']} wall={data['wall_s']}s "
                f"chunks={n} cx=[{min(xs)},{max(xs)}] cz=[{min(zs)},{max(zs)}]{synth}"
            )
    return 1 if failed else 0


def cmd_convert(args) -> int:
    from .convert import waterfall_to_timeline

    data = waterfall_to_timeline(
        args.waterfall,
        system=args.system,
        display_name=args.display_name,
        stage=args.stage,
        event=args.event,
        stage1=args.stage1,
        seed=args.seed,
        result_json=args.result,
    )
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, ensure_ascii=False) + "\n")
    print(
        f"wrote {out}: {data['system']} wall={data['wall_s']}s "
        f"chunks={len(data['chunks'])} event={args.event}"
    )
    return 0


def cmd_synth(args) -> int:
    from .timeline import synth

    data = synth(
        args.pattern,
        args.wall,
        seed=args.seed,
        size=args.size,
        system=args.system,
        display_name=args.display_name,
        stage=args.stage,
        workers=args.workers,
    )
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, ensure_ascii=False) + "\n")
    print(f"wrote {out}: synthetic '{args.pattern}' wall={args.wall}s")
    return 0


def cmd_tile(args) -> int:
    from .mca_tile import render_tile
    from .theme import Theme

    theme = Theme.load(args.theme or _DEFAULT_THEME)
    img = render_tile(args.mca, theme)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out)
    print(f"wrote {args.out} ({img.size[0]}x{img.size[1]})")
    return 0


def _add_style_flags(p):
    p.add_argument("--theme", help="theme file override (JSON/YAML)")
    p.add_argument("--layout", help="layout override: race3|race4|single|vs2")
    p.add_argument("--view", help="view override: reveal|heat")
    p.add_argument("--fps", type=int, help="fps override")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="hcviz", description="bench timeline → GIF/MP4 race renderer"
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("render", help="render a race.yaml to GIF/MP4")
    p.add_argument("race")
    p.add_argument("--out", action="append", required=True,
                   help=".gif or .mp4 (repeatable)")
    _add_style_flags(p)
    p.set_defaults(fn=cmd_render)

    p = sub.add_parser("still", help="render a single frame at time t")
    p.add_argument("race")
    p.add_argument("-t", required=True,
                   help="seconds, or 'end' (final race frame) / 'endcard'")
    p.add_argument("--out", required=True)
    _add_style_flags(p)
    p.set_defaults(fn=cmd_still)

    p = sub.add_parser("validate", help="validate timeline.json files")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_validate)

    p = sub.add_parser("convert", help="HC_BENCH_TIMELINE waterfall → timeline.json")
    p.add_argument("waterfall")
    p.add_argument("--out", required=True)
    p.add_argument("--system", required=True)
    p.add_argument("--display-name", required=True)
    p.add_argument("--stage", default="")
    p.add_argument("--event", default="complete",
                   choices=("complete", "strict", "serialize", "deco", "chain"))
    p.add_argument("--stage1", choices=("chain",),
                   help="also emit t_stage1_ms per chunk (two-stage reveal)")
    p.add_argument("--seed", type=int)
    p.add_argument("--result", help="bench stdout JSON (adds seed/canonical/pass)")
    p.set_defaults(fn=cmd_convert)

    p = sub.add_parser("synth", help="generate a synthetic timeline")
    p.add_argument("--pattern", required=True, choices=("scan", "wave", "burst"))
    p.add_argument("--wall", type=float, required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--size", type=int, default=32)
    p.add_argument("--system")
    p.add_argument("--display-name")
    p.add_argument("--stage", default="")
    p.add_argument("--workers", type=int)
    p.set_defaults(fn=cmd_synth)

    p = sub.add_parser("tile", help=".mca → top-view tile png")
    p.add_argument("mca")
    p.add_argument("--out", required=True)
    p.add_argument("--theme")
    p.set_defaults(fn=cmd_tile)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
