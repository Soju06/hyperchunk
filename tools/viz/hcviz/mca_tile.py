""".mca region → 352×352 top-view tile (muted palette + slope shading).

Reuses tools/golden/mca.py (Anvil container + NBT reader). Surface comes from
the serialized Heightmaps: MOTION_BLOCKING is the visible top (terrain, water,
leaves), OCEAN_FLOOR the floor beneath fluids — their difference is water
depth. The top block is looked up in the owning section's palette.
"""

from __future__ import annotations

import hashlib
import importlib.util
import sys
from pathlib import Path

import numpy as np
from PIL import Image

_GOLDEN_DIR = Path(__file__).resolve().parents[2] / "golden"


class McaTileError(Exception):
    pass


def _load_golden_mca():
    path = _GOLDEN_DIR / "mca.py"
    if not path.is_file():
        raise McaTileError(f"tools/golden/mca.py not found at {path}")
    if "hc_golden_mca" in sys.modules:
        return sys.modules["hc_golden_mca"]
    spec = importlib.util.spec_from_file_location("hc_golden_mca", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["hc_golden_mca"] = mod  # dataclasses looks the module up by name
    spec.loader.exec_module(mod)
    return mod


# Category codes into the theme terrain palette.
_CATS = (
    "paper", "water_deep", "water_shallow", "sand", "grass",
    "grass_alt", "grass_dark", "stone", "snow",
)
_CAT_CODE = {name: i for i, name in enumerate(_CATS)}

_EXACT = {
    "grass_block": "grass",
    "dirt": "grass_alt", "coarse_dirt": "grass_alt", "rooted_dirt": "grass_alt",
    "podzol": "grass_alt", "mycelium": "grass_alt", "dirt_path": "grass_alt",
    "mud": "grass_dark", "muddy_mangrove_roots": "grass_dark", "farmland": "grass_alt",
    "sand": "sand", "red_sand": "sand", "sandstone": "sand",
    "smooth_sandstone": "sand", "suspicious_sand": "sand",
    "gravel": "stone", "suspicious_gravel": "stone", "clay": "stone",
    "stone": "stone", "deepslate": "stone", "granite": "stone", "diorite": "stone",
    "andesite": "stone", "calcite": "stone", "tuff": "stone", "bedrock": "stone",
    "dripstone_block": "stone", "pointed_dripstone": "stone",
    "cobblestone": "stone", "mossy_cobblestone": "stone", "obsidian": "stone",
    "magma_block": "stone", "basalt": "stone", "smooth_basalt": "stone",
    "blackstone": "stone", "lava": "stone",
    "water": "water_shallow", "flowing_water": "water_shallow",
    "bubble_column": "water_shallow",
    "snow": "snow", "snow_block": "snow", "powder_snow": "snow",
    "ice": "water_shallow", "packed_ice": "snow", "blue_ice": "water_shallow",
    "frosted_ice": "water_shallow",
    "moss_block": "grass_dark", "moss_carpet": "grass_dark",
    "mangrove_roots": "grass_dark", "cactus": "grass_dark",
    "bamboo": "grass_dark", "sugar_cane": "grass_alt",
    "pumpkin": "grass_alt", "melon": "grass_alt",
    "terracotta": "sand",
}
_SUFFIX = (
    ("_leaves", "grass_dark"), ("_log", "grass_dark"), ("_wood", "grass_dark"),
    ("_terracotta", "sand"), ("_sapling", "grass_dark"), ("_ore", "stone"),
)


def _category(name: str) -> int:
    short = name.removeprefix("minecraft:")
    cat = _EXACT.get(short)
    if cat is None:
        for suf, c in _SUFFIX:
            if short.endswith(suf):
                cat = c
                break
    return _CAT_CODE[cat or "stone"]


def _unpack(longs, bits: int, count: int) -> np.ndarray:
    """1.16+ non-spanning unpack: 64//bits entries per long, LSB-first."""
    arr = np.asarray(longs, dtype=np.int64).view(np.uint64)
    vpl = 64 // bits
    shifts = (np.arange(vpl, dtype=np.uint64) * np.uint64(bits))[None, :]
    vals = (arr[:, None] >> shifts) & np.uint64((1 << bits) - 1)
    return vals.reshape(-1)[:count].astype(np.int32)


def _chunk_categories(root, mb: np.ndarray, of: np.ndarray) -> np.ndarray:
    """Per-column category codes for one chunk (256 values, z*16+x order)."""
    cats = np.full(256, _CAT_CODE["paper"], dtype=np.uint8)
    empty = mb == 0
    is_water = (mb - of > 0) & ~empty
    cats[is_water] = _CAT_CODE["water_shallow"]

    sections = {}
    for sec in root.get("sections", []):
        if "block_states" in sec:
            sections[sec["Y"]] = sec["block_states"]

    ys = mb - 65  # absolute y of the top blocking block (raw value = y+1+64)
    need = ~is_water & ~empty
    sec_ids = ys >> 4
    cols = np.arange(256, dtype=np.int32)
    for sy in np.unique(sec_ids[need]):
        bs = sections.get(int(sy))
        sel = need & (sec_ids == sy)
        if bs is None:
            cats[sel] = _CAT_CODE["stone"]
            continue
        pal = bs["palette"]
        pal_cats = np.array([_category(p["Name"]) for p in pal], dtype=np.uint8)
        if "data" not in bs:
            cats[sel] = pal_cats[0]
            continue
        bits = max(4, (len(pal) - 1).bit_length())
        data = _unpack(bs["data"], bits, 4096)
        cells = ((ys[sel] & 15) << 8) | cols[sel]
        cats[sel] = pal_cats[data[cells]]
    return cats


def render_tile(mca_path, theme) -> Image.Image:
    mca = _load_golden_mca()
    region = mca.read_region(str(mca_path))
    if not region:
        raise McaTileError(f"{mca_path}: no chunks present")

    H = np.zeros((512, 512), dtype=np.int32)
    DEPTH = np.zeros((512, 512), dtype=np.int32)
    CAT = np.full((512, 512), _CAT_CODE["paper"], dtype=np.uint8)
    for entry in region.values():
        _, root = mca.parse_nbt(entry.payload)
        hm = root.get("Heightmaps", {})
        if "MOTION_BLOCKING" not in hm:
            continue
        mb = _unpack(hm["MOTION_BLOCKING"], 9, 256)
        of = _unpack(hm.get("OCEAN_FLOOR", hm["MOTION_BLOCKING"]), 9, 256)
        cats = _chunk_categories(root, mb, of)
        x0, z0 = entry.x * 16, entry.z * 16
        H[z0 : z0 + 16, x0 : x0 + 16] = mb.reshape(16, 16)
        DEPTH[z0 : z0 + 16, x0 : x0 + 16] = (mb - of).reshape(16, 16)
        CAT[z0 : z0 + 16, x0 : x0 + 16] = cats.reshape(16, 16)

    tr = theme.get("terrain")
    shallow = _CAT_CODE["water_shallow"]
    deep = (CAT == shallow) & (DEPTH >= tr["water_deep_threshold"])
    CAT[deep] = _CAT_CODE["water_deep"]

    palette = np.zeros((len(_CATS), 3), dtype=np.float64)
    for name, code in _CAT_CODE.items():
        palette[code] = theme.color("paper") if name == "paper" else tr[name]

    # slope shading from the x-gradient of the surface height; flat on water
    dx = H - np.roll(H, 1, axis=1)
    dx[:, 0] = 0
    shade = np.clip(1.0 + 0.05 * dx, tr["shade_min"], tr["shade_max"])
    water = (CAT == shallow) | (CAT == _CAT_CODE["water_deep"])
    shade[water | (CAT == _CAT_CODE["paper"])] = 1.0

    rgb = palette[CAT] * shade[..., None]
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    psize = theme.get("panel", "size")
    return img.resize((psize, psize), Image.NEAREST)


def tile_for_mca(mca_path, theme, tiles_dir) -> Path:
    """Render (or reuse cached) tile png for an mca file."""
    mca_path = Path(mca_path)
    if not mca_path.is_file():
        raise McaTileError(f"mca not found: {mca_path}")
    tiles_dir = Path(tiles_dir)
    tiles_dir.mkdir(parents=True, exist_ok=True)
    stat = mca_path.stat()
    key = hashlib.sha1(
        f"{mca_path.resolve()}|{stat.st_mtime_ns}|{stat.st_size}|{theme.path}".encode()
    ).hexdigest()[:12]
    out = tiles_dir / f"{mca_path.stem}-{key}.png"
    if not out.is_file():
        render_tile(mca_path, theme).save(out)
    return out
