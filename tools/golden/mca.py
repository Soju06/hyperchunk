"""Minimal Anvil (.mca) region + NBT reader for golden-baseline tooling.

Covers exactly what the golden scripts need:
- region header parsing (offset table, timestamp table)
- per-chunk payload extraction and decompression (gzip/zlib/none; lz4 if the
  optional lz4 package is present — 26.2 default is zlib id=2)
- a full big-endian NBT parser to python objects
- LastUpdate byte-masking for canonical (timestamp-free) payload hashing

Not a writer. The golden baseline is captured from vanilla, never synthesized.
"""

from __future__ import annotations

import gzip
import struct
import zlib
from dataclasses import dataclass

SECTOR = 4096

# ---------------------------------------------------------------- region ---


@dataclass
class ChunkEntry:
    index: int          # 0..1023, index = (cx & 31) + (cz & 31) * 32
    x: int              # chunk x within region (0..31)
    z: int              # chunk z within region (0..31)
    timestamp: int      # epoch seconds from the header timestamp table
    compression: int    # 1 gzip, 2 zlib, 3 none, 4 lz4, 127 custom
    raw: bytes          # compressed payload as stored
    payload: bytes      # decompressed NBT payload


def _decompress(comp: int, data: bytes) -> bytes:
    if comp == 2:
        return zlib.decompress(data)
    if comp == 1:
        return gzip.decompress(data)
    if comp == 3:
        return data
    if comp == 4:
        try:
            import lz4.block  # type: ignore
        except ImportError as e:
            raise RuntimeError(
                "chunk uses lz4 region compression; `pip install lz4` to parse"
            ) from e
        return lz4.block.decompress(data)
    raise RuntimeError(f"unsupported region compression id {comp}")


def read_region(path: str) -> dict[int, ChunkEntry]:
    """Return {index: ChunkEntry} for all present chunks."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 2 * SECTOR:
        return {}
    chunks: dict[int, ChunkEntry] = {}
    for i in range(1024):
        off_raw = struct.unpack_from(">I", blob, i * 4)[0]
        sector_off, sector_count = off_raw >> 8, off_raw & 0xFF
        if sector_off == 0 and sector_count == 0:
            continue
        ts = struct.unpack_from(">I", blob, SECTOR + i * 4)[0]
        base = sector_off * SECTOR
        (length,) = struct.unpack_from(">I", blob, base)
        comp = blob[base + 4]
        raw = blob[base + 5 : base + 4 + length]
        chunks[i] = ChunkEntry(
            index=i, x=i % 32, z=i // 32, timestamp=ts,
            compression=comp, raw=raw, payload=_decompress(comp, raw),
        )
    return chunks


# ------------------------------------------------------------------- NBT ---

TAG_NAMES = {
    0: "End", 1: "Byte", 2: "Short", 3: "Int", 4: "Long", 5: "Float",
    6: "Double", 7: "ByteArray", 8: "String", 9: "List", 10: "Compound",
    11: "IntArray", 12: "LongArray",
}


class _Reader:
    def __init__(self, buf: bytes):
        self.buf = buf
        self.pos = 0

    def take(self, n: int) -> bytes:
        b = self.buf[self.pos : self.pos + n]
        self.pos += n
        return b

    def u1(self) -> int: return self.take(1)[0]
    def u2(self) -> int: return struct.unpack(">H", self.take(2))[0]
    def i1(self) -> int: return struct.unpack(">b", self.take(1))[0]
    def i2(self) -> int: return struct.unpack(">h", self.take(2))[0]
    def i4(self) -> int: return struct.unpack(">i", self.take(4))[0]
    def i8(self) -> int: return struct.unpack(">q", self.take(8))[0]
    def f4(self) -> float: return struct.unpack(">f", self.take(4))[0]
    def f8(self) -> float: return struct.unpack(">d", self.take(8))[0]

    def string(self) -> str:
        return self.take(self.u2()).decode("utf-8", errors="replace")

    def payload(self, tag: int):
        if tag == 1: return self.i1()
        if tag == 2: return self.i2()
        if tag == 3: return self.i4()
        if tag == 4: return self.i8()
        if tag == 5: return self.f4()
        if tag == 6: return self.f8()
        if tag == 7: return self.take(self.i4())
        if tag == 8: return self.string()
        if tag == 9:
            etag = self.u1()
            n = self.i4()
            return [self.payload(etag) for _ in range(n)]
        if tag == 10:
            out = {}
            while True:
                t = self.u1()
                if t == 0:
                    return out
                name = self.string()
                out[name] = self.payload(t)
        if tag == 11:
            n = self.i4()
            return list(struct.unpack(f">{n}i", self.take(4 * n)))
        if tag == 12:
            n = self.i4()
            return list(struct.unpack(f">{n}q", self.take(8 * n)))
        raise RuntimeError(f"bad NBT tag {tag} at {self.pos}")


def parse_nbt(payload: bytes):
    """Parse an uncompressed NBT payload; returns (root_name, root_dict)."""
    r = _Reader(payload)
    tag = r.u1()
    if tag != 10:
        raise RuntimeError(f"root tag is {TAG_NAMES.get(tag, tag)}, expected Compound")
    name = r.string()
    return name, r.payload(10)


# ---------------------------------------------------------- canonicalize ---

# TAG_Long(0x04), name length 10, "LastUpdate", followed by the 8-byte value.
_LAST_UPDATE = b"\x04\x00\x0aLastUpdate"


def mask_last_update(payload: bytes) -> bytes:
    """Zero the root LastUpdate value (save-time game tick, not worldgen)."""
    i = payload.find(_LAST_UPDATE)
    if i < 0:
        return payload
    j = i + len(_LAST_UPDATE)
    return payload[:j] + b"\x00" * 8 + payload[j + 8 :]


def nbt_diff(a, b, path="", out=None, limit=200):
    """Recursive structural diff; returns list of 'path: a != b' strings."""
    if out is None:
        out = []
    if len(out) >= limit:
        return out
    if type(a) is not type(b):
        out.append(f"{path}: type {type(a).__name__} != {type(b).__name__}")
        return out
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a:
                out.append(f"{path}.{k}: missing in A")
            elif k not in b:
                out.append(f"{path}.{k}: missing in B")
            else:
                nbt_diff(a[k], b[k], f"{path}.{k}", out, limit)
            if len(out) >= limit:
                break
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append(f"{path}: list length {len(a)} != {len(b)}")
        else:
            for i, (ea, eb) in enumerate(zip(a, b)):
                nbt_diff(ea, eb, f"{path}[{i}]", out, limit)
                if len(out) >= limit:
                    break
    else:
        if a != b:
            out.append(f"{path}: {a!r} != {b!r}")
    return out
