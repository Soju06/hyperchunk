#!/usr/bin/env bash
# ADR-004 D3/Verification: 산출물에 FMA 명령이 없어야 한다.
# FMA contraction 은 중간 반올림을 없애 지형 비트를 조용히 바꾼다 (ADR-002 Pitfall 1).
set -euo pipefail

LIB="${1:-build/core/libhyperchunk.a}"

if [ ! -f "$LIB" ]; then
  echo "FAIL: $LIB not found (build first: cmake --build build)"
  exit 1
fi

# grep -q 를 파이프에 직접 쓰면 첫 매치에서 조기 종료 → objdump SIGPIPE(141)
# → pipefail 아래에서 매치가 '있을 때' 파이프라인이 실패해 FAIL 경로를
# 건너뛰는 false-PASS 가 난다. 출력을 전부 소비해 캡처한 뒤 판정한다.

# 검사하는 mnemonic 은 x86-64 기준이다. 포맷이 다르면 정규식 자체가 무의미하다.
format="$(objdump -f "$LIB")"
if ! grep -q 'elf64-x86-64' <<<"$format"; then
  echo "FAIL: $LIB has no elf64-x86-64 members (empty archive or foreign arch)"
  exit 1
fi

# slim LTO(GIMPLE bytecode) 멤버는 네이티브 코드가 없어 디스어셈블 검사가
# 공허해지고, FMA contraction 은 링크 시점으로 이동한다. -flto/IPO 금지.
lto="$(objdump -h "$LIB" | grep -F '.gnu.lto' || true)"
if [ -n "$lto" ]; then
  echo "FAIL: $LIB contains LTO bytecode sections — disassembly gate is vacuous under -flto/IPO"
  exit 1
fi

disasm="$(objdump -d "$LIB")"

matches="$(grep -E '\bvfmadd|\bvfmsub|\bvfnmadd|\bvfnmsub' <<<"$disasm" || true)"
if [ -n "$matches" ]; then
  echo "FAIL: FMA instruction found in $LIB"
  head -5 <<<"$matches"
  exit 1
fi

# 명령이 0개 디스어셈블됐다면 PASS 는 공허하다 — 명시적으로 거부한다.
ninsn="$(grep -cE '^[[:space:]]+[0-9a-f]+:' <<<"$disasm" || true)"
if [ "$ninsn" -eq 0 ]; then
  echo "FAIL: no instructions disassembled from $LIB — vacuous check"
  exit 1
fi

# AVX-512 TU (P2-10) 비공허 확인: df_simd_avx512.o 멤버가 존재하고 실제
# zmm 코드를 담아야 한다 — 멤버 누락/스텁-공허 시 이 게이트가 avx512
# 커널을 전혀 판정하지 않는 false-PASS 채널을 막는다 (컴파일은 호스트
# CPU 무관 — Zen3 로컬에서도 검사 유효).
avx512_dis="$(awk '/^df_simd_avx512(\.c)?\.o:/{f=1} f && /\.o:/ && !/^df_simd_avx512(\.c)?\.o:/{f=0} f' <<<"$disasm")"
n512="$(grep -cE '^[[:space:]]+[0-9a-f]+:' <<<"$avx512_dis" || true)"
if [ "$n512" -eq 0 ]; then
  echo "FAIL: df_simd_avx512.o has no instructions in $LIB — avx512 gate vacuous"
  exit 1
fi
if ! grep -q 'zmm' <<<"$avx512_dis"; then
  echo "FAIL: df_simd_avx512.o contains no zmm code in $LIB — avx512 kernel missing"
  exit 1
fi

echo "PASS: no FMA instructions in $LIB ($ninsn instructions inspected; avx512 TU $n512 insn incl.)"
