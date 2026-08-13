"""hcviz — bench timeline → GIF/MP4 race renderer.

Measurement and visualization are fully separated: benches emit timeline
captures (HC_BENCH_TIMELINE waterfall files, probe logs), hcviz re-draws them
from converted timeline.json files. Everything visual comes from a theme file
(themes/ivory.json is the shipped v5 spec) — no design values in code.
"""

__version__ = "0.1.0"
