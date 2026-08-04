/* Task 14: NBT 리더 왕복 게이트 — 골든 starts 프래그먼트 (ADR-003 D4
 * 재생 입력) 를 파스 → 재직렬화하면 원본 바이트가 나와야 한다.
 *
 * 이것이 증명하는 것: (a) 리더의 태그/길이 해석, (b) 파일-순서 put →
 * HashMap 에뮬레이션 재방출이 항등 (파일 순서는 같은 키 집합의 HashMap
 * 순회 순서이고, 그 순서로 재삽입하면 버킷 체인 상대 순서가 보존된다 —
 * hc_nbt.h §리더), (c) float/int_array 비트 보존.
 *
 * 인자: <fragment.nbt>... (없는 파일은 skip 카운트 — 골든 로컬 아티팩트
 * 부재 시 게이트는 통과하되 0개 검증을 스탬프). */

#undef NDEBUG

#include "../../core/src/hc_nbt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char g_backing[64u << 20];

int main(int argc, char **argv) {
    hc_arena_t a;
    hc_arena_init(&a, g_backing, sizeof g_backing);

    int checked = 0, skipped = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "skip (missing local golden): %s\n", argv[i]);
            skipped++;
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        uint8_t *buf = hc_arena_alloc(&a, (size_t)sz, 1);
        assert(buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz);
        fclose(f);

        hc_nbt_t *root = hc_nbt_parse(&a, buf, (size_t)sz);
        if (!root) {
            fprintf(stderr, "FAIL: parse error: %s\n", argv[i]);
            return 1;
        }
        uint8_t *out = hc_arena_alloc(&a, (size_t)sz + 64, 1);
        assert(out);
        ptrdiff_t n = hc_nbt_write(root, out, (size_t)sz + 64);
        if (n != sz || memcmp(out, buf, (size_t)sz) != 0) {
            size_t off = 0;
            while (off < (size_t)(n < sz ? n : sz) && out[off] == buf[off])
                off++;
            fprintf(stderr,
                    "FAIL: roundtrip mismatch %s: in=%ld out=%td first diff "
                    "@%zu\n",
                    argv[i], sz, n, off);
            return 1;
        }
        printf("roundtrip OK: %s (%ld bytes)\n", argv[i], sz);
        checked++;
    }
    printf("test_nbt_read: PASS (%d roundtripped, %d skipped)\n", checked,
           skipped);
    return 0;
}
