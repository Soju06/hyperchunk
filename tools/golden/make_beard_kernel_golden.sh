#!/usr/bin/env bash
# Regenerate golden/rng/beard_kernel_bits.txt from the pinned vanilla server
# jar and rebuild core/src/beard_kernel.h from it (Task 14). One command,
# idempotent, same pattern as make_mth_golden.sh: reflection-dump
# Beardifier.BEARD_KERNEL (values depend on the JDK Math.pow intrinsic, so
# the C side pins the observed bits instead of recomputing). Seed-independent.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
CP="$(cat "$CP_FILE")"

BUILD="$HERE/work/beard-classes"
mkdir -p "$BUILD"
javac -cp "$CP" -d "$BUILD" "$HERE/KernelProbe.java"
java -cp "$BUILD:$CP" KernelProbe "$ROOT/golden/rng"

python3 - "$ROOT/golden/rng/beard_kernel_bits.txt" \
    "$ROOT/core/src/beard_kernel.h" <<'EOF'
import sys

bits_path, header_path = sys.argv[1], sys.argv[2]
bits = [int(line, 16) for line in open(bits_path)]
assert len(bits) == 13824, len(bits)

lines = [
    "/* Beardifier.BEARD_KERNEL — 실서버 26.2 Beardifier <clinit> 리플렉션",
    " * 덤프 (tools/golden/make_beard_kernel_golden.sh). 값 = (float)Math.pow(E,",
    " * -((dx)^2+(dy+0.5)^2+(dz)^2)/16), 인덱스 [zi*24*24 + xi*24 + yi],",
    " * dx=xi-12 등. JDK Math.pow 인트린식 의존이라 재계산 대신 비트 고정. */",
    "#ifndef HC_BEARD_KERNEL_H",
    "#define HC_BEARD_KERNEL_H",
    "",
    "#include <stdint.h>",
    "",
    "static const uint32_t HC_BEARD_KERNEL_BITS[13824] = {",
]
for i in range(0, 13824, 8):
    row = ", ".join(f"0x{b:08x}" for b in bits[i : i + 8])
    lines.append(f"    {row},")
lines += ["};", "", "#endif /* HC_BEARD_KERNEL_H */", ""]
open(header_path, "w").write("\n".join(lines))
print(f"regenerated {header_path}")
EOF
