"""GIF/MP4 encoding via ffmpeg raw-RGB pipe."""

from __future__ import annotations

import subprocess
from pathlib import Path

import numpy as np

MP4_WIDTH, MP4_HEIGHT = 1920, 1080  # X/YouTube target


class VideoError(Exception):
    pass


def _ffmpeg_cmd(out: Path, w: int, h: int, fps: int, bg_rgb) -> list:
    base = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{w}x{h}",
        "-r", str(fps), "-i", "-", "-an",
    ]
    suffix = out.suffix.lower()
    if suffix == ".gif":
        return base + [
            "-vf",
            "split[a][b];[a]palettegen=max_colors=192:stats_mode=diff[p];"
            "[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle",
            "-loop", "0", str(out),
        ]
    if suffix == ".mp4":
        bg_hex = "0x{:02X}{:02X}{:02X}".format(*bg_rgb)
        return base + [
            "-vf",
            f"scale={MP4_WIDTH}:-2:flags=lanczos,"
            f"pad={MP4_WIDTH}:{MP4_HEIGHT}:(ow-iw)/2:(oh-ih)/2:color={bg_hex}",
            "-c:v", "libx264", "-crf", "18", "-preset", "slow",
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(out),
        ]
    raise VideoError(f"unsupported output extension: {out.name} (want .gif or .mp4)")


def encode(frame_factory, w: int, h: int, fps: int, outs, bg_rgb=(0, 0, 0)):
    """Encode each output path from a fresh frame iterator.

    frame_factory: () -> iterator of PIL RGB images sized (w, h).
    """
    for out in outs:
        out = Path(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        cmd = _ffmpeg_cmd(out, w, h, fps, bg_rgb)
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        try:
            for img in frame_factory():
                proc.stdin.write(np.asarray(img, dtype=np.uint8).tobytes())
            proc.stdin.close()
        except BrokenPipeError:
            pass
        rc = proc.wait()
        if rc != 0:
            raise VideoError(f"ffmpeg failed ({rc}) for {out}")
