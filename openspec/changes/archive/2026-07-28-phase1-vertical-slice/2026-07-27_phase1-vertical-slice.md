# Phase 1: Vertical Slice — 바닐라 패리티 관통

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** 순수 C로 5스테이지 월드젠을 최소 구현하고, 단일 청크의 region 출력이 바닐라 Java와 `sha256` 단위로 일치함을 증명한다. 성능은 목표가 아니다.

**Architecture:** `libhyperchunk` (순수 C, 의존성 `libc`만) + `hyperchunk` CLI + Java golden 생성기. 코어는 리전 단위 진입점만 노출하고 파일 I/O를 하지 않는다. 스칼라 참조 커널만 구현하며 SIMD는 Phase 2다.

**Tech Stack:** C11, gcc 13.3, CMake 3.28, Java 21 (golden 생성 전용), Python 3.12 (해시 비교 스크립트)

**Decisions:** [../../DECISIONS.md](/home/ubuntu/projects/hyperchunk/DECISIONS.md) — ADR-001~005

---

## Current context / assumptions

**확정된 결정 (ADR 참조):**

| 항목 | 값 | 출처 |
|---|---|---|
| 목적 | 기술 시연, ROI 무시 | ADR-001 D1 |
| 산출물 | 3-way 탑뷰 비교 숏폼 | ADR-001 D2 |
| 구현 | 밑바닥부터 순수 C | ADR-002 D1 |
| 파이프라인 | 5스테이지 전부 | ADR-002 D2 |
| 수용 기준 | region `sha256` 일치 | ADR-002 D3 |
| 범위 | 26.2 고정 / 오버월드만 / 구조물은 배치까지 | ADR-002 D4 + ADR-006 |
| 경계 | 리전 단위 C ABI만 | ADR-003 D2 |
| 커널 | Phase 1은 스칼라만 | ADR-004 (Phase 2) |
| FMA | 전면 금지 | ADR-004 D3 |

**Phase 1 범위에서 명시적으로 제외:**

- SIMD 커널 (AVX2/AVX-512) — Phase 2
- 멀티스레드 배치 스케줄러 — Phase 2
- JNI / Fabric 모드 — Phase 3
- 3-way GIF 제작 — Phase 3
- 성능 측정 및 비교 — Phase 2 이후

**환경 실측 (2026-07-27, 개발 박스):**

```
CPU     : AMD Ryzen 9 5900X (Zen 3), 22 vCPU 보고
ISA     : avx avx2 fma sse4_1 sse4_2 sse4a  (AVX-512 없음)
gcc     : 13.3.0        ✅
cmake   : 3.28.3        ✅
java    : 17.0.19       ⚠️ MC 26.2는 Java 25 필요 → Task 0에서 JDK 25 설치 (ADR-006 D2)
clang   : MISSING       (선택, gcc로 충분)
ninja   : MISSING       (선택, Makefile 생성기로 대체)
python3 : 3.12.3        ✅
RAM     : 47 GB
```

⚠️ **벤치 신뢰성 경고 (ADR-002 Pitfall 5):** 이 박스는 `hypervisor` 플래그가 있고 `lscpu`가 `Core(s) per socket: 22`, `Thread(s) per core: 1`, `L3 cache: 352 MiB`를 보고한다. 실제 5900X는 12코어/24스레드, L3 64MB다. SMT·캐시 토폴로지가 오보고되는 VM이므로 **이 박스의 사이클 카운트는 벤치 근거로 사용 불가.** Phase 1은 패리티만 다루므로 무해하지만, Phase 2 시작 전 베어메탈 또는 코어 핀닝 인스턴스를 확보해야 한다.

**가정:**

- 대상 버전은 26.2로 고정 (ADR-006 D1). 26.1부터 완전 비난독화라 매핑 레이어 불필요 (ADR-006 D3)
- 바닐라 알고리즘 참조는 디컴파일된 소스가 아니라 공개 문서(minecraft.wiki) + cubiomes 구현 + 실측 대조로 확보한다
- Task 1~4는 데이터팩 스키마를 다루지 않는다. 하드코딩된 오버월드 기본 설정만 사용한다 (스키마 파서는 Task 12)

---

## Proposed approach

**핵심 전략: 가장 불확실한 것을 가장 먼저 죽인다.**

패리티가 이 프로젝트의 유일한 사망 원인이다. 속도는 못 내도 창피할 뿐이지만, 패리티가 안 맞으면 프로젝트 전체가 무의미해진다. 반면 JNI 경계 비용은 이미 계산으로 "리전 단위에서 0%"가 증명됐으므로 리스크가 아니다.

따라서 Phase 1은 **느려도 되는 스칼라 구현으로 5스테이지를 관통**하고 `sha256` 일치를 확보한다. 그 순간이 프로젝트 첫 진짜 마일스톤이며, 그 이후 모든 최적화가 "눈에 보이는 진전"이 된다.

**하위 전략: 스테이지별 golden 대조 (한 번에 관통하지 않는다).**

5스테이지를 다 만들고 마지막에 `sha256`을 비교하면 어느 스테이지가 틀렸는지 알 수 없다. 대신 Java golden 생성기가 **스테이지별 중간 산출물**을 덤프하게 만들고, C 구현을 스테이지 단위로 대조한다. 이것이 ADR-002 Pitfall 1(FMA contraction)과 Pitfall 2(RNG 순서)를 조기에 잡는 유일한 방법이다.

대조 순서: `RNG → noise → surface → carvers → features → lighting → region`

---

## Step-by-step plan

### Task 0: 개발 환경 준비 및 대상 버전 고정

**Objective:** JDK 25 설치, TARGET_VERSION=26.2 고정, 프로젝트 스캐폴드 생성

**Files:**
- Create: `/home/ubuntu/projects/hyperchunk/.gitignore`
- Create: `/home/ubuntu/projects/hyperchunk/README.md`
- Create: `/home/ubuntu/projects/hyperchunk/TARGET_VERSION`

**Step 1: Java 21 설치**

```bash
sudo apt-get update && sudo apt-get install -y openjdk-25-jdk  # 없으면 Temurin 25
java -version 2>&1 | head -1
```
Expected: `openjdk version "25.x.x"`

**Step 2: 대상 버전 확정**

TARGET_VERSION은 26.2로 확정이다 (ADR-006). 매니페스트에서 26.2 존재만 확인한다.

```bash
curl -s https://launchermeta.mojang.com/mc/game/version_manifest_v2.json \
  | python3 -c "import json,sys; d=json.load(sys.stdin); print([v['id'] for v in d['versions'] if v['id']=='26.2'])"
```

`TARGET_VERSION`에 `26.2`를 기록한다. 이 값은 Phase 1 전체에서 고정된다 (ADR-002 D4 + ADR-006 D1).

**Step 3: git init + 스캐폴드**

```bash
cd /home/ubuntu/projects/hyperchunk
git init
mkdir -p core/{include,src} cli tools/golden tests/{unit,parity} scripts
```

⚠️ **git identity는 설정하지 말 것.** 사용자에게 name/email 값을 확인받은 후 설정한다.

**Step 4: `.gitignore` 작성**

```gitignore
build/
*.o
*.so
*.a
golden/
tools/golden/libs/
tools/golden/*.class
__pycache__/
```

**Step 5: 커밋**

```bash
git add -A && git commit -m "chore: scaffold hyperchunk"
```

---

### Task 1: CMake 빌드 시스템 + FMA 금지 강제

**Objective:** 코어 라이브러리와 CLI가 빌드되고, FMA 명령이 산출물에 나타나지 않도록 컴파일 플래그를 강제한다

**Files:**
- Create: `core/CMakeLists.txt`
- Create: `CMakeLists.txt`
- Create: `core/include/hyperchunk.h`
- Create: `core/src/version.c`
- Create: `scripts/check_no_fma.sh`

**Step 1: 루트 `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.28)
project(hyperchunk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ADR-004 D3: FMA 전면 금지. 패리티 불변식.
# -ffp-contract=off 가 핵심. mul+add 를 FMA 로 접는 것을 막는다.
add_compile_options(
  -O2
  -ffp-contract=off
  -fno-fast-math
  -Wall -Wextra -Werror
)

add_subdirectory(core)
add_subdirectory(cli)
enable_testing()
add_subdirectory(tests)
```

**Step 2: `core/CMakeLists.txt`**

```cmake
add_library(hyperchunk STATIC
  src/version.c
)
target_include_directories(hyperchunk PUBLIC include)
# ADR-003 D1: libc 외 의존성 없음
```

**Step 3: `core/include/hyperchunk.h` — 공개 헤더 (경계 불변식)**

```c
#ifndef HC_H
#define HC_H

#include <stdint.h>
#include <stddef.h>

/* ADR-003 D2: 경계는 리전 단위로만 노출한다.
 * density function 노드 단위 함수는 이 헤더에 절대 선언하지 않는다.
 * 노드 단위 FFI 는 청크당 84,000 회 호출 = 18.5% 손실 경로다. */

#define HC_ABI_VERSION 1

const char *hc_version(void);
int         hc_abi_version(void);

#endif /* HC_H */
```

**Step 4: `core/src/version.c`**

```c
#include "hyperchunk.h"

const char *hc_version(void) { return "0.1.0-phase1"; }
int hc_abi_version(void) { return HC_ABI_VERSION; }
```

**Step 5: `scripts/check_no_fma.sh` — FMA 검증 게이트**

```bash
#!/usr/bin/env bash
# ADR-004 Verification: 산출물에 FMA 명령이 없어야 한다.
set -euo pipefail
LIB="${1:-build/core/libhyperchunk.a}"
if objdump -d "$LIB" | grep -qE '\bvfmadd|\bvfmsub|\bvfnmadd|\bvfnmsub'; then
  echo "FAIL: FMA instruction found in $LIB"
  objdump -d "$LIB" | grep -E '\bvfmadd|\bvfmsub|\bvfnmadd|\bvfnmsub' | head -5
  exit 1
fi
echo "PASS: no FMA instructions in $LIB"
```

**Step 6: 빌드 및 게이트 실행**

```bash
cd /home/ubuntu/projects/hyperchunk
cmake -S . -B build && cmake --build build -j
chmod +x scripts/check_no_fma.sh && ./scripts/check_no_fma.sh
ldd build/core/libhyperchunk.a 2>&1 | head -2 || true
```
Expected: 빌드 성공, `PASS: no FMA instructions`

**Step 7: 커밋**

```bash
git add -A && git commit -m "build: cmake skeleton with FMA prohibition gate"
```

---

### Task 2: Java golden 생성기 — 스테이지별 덤프

**Objective:** 바닐라 MC를 실행해 단일 청크의 스테이지별 중간 산출물과 최종 region을 덤프한다. 이것이 모든 패리티 대조의 기준점이 된다.

**Files:**
- Create: `tools/golden/GoldenDump.java`
- Create: `tools/golden/fetch_server.sh`
- Create: `tools/golden/run.sh`

**Step 1: `tools/golden/fetch_server.sh` — 서버 jar 확보**

```bash
#!/usr/bin/env bash
set -euo pipefail
VER="$(cat "$(dirname "$0")/../../TARGET_VERSION")"
OUT="$(dirname "$0")/libs"
mkdir -p "$OUT"
URL="$(curl -s https://launchermeta.mojang.com/mc/game/version_manifest_v2.json \
  | python3 -c "
import json,sys
d=json.load(sys.stdin)
u=[v['url'] for v in d['versions'] if v['id']=='$VER'][0]
print(u)")"
curl -s "$URL" | python3 -c "
import json,sys
print(json.load(sys.stdin)['downloads']['server']['url'])" \
  | xargs curl -o "$OUT/server-$VER.jar"
echo "fetched: $OUT/server-$VER.jar"
```

**Step 2: golden 덤프 전략 결정 (구현 전 확인 필요)**

⚠️ **여기가 Phase 1 최대 불확실성이다.** 바닐라 내부 스테이지 산출물에 접근하는 경로가 셋이고, 실제 가능성을 확인해야 한다:

| 경로 | 방법 | 리스크 |
|---|---|---|
| A. Fabric 모드 + mixin | `ChunkGenerator` 각 단계 후 훅 (26.1+ 비난독화라 Mojang 실명 직접 사용, 매핑 불필요) | Fabric 설치 필요, 가장 확실 |
| B. 리플렉션 하네스 | 서버 jar를 클래스패스로 두고 직접 호출 | 비난독화라 실명 호출 가능, 부트스트랩 순서가 관건 |
| C. region 파일만 대조 | 최종 결과만 비교 | 스테이지 격리 불가 → 디버깅 지옥 |

**추천: A.** Fabric mixin으로 각 스테이지 직후 `ProtoChunk` 상태를 덤프한다. 26.1+는 완전 비난독화이므로 Mojang 실명으로 바로 작성한다 (Yarn은 26.x에서 지원 중단됨).

C는 폴백으로만 쓴다. 스테이지 격리 없이 5스테이지 패리티를 디버깅하는 것은 실질적으로 불가능하다.

**Step 3: 최소 검증 — region만이라도 뽑히는지 확인**

전략 A 구현 전에 C 경로로 최소 golden을 확보해 파이프라인을 뚫는다.

```bash
cd tools/golden && chmod +x fetch_server.sh && ./fetch_server.sh
mkdir -p /tmp/golden-world && cd /tmp/golden-world
echo "eula=true" > eula.txt
cat > server.properties <<'EOF'
level-seed=1234567890
level-type=minecraft:normal
online-mode=false
max-players=1
view-distance=2
EOF
java -Xmx2G -jar /home/ubuntu/projects/hyperchunk/tools/golden/libs/server-*.jar nogui &
sleep 90
kill %1 || true
ls -la world/region/
sha256sum world/region/r.0.0.mca
```
Expected: `r.0.0.mca` 생성, sha256 출력

**Step 4: golden 고정**

```bash
mkdir -p /home/ubuntu/projects/hyperchunk/golden
cp /tmp/golden-world/world/region/r.0.0.mca \
   /home/ubuntu/projects/hyperchunk/golden/seed1234567890_r.0.0.mca
sha256sum /home/ubuntu/projects/hyperchunk/golden/*.mca \
   > /home/ubuntu/projects/hyperchunk/golden/SHA256SUMS
```

**Step 5: 커밋 (golden 바이너리는 gitignore, 해시만 커밋)**

```bash
cd /home/ubuntu/projects/hyperchunk
git add -f golden/SHA256SUMS tools/golden/
git commit -m "test: golden region baseline for seed 1234567890"
```

---

### Task 3: RNG 재구현 — Xoroshiro128++ 와 LCG

**Objective:** 바닐라의 두 RNG를 비트단위 재현한다. 이것이 틀리면 이후 모든 스테이지가 틀린다.

**Files:**
- Create: `core/include/hc_rng.h`
- Create: `core/src/rng.c`
- Create: `tests/unit/test_rng.c`
- Modify: `core/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Step 1: 실패하는 테스트 작성**

`tests/unit/test_rng.c`:

```c
#include "hc_rng.h"
#include <assert.h>
#include <stdio.h>

/* golden 값은 Task 3 Step 4 에서 Java 로 생성해 채운다.
 * 지금은 의도적으로 틀린 값을 넣어 RED 를 확인한다. */
int main(void) {
    hc_xoro_t r;
    hc_xoro_init(&r, 1234567890LL);
    uint64_t v0 = hc_xoro_next(&r);
    printf("xoro[0] = %llu\n", (unsigned long long)v0);
    assert(v0 == 0 /* PLACEHOLDER — Step 4 에서 교체 */);
    return 0;
}
```

**Step 2: 테스트 실행해 실패 확인**

```bash
cmake --build build -j && ./build/tests/test_rng
```
Expected: FAIL — 컴파일 에러 (`hc_rng.h` 없음)

**Step 3: 최소 구현**

`core/include/hc_rng.h`:

```c
#ifndef HC_RNG_H
#define HC_RNG_H
#include <stdint.h>

typedef struct { uint64_t lo, hi; } hc_xoro_t;

void     hc_xoro_init(hc_xoro_t *r, int64_t seed);
uint64_t hc_xoro_next(hc_xoro_t *r);
int32_t  hc_xoro_next_int(hc_xoro_t *r, int32_t bound);
double   hc_xoro_next_double(hc_xoro_t *r);

typedef struct { uint64_t s; } hc_lcg_t;

void    hc_lcg_init(hc_lcg_t *r, int64_t seed);
int32_t hc_lcg_next(hc_lcg_t *r, int bits);

#endif
```

`core/src/rng.c`:

```c
#include "hc_rng.h"

/* --- Xoroshiro128++ (1.18+ worldgen) --- */

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

/* Java: RandomSupport.mixStafford13 */
static uint64_t mix_stafford13(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

#define GOLDEN_RATIO_64 0x9E3779B97F4A7C15ULL
#define SILVER_RATIO_64 0x6A09E667F3BCC909ULL

void hc_xoro_init(hc_xoro_t *r, int64_t seed) {
    uint64_t l = (uint64_t)seed ^ SILVER_RATIO_64;
    uint64_t h = l + GOLDEN_RATIO_64;
    r->lo = mix_stafford13(l);
    r->hi = mix_stafford13(h);
    /* 둘 다 0 이면 축퇴하므로 바닐라도 회피한다 */
    if (r->lo == 0 && r->hi == 0) { r->lo = GOLDEN_RATIO_64; r->hi = SILVER_RATIO_64; }
}

uint64_t hc_xoro_next(hc_xoro_t *r) {
    uint64_t s0 = r->lo, s1 = r->hi;
    uint64_t res = rotl(s0 + s1, 17) + s0;   /* ++ variant */
    s1 ^= s0;
    r->lo = rotl(s0, 49) ^ s1 ^ (s1 << 21);
    r->hi = rotl(s1, 28);
    return res;
}

/* Java nextInt(bound): 상위 비트 사용 + rejection */
int32_t hc_xoro_next_int(hc_xoro_t *r, int32_t bound) {
    uint32_t b = (uint32_t)bound;
    uint32_t v = (uint32_t)(hc_xoro_next(r) >> 32);
    uint64_t m = (uint64_t)v * (uint64_t)b;
    uint32_t lo = (uint32_t)m;
    if (lo < b) {
        uint32_t t = (uint32_t)(-(int32_t)b) % b;
        while (lo < t) {
            v = (uint32_t)(hc_xoro_next(r) >> 32);
            m = (uint64_t)v * (uint64_t)b;
            lo = (uint32_t)m;
        }
    }
    return (int32_t)(m >> 32);
}

double hc_xoro_next_double(hc_xoro_t *r) {
    return (double)(hc_xoro_next(r) >> 11) * 0x1.0p-53;
}

/* --- Legacy LCG (java.util.Random 호환) --- */

#define LCG_MUL  0x5DEECE66DULL
#define LCG_ADD  0xBULL
#define LCG_MASK ((1ULL << 48) - 1)

void hc_lcg_init(hc_lcg_t *r, int64_t seed) {
    r->s = ((uint64_t)seed ^ LCG_MUL) & LCG_MASK;
}

int32_t hc_lcg_next(hc_lcg_t *r, int bits) {
    r->s = (r->s * LCG_MUL + LCG_ADD) & LCG_MASK;
    return (int32_t)(r->s >> (48 - bits));
}
```

`core/CMakeLists.txt`에 `src/rng.c` 추가.

**Step 4: Java 로 golden 값 생성**

```bash
cd /tmp && cat > RngGolden.java <<'EOF'
import java.util.Random;
public class RngGolden {
  public static void main(String[] a) {
    // 1) legacy LCG 대조
    Random r = new Random(1234567890L);
    System.out.println("lcg_int_0=" + r.nextInt());
    System.out.println("lcg_int_1=" + r.nextInt());
    System.out.println("lcg_dbl_2=" + r.nextDouble());
    // 2) Xoroshiro 는 MC 내부 클래스이므로 Task 2 전략 A 확보 후 덤프
  }
}
EOF
javac RngGolden.java && java RngGolden
```

출력값을 `tests/unit/test_rng.c`의 PLACEHOLDER에 채운다.

⚠️ Xoroshiro golden은 MC 내부 Xoroshiro 클래스(26.x 비난독화 실명 기준)라 Task 2 전략 A(Fabric mixin)가 확보된 후에 덤프해야 한다. 그때까지 LCG만 검증한다.

**Step 5: 테스트 통과 확인**

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: PASS

**Step 6: 커밋**

```bash
git add -A && git commit -m "feat: Xoroshiro128++ and legacy LCG with golden-verified LCG"
```

---

### Task 4: arena 할당자 + SoA 블록 스토리지

**Objective:** 청크 생성 중 힙 할당을 0으로 만드는 arena와, 블록 데이터를 SoA로 담는 스토리지를 구현한다 (ADR-003 D3)

**Files:**
- Create: `core/include/hc_arena.h`
- Create: `core/src/arena.c`
- Create: `core/include/hc_chunk.h`
- Create: `core/src/chunk.c`
- Create: `tests/unit/test_arena.c`

**Step 1: 실패하는 테스트**

`tests/unit/test_arena.c`:

```c
#include "hc_arena.h"
#include <assert.h>
#include <string.h>

int main(void) {
    unsigned char backing[4096];
    hc_arena_t a;
    hc_arena_init(&a, backing, sizeof backing);

    void *p = hc_arena_alloc(&a, 100, 16);
    assert(p != NULL);
    assert(((uintptr_t)p % 16) == 0);

    size_t used_before = hc_arena_used(&a);
    assert(used_before >= 100);

    hc_arena_reset(&a);
    assert(hc_arena_used(&a) == 0);

    /* 용량 초과는 NULL, abort 아님 */
    assert(hc_arena_alloc(&a, 1u << 20, 16) == NULL);
    return 0;
}
```

**Step 2: 실패 확인**

```bash
cmake --build build -j 2>&1 | tail -3
```
Expected: FAIL — `hc_arena.h` 없음

**Step 3: 구현**

`core/include/hc_arena.h`:

```c
#ifndef HC_ARENA_H
#define HC_ARENA_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned char *base;
    size_t cap;
    size_t off;
} hc_arena_t;

void   hc_arena_init(hc_arena_t *a, void *backing, size_t cap);
void  *hc_arena_alloc(hc_arena_t *a, size_t n, size_t align);
void   hc_arena_reset(hc_arena_t *a);
size_t hc_arena_used(const hc_arena_t *a);

#endif
```

`core/src/arena.c`:

```c
#include "hc_arena.h"

void hc_arena_init(hc_arena_t *a, void *backing, size_t cap) {
    a->base = (unsigned char *)backing;
    a->cap = cap;
    a->off = 0;
}

void *hc_arena_alloc(hc_arena_t *a, size_t n, size_t align) {
    size_t p = (a->off + (align - 1)) & ~(align - 1);
    if (p > a->cap || n > a->cap - p) return NULL;   /* 오버플로 안전 */
    a->off = p + n;
    return a->base + p;
}

void hc_arena_reset(hc_arena_t *a) { a->off = 0; }

size_t hc_arena_used(const hc_arena_t *a) { return a->off; }
```

`core/include/hc_chunk.h`:

```c
#ifndef HC_CHUNK_H
#define HC_CHUNK_H
#include <stdint.h>
#include "hc_arena.h"

/* 오버월드 y 범위: 1.21 기준 -64..319 (384). 26.2에서 유지 여부는 Task 2 golden에서 실측 확정 (ADR-006 Pitfall 4) */
#define HC_MIN_Y      (-64)
#define HC_HEIGHT     384
#define HC_CHUNK_XZ   16
#define HC_BLOCKS     (HC_CHUNK_XZ * HC_CHUNK_XZ * HC_HEIGHT)

/* SoA: 블록당 객체를 만들지 않는다. 팔레트 인덱스 평면 배열만 둔다.
 * ADR-003 D3 — 자바는 청크당 ~40,808 객체를 만든다. 여기서는 0 이다. */
typedef struct {
    int32_t   cx, cz;
    uint16_t *states;                       /* HC_BLOCKS, 팔레트 인덱스 */
    int32_t   heightmap_ws[256];            /* WORLD_SURFACE */
    int32_t   heightmap_ocean_floor[256];
} hc_chunk_t;

int hc_chunk_init(hc_chunk_t *c, hc_arena_t *a, int32_t cx, int32_t cz);

static inline size_t hc_idx(int x, int y, int z) {
    return (size_t)((y - HC_MIN_Y) * 256 + z * 16 + x);
}

#endif
```

`core/src/chunk.c`:

```c
#include "hc_chunk.h"
#include <string.h>

int hc_chunk_init(hc_chunk_t *c, hc_arena_t *a, int32_t cx, int32_t cz) {
    c->cx = cx; c->cz = cz;
    c->states = (uint16_t *)hc_arena_alloc(a, sizeof(uint16_t) * HC_BLOCKS, 64);
    if (!c->states) return -1;
    memset(c->states, 0, sizeof(uint16_t) * HC_BLOCKS);
    memset(c->heightmap_ws, 0, sizeof c->heightmap_ws);
    memset(c->heightmap_ocean_floor, 0, sizeof c->heightmap_ocean_floor);
    return 0;
}
```

⚠️ **ADR-003 Pitfall 3:** arena를 배치 간 재사용할 때 `memset` 누락이 패리티 버그로 나타난다. `hc_chunk_init`이 항상 zero-fill 하도록 유지한다.

**Step 4: 통과 확인**

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: 2 tests passed

**Step 5: 커밋**

```bash
git add -A && git commit -m "feat: arena allocator and SoA chunk storage"
```

---

### Task 5: density function 그래프 IR + 스칼라 평가기

**Objective:** 인터프리터 트리 대신 평탄화된 IR을 정의하고 스칼라 평가기를 구현한다. Phase 2 SIMD 커널이 같은 IR을 소비한다.

**Files:**
- Create: `core/include/hc_df.h`
- Create: `core/src/df_eval.c`
- Create: `core/src/noise_perlin.c`
- Create: `tests/unit/test_perlin.c`

**Step 1: 실패하는 테스트 — Perlin 단일 샘플**

`tests/unit/test_perlin.c`:

```c
#include "hc_df.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    hc_perlin_t p;
    hc_perlin_init(&p, 1234567890LL);
    double v = hc_perlin_sample(&p, 0.5, 0.25, 0.125);
    printf("perlin=%.17g\n", v);
    /* Task 5 Step 5 에서 golden 으로 교체 */
    assert(fabs(v - 0.0) < 1e-300);
    return 0;
}
```

**Step 2: 실패 확인**

```bash
cmake --build build -j 2>&1 | tail -3
```
Expected: FAIL

**Step 3: IR 정의**

`core/include/hc_df.h`:

```c
#ifndef HC_DF_H
#define HC_DF_H
#include <stdint.h>
#include <stddef.h>
#include "hc_arena.h"

/* --- Perlin --- */
typedef struct {
    uint8_t perm[512];
    double  xo, yo, zo;
} hc_perlin_t;

void   hc_perlin_init(hc_perlin_t *p, int64_t seed);
double hc_perlin_sample(const hc_perlin_t *p, double x, double y, double z);

/* --- density function IR ---
 * 트리 순회 대신 평탄 배열. 노드는 위상 정렬되어 있고
 * 각 노드는 자기보다 낮은 인덱스만 참조한다.
 * Phase 2 SIMD 커널이 동일 IR 을 소비한다. */
typedef enum {
    HC_DF_CONST = 0,
    HC_DF_X, HC_DF_Y, HC_DF_Z,
    HC_DF_NOISE,
    HC_DF_ADD, HC_DF_MUL, HC_DF_MIN, HC_DF_MAX,
    HC_DF_CLAMP,
    HC_DF_Y_CLAMPED_GRADIENT,
    HC_DF_SPLINE,
    HC_DF_OP_COUNT
} hc_df_op_t;

typedef struct {
    uint8_t op;
    int32_t a, b;          /* 피연산자 노드 인덱스, 미사용은 -1 */
    double  k0, k1, k2;    /* 상수 파라미터 */
    int32_t noise_id;      /* HC_DF_NOISE 일 때 */
} hc_df_node_t;

typedef struct {
    hc_df_node_t *nodes;
    int32_t         n;
    hc_perlin_t  *noises;
    int32_t         n_noises;
    int32_t         root;
} hc_df_graph_t;

/* 스칼라 참조 평가기. Phase 1 은 이것만 쓴다. */
double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                    double *scratch);

#endif
```

**Step 4: 구현**

`core/src/noise_perlin.c`:

```c
#include "hc_df.h"
#include "hc_rng.h"

/* 바닐라 ImprovedNoise 의 16 방향 gradient.
 * ADR-004: 16 entries = zmm 2 개. Phase 2 에서 vpermt2pd 대상. */
static const int8_t GRAD[16][3] = {
    { 1, 1, 0},{-1, 1, 0},{ 1,-1, 0},{-1,-1, 0},
    { 1, 0, 1},{-1, 0, 1},{ 1, 0,-1},{-1, 0,-1},
    { 0, 1, 1},{ 0,-1, 1},{ 0, 1,-1},{ 0,-1,-1},
    { 1, 1, 0},{ 0,-1, 1},{-1, 1, 0},{ 0,-1,-1},
};

static double smoothstep(double t) {
    /* 바닐라: t*t*t*(t*(t*6-15)+10)
     * ADR-004 D3 — 이 표현을 FMA 로 접으면 결과 비트가 달라진다.
     * -ffp-contract=off 가 컴파일러 차원에서 막는다. */
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

static double lerp(double t, double a, double b) { return a + t * (b - a); }

static double grad_dot(int hash, double x, double y, double z) {
    const int8_t *g = GRAD[hash & 15];
    return (double)g[0] * x + (double)g[1] * y + (double)g[2] * z;
}

void hc_perlin_init(hc_perlin_t *p, int64_t seed) {
    hc_xoro_t r;
    hc_xoro_init(&r, seed);
    p->xo = hc_xoro_next_double(&r) * 256.0;
    p->yo = hc_xoro_next_double(&r) * 256.0;
    p->zo = hc_xoro_next_double(&r) * 256.0;
    for (int i = 0; i < 256; i++) p->perm[i] = (uint8_t)i;
    /* 바닐라 셔플: i 오름차순, nextInt(256 - i) 소비 순서까지 동일해야 한다.
     * ADR-002 Pitfall 2 */
    for (int i = 0; i < 256; i++) {
        int j = i + hc_xoro_next_int(&r, 256 - i);
        uint8_t t = p->perm[i]; p->perm[i] = p->perm[j]; p->perm[j] = t;
    }
    for (int i = 0; i < 256; i++) p->perm[256 + i] = p->perm[i];
}

double hc_perlin_sample(const hc_perlin_t *p, double x, double y, double z) {
    double dx = x + p->xo, dy = y + p->yo, dz = z + p->zo;
    double fx = __builtin_floor(dx), fy = __builtin_floor(dy), fz = __builtin_floor(dz);
    int X = (int)((int64_t)fx & 255), Y = (int)((int64_t)fy & 255), Z = (int)((int64_t)fz & 255);
    double rx = dx - fx, ry = dy - fy, rz = dz - fz;
    double u = smoothstep(rx), v = smoothstep(ry), w = smoothstep(rz);

    const uint8_t *P = p->perm;
    int A = P[X] + Y, AA = P[A] + Z, AB = P[A + 1] + Z;
    int B = P[X + 1] + Y, BA = P[B] + Z, BB = P[B + 1] + Z;

    double x1 = lerp(u, grad_dot(P[AA],   rx,       ry,       rz),
                        grad_dot(P[BA],   rx - 1.0, ry,       rz));
    double x2 = lerp(u, grad_dot(P[AB],   rx,       ry - 1.0, rz),
                        grad_dot(P[BB],   rx - 1.0, ry - 1.0, rz));
    double y1 = lerp(v, x1, x2);
    double x3 = lerp(u, grad_dot(P[AA+1], rx,       ry,       rz - 1.0),
                        grad_dot(P[BA+1], rx - 1.0, ry,       rz - 1.0));
    double x4 = lerp(u, grad_dot(P[AB+1], rx,       ry - 1.0, rz - 1.0),
                        grad_dot(P[BB+1], rx - 1.0, ry - 1.0, rz - 1.0));
    double y2 = lerp(v, x3, x4);
    return lerp(w, y1, y2);
}
```

`core/src/df_eval.c`:

```c
#include "hc_df.h"

double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                    double *scratch) {
    for (int32_t i = 0; i < g->n; i++) {
        const hc_df_node_t *nd = &g->nodes[i];
        double r;
        switch (nd->op) {
        case HC_DF_CONST: r = nd->k0; break;
        case HC_DF_X:     r = x; break;
        case HC_DF_Y:     r = y; break;
        case HC_DF_Z:     r = z; break;
        case HC_DF_NOISE:
            r = hc_perlin_sample(&g->noises[nd->noise_id],
                                   x * nd->k0, y * nd->k1, z * nd->k2);
            break;
        case HC_DF_ADD: r = scratch[nd->a] + scratch[nd->b]; break;
        case HC_DF_MUL: r = scratch[nd->a] * scratch[nd->b]; break;
        case HC_DF_MIN: r = scratch[nd->a] < scratch[nd->b] ? scratch[nd->a] : scratch[nd->b]; break;
        case HC_DF_MAX: r = scratch[nd->a] > scratch[nd->b] ? scratch[nd->a] : scratch[nd->b]; break;
        case HC_DF_CLAMP: {
            double t = scratch[nd->a];
            r = t < nd->k0 ? nd->k0 : (t > nd->k1 ? nd->k1 : t);
            break;
        }
        case HC_DF_Y_CLAMPED_GRADIENT: {
            double t = (y - nd->k0) / (nd->k1 - nd->k0);
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
            r = nd->k2 * t;
            break;
        }
        default: r = 0.0; break;
        }
        scratch[i] = r;
    }
    return scratch[g->root];
}
```

⚠️ `HC_DF_SPLINE`은 Task 6에서 채운다. 지금은 `default`로 0을 반환한다.

**Step 5: golden 대조 후 테스트 갱신**

Task 2 전략 A가 확보되면 mixin으로 동일 좌표의 `ImprovedNoise` 샘플을 덤프해 `test_perlin.c`의 assert를 교체한다.

**Step 6: 커밋**

```bash
git add -A && git commit -m "feat: flat density function IR and scalar Perlin"
```

---

### Task 6: 스플라인 + 오버월드 노이즈 라우터 하드코딩

**Objective:** 26.2 오버월드의 density function 그래프를 하드코딩으로 구성해 `final_density`를 평가할 수 있게 한다

**Files:**
- Create: `core/src/df_spline.c`
- Create: `core/src/overworld_router.c`
- Create: `core/include/hc_overworld.h`
- Create: `tests/parity/test_noise_column.c`

⚠️ **이 태스크가 Phase 1 최대 작업량이다.** 26.2 오버월드 노이즈 라우터는 `continents`, `erosion`, `depth`, `ridges`, `jaggedness`, `offset`, `factor` 등을 조합하며 스플라인이 중첩된다. 실제 파라미터는 `data/minecraft/worldgen/noise_settings/overworld.json`에서 추출한다.

**Step 1: 바닐라 JSON 추출**

```bash
cd /tmp && VER="$(cat /home/ubuntu/projects/hyperchunk/TARGET_VERSION)"
unzip -o -q /home/ubuntu/projects/hyperchunk/tools/golden/libs/server-$VER.jar -d server-extract
find server-extract -name "overworld.json" -path "*noise_settings*"
```

추출한 JSON을 `reference/overworld.json`으로 보관하고, 이를 그대로 IR로 옮긴다.

**Step 2~6:** JSON 구조에 맞춰 IR 노드를 생성하는 하드코딩 빌더 작성 → 컬럼 단위 golden 대조 → 커밋

⚠️ 이 태스크는 실제 JSON을 본 후 세분화해야 한다. 지금 단계에서 하위 태스크를 미리 나누면 추측이 된다. **Task 6 착수 시점에 JSON을 읽고 이 플랜을 갱신할 것.**

---

### Task 7~11: 나머지 스테이지 (착수 시 세분화)

| Task | 스테이지 | 핵심 리스크 |
|---|---|---|
| 7 | surface rule | 바이옴별 규칙 트리, `SurfaceRules` 조건 순서 |
| 8 | carvers | 랜덤 워크 + scatter write, RNG 소비 순서 |
| 9 | features (배치까지) | **최대 난이도.** 청크 경계 write, 체스판 스케줄링 (ADR-002 Pitfall 4) |
| 10 | lighting | flood fill, 경계 청크 의존 |
| 11 | region 직렬화 | NBT + zlib, libdeflate 조립 (ADR-005 D2) |

각 태스크는 착수 직전에 golden 덤프를 확보한 뒤 세분화한다. 스테이지 산출물을 대조할 수 없으면 그 태스크를 시작하지 않는다.

---

### Task 12: 데이터팩 스키마 파서

**Objective:** `noise_settings` / `density_function` JSON을 IR로 컴파일해 Terralith급 데이터팩을 자동 호환한다 (ADR-003 D4)

Task 6에서 하드코딩한 그래프를 JSON 파서로 대체한다. 하드코딩 결과와 파서 결과의 IR이 동일함을 테스트로 고정한 뒤 전환한다.

---

### Task 13: 패리티 게이트 CLI

**Objective:** `hyperchunk-verify`가 golden region과 `sha256` 일치를 판정한다

**Files:**
- Create: `cli/hyperchunk_verify.c`
- Create: `scripts/parity_gate.sh`

```bash
#!/usr/bin/env bash
# ADR-002 D3 수용 기준
set -euo pipefail
SEED=1234567890
./build/cli/hyperchunk-verify --seed $SEED --region 0 0 --out /tmp/ours_r.0.0.mca
A="$(sha256sum /tmp/ours_r.0.0.mca | cut -d' ' -f1)"
B="$(cut -d' ' -f1 golden/SHA256SUMS | head -1)"
[ "$A" = "$B" ] && echo "PASS: bit-exact parity" || { echo "FAIL: $A != $B"; exit 1; }
```

---

## Files likely to change

```
hyperchunk/
├── DECISIONS.md                       (완료)
├── TARGET_VERSION                     Task 0
├── CMakeLists.txt                     Task 1
├── core/
│   ├── include/
│   │   ├── hyperchunk.h                     Task 1  ← 리전 단위 경계만
│   │   ├── hc_rng.h                 Task 3
│   │   ├── hc_arena.h               Task 4
│   │   ├── hc_chunk.h               Task 4
│   │   ├── hc_df.h                  Task 5
│   │   └── hc_overworld.h           Task 6
│   └── src/
│       ├── version.c                  Task 1
│       ├── rng.c                       Task 3
│       ├── arena.c                     Task 4
│       ├── chunk.c                     Task 4
│       ├── noise_perlin.c              Task 5
│       ├── df_eval.c                   Task 5
│       ├── df_spline.c                 Task 6
│       ├── overworld_router.c          Task 6
│       ├── surface.c                   Task 7
│       ├── carvers.c                   Task 8
│       ├── features.c                  Task 9
│       ├── lighting.c                  Task 10
│       └── region_nbt.c                Task 11
├── cli/hyperchunk_verify.c                   Task 13
├── tools/golden/                       Task 2
├── tests/{unit,parity}/                Task 3~11
└── scripts/{check_no_fma,parity_gate}.sh
```

---

## Tests / validation

**태스크별 게이트:**

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
./scripts/check_no_fma.sh          # ADR-004 Verification
```

**Phase 1 완료 조건 (전부 충족해야 함):**

1. `./scripts/parity_gate.sh` → `PASS: bit-exact parity`
2. `./scripts/check_no_fma.sh` → `PASS: no FMA instructions`
3. `ldd`로 코어가 `libc` 외 의존성 없음 확인 (ADR-003 D1)
4. `core/include/`의 모든 헤더에 노드 단위 진입점이 없음 (ADR-003 D2, 코드 리뷰로 확인)
5. 최소 3개 시드에서 패리티 통과 (단일 시드 통과는 우연일 수 있음)

---

## Risks, tradeoffs, and open questions

### 🔴 R1: golden 스테이지 덤프 확보 (Phase 1 최대 리스크)

Task 2 전략 A(Fabric mixin)가 실패하면 최종 region만 대조하게 되고, 5스테이지 패리티 디버깅이 실질적으로 불가능해진다.

**완화:** Task 2를 Task 3보다 먼저 완료하고, 전략 A가 막히면 즉시 보고한다. Phase 1 착수 전 이 리스크를 먼저 해소하는 것이 옳다.

### 🔴 R2: features 병렬 패리티 (Task 9)

청크 경계를 넘는 write + RNG 소비 순서 재현이 C2ME가 수년간 씨름한 문제다 (ADR-002 Pitfall 4).

**완화:** Phase 1은 단일 스레드다. 병렬화는 Phase 2로 미루고, Phase 1에서는 순차 처리로 패리티만 확보한다.

### 🟡 R3: 벤치 호스트 신뢰성

현재 박스는 토폴로지 오보고 VM이다 (위 환경 섹션). Phase 1은 패리티만 다뤄 무해하나, **Phase 2 착수 전 베어메탈 확보가 필수 선행조건**이다.

### 🟡 R4: 바닐라 알고리즘 참조 방법

디컴파일 소스를 배포물에 포함하면 법적 문제가 된다 (ADR-001 D5). 공개 문서 + cubiomes + 실측 대조로만 진행한다.

**완화:** 리버스 엔지니어링 결과를 코드 주석으로 남기되, 디컴파일된 코드를 복사하지 않는다.

### 🟡 R5: Task 6 규모 미확정

오버월드 노이즈 라우터의 실제 복잡도는 JSON을 봐야 안다. 지금 하위 태스크를 나누면 추측이다.

**완화:** Task 6 착수 시 JSON을 읽고 이 플랜을 갱신한다. 플랜 갱신 자체를 Task 6의 첫 스텝으로 둔다.

### Open questions

1. **라이선스?** C2ME 포크가 아니므로 GPL 전파가 없다 (ADR-002). MIT / Apache-2.0 / GPL-3.0 중 선택 필요
2. **프로젝트 이름?** `hyperchunk`는 작업명이다. ADR-001 D5에 따라 Minecraft를 지배적 요소로 쓸 수 없다
3. **git identity?** name/email 값을 사용자에게 확인받아야 한다 (Task 0 Step 3)
4. **공개 저장소 시점?** 패리티 게이트 통과 후가 자연스럽다

---

## Execution handoff

Plan complete. 다음 단계는 `dev-delegation`으로 실행 executor를 확인하고, `subagent-driven-development` 루프(태스크별 spec 준수 리뷰 → 코드 품질 리뷰)로 Task 0부터 진행한다.

**단, Task 1 착수 전에 R1(golden 스테이지 덤프)을 먼저 해소할 것을 권한다.** 그것이 막히면 Task 3 이후 전체가 디버깅 불가 상태로 진행된다.
