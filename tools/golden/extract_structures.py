#!/usr/bin/env python3
"""Extract the golden structure inputs for the Task-14 full-region gate.

Outputs (golden/structures/, local-only like region-ref/ — regenerable
from the golden .mca by this tool; hashes printed for the completion note):

  c.<x>.<z>.starts.nbt   raw byte span of the chunk's structures.starts
                         compound payload, wrapped as a nameless root
                         compound (\\x0a\\x00\\x00 <entries> \\x00). VERBATIM
                         golden bytes — never re-serialized. This is the
                         ADR-003 D4 replay input: piece lists come from the
                         recorded run (jigsaw assembly excluded from scope),
                         placement is reimplemented in C.

  references.txt         golden References table, one line per
                         (chunk, structure, packed long) in file order:
                         "<cx> <cz> <structure> <long>". Consumed as
                         (a) cross-validation for derived references and
                         (b) the only source for out-of-region starts whose
                         piece lists were not captured (r.0.0-external
                         jigsaw starts, e.g. trial_chambers (13,35) — its
                         start chunk lives in r.0.1 which was not recorded).

Byte-span extraction: the NBT scanner below re-walks the payload tracking
offsets; the starts value span is copied straight out of the payload.
"""

import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mca import parse_nbt, read_region

ROOT = Path(__file__).resolve().parents[2]
REGION = ROOT / "golden" / "seed1234567890_r.0.0.mca"
OUT = ROOT / "golden" / "structures"


class Scanner:
    """NBT walker that can report the byte span of a named root path."""

    def __init__(self, buf: bytes):
        self.buf = buf
        self.pos = 0

    def u1(self):
        v = self.buf[self.pos]
        self.pos += 1
        return v

    def u2(self):
        v = struct.unpack_from(">H", self.buf, self.pos)[0]
        self.pos += 2
        return v

    def i4(self):
        v = struct.unpack_from(">i", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def skip(self, n):
        self.pos += n

    def name(self):
        n = self.u2()
        s = self.buf[self.pos:self.pos + n].decode()
        self.pos += n
        return s

    def skip_payload(self, tag):
        if tag == 1:
            self.skip(1)
        elif tag == 2:
            self.skip(2)
        elif tag in (3, 5):
            self.skip(4)
        elif tag in (4, 6):
            self.skip(8)
        elif tag == 7:
            self.skip(self.i4())
        elif tag == 8:
            self.skip(self.u2())
        elif tag == 9:
            et = self.u1()
            n = self.i4()
            for _ in range(n):
                self.skip_payload(et)
        elif tag == 10:
            while True:
                t = self.u1()
                if t == 0:
                    break
                self.name()
                self.skip_payload(t)
        elif tag == 11:
            self.skip(4 * self.i4())
        elif tag == 12:
            self.skip(8 * self.i4())
        else:
            raise ValueError(f"tag {tag} @ {self.pos}")

    def find_span(self, path):
        """Return (start, end) byte span of the payload of key path
        (list of names) under the root compound."""
        tag = self.u1()
        assert tag == 10
        self.name()
        return self._find_in_compound(path)

    def _find_in_compound(self, path):
        while True:
            t = self.u1()
            if t == 0:
                raise KeyError(path)
            nm = self.name()
            if nm == path[0]:
                if len(path) == 1:
                    start = self.pos
                    self.skip_payload(t)
                    return t, start, self.pos
                assert t == 10
                return self._find_in_compound(path[1:])
            self.skip_payload(t)


def main() -> int:
    chunks = read_region(str(REGION))
    OUT.mkdir(parents=True, exist_ok=True)

    ref_lines = []
    n_starts = 0
    for idx in sorted(chunks):
        e = chunks[idx]
        root = parse_nbt(e.payload)
        if isinstance(root, tuple):
            root = root[1]
        st = root.get("structures", {})
        if st.get("starts"):
            tag, a, b = Scanner(e.payload).find_span(["structures", "starts"])
            assert tag == 10
            # span of a compound payload already includes its END byte
            frag = b"\x0a\x00\x00" + e.payload[a:b]
            # scanner span check: the span must reparse to the same tree
            path = OUT / f"c.{e.x}.{e.z}.starts.nbt"
            path.write_bytes(frag)
            sha = hashlib.sha256(frag).hexdigest()
            print(f"{sha}  structures/{path.name}")
            n_starts += 1
        for sname, arr in st.get("References", {}).items():
            for v in arr:
                ref_lines.append(f"{e.x} {e.z} {sname} {v & 0xFFFFFFFFFFFFFFFF:016x}")

    refs = OUT / "references.txt"
    with open(refs, "w") as f:
        f.write("# golden r.0.0 structures.References — cx cz structure "
                "packedlong(hex, x|z<<32)\n")
        for line in ref_lines:
            f.write(line + "\n")
    sha = hashlib.sha256(refs.read_bytes()).hexdigest()
    print(f"{sha}  structures/references.txt")
    print(f"extracted {n_starts} starts fragments, {len(ref_lines)} reference longs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
