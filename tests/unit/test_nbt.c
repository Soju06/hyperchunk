/* NBT 라이터 단위 테스트 (Task 12).
 *
 * 1) hc_nbt_java_map_order 가 golden r.0.0.mca 실측 키 순서를 재현하는지
 *    — 실측 순서는 .hermes/notes/task12-region/ 인벤토리 (1024 청크 전수
 *    조사, 컴파운드별 유일 순서). 삽입 순서는 바닐라 put 순서와 충돌
 *    제약(같은 버킷 키들의 상대 순서)이 같은 대표를 쓴다.
 * 2) 직렬화 바이트 수작업 벡터 (무명 루트, 빈 리스트 etag=End,
 *    modified-UTF-8 프레이밍, 배열 빅엔디언).
 * 3) 버퍼 초과 -1. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../core/include/hc_arena.h"
#include "../../core/src/hc_nbt.h"

static unsigned char g_backing[1u << 20];
static hc_arena_t    g_arena;

static int g_fail = 0;

static void check_order(const char *label, const char *const *ins, int n,
                        const char *const *want) {
    uint8_t perm[64];
    hc_nbt_java_map_order(ins, n, perm);
    for (int i = 0; i < n; i++) {
        if (strcmp(ins[perm[i]], want[i]) != 0) {
            fprintf(stderr, "FAIL %s: pos %d got %s want %s\n", label, i,
                    ins[perm[i]], want[i]);
            g_fail = 1;
            return;
        }
    }
    printf("ok  order %s\n", label);
}

static void test_orders(void) {
    /* 루트 15키 (충돌: zPos 가 block_entities 보다 먼저 삽입) */
    static const char *const root_ins[] = {
        "xPos",        "zPos",       "yPos",           "LastUpdate",
        "InhabitedTime", "Status",   "sections",       "block_entities",
        "Heightmaps",  "block_ticks", "fluid_ticks",   "PostProcessing",
        "structures",  "isLightOn",  "DataVersion",
    };
    static const char *const root_want[] = {
        "Status",     "zPos",        "block_entities", "yPos",
        "LastUpdate", "structures",  "InhabitedTime",  "xPos",
        "Heightmaps", "sections",    "isLightOn",      "block_ticks",
        "PostProcessing", "DataVersion", "fluid_ticks",
    };
    check_order("root15", root_ins, 15, root_want);

    /* 섹션 4변형 (충돌: block_states<SkyLight, BlockLight<Y) */
    static const char *const sec_ins[] = {"block_states", "biomes", "SkyLight",
                                          "BlockLight", "Y"};
    static const char *const sec_want[] = {"block_states", "SkyLight",
                                           "biomes", "BlockLight", "Y"};
    check_order("section5", sec_ins, 5, sec_want);
    static const char *const sec_s_ins[] = {"block_states", "biomes",
                                            "SkyLight", "Y"};
    static const char *const sec_s_want[] = {"block_states", "SkyLight",
                                             "biomes", "Y"};
    check_order("sectionS", sec_s_ins, 4, sec_s_want);
    static const char *const sec_b_ins[] = {"block_states", "biomes",
                                            "BlockLight", "Y"};
    static const char *const sec_b_want[] = {"block_states", "biomes",
                                             "BlockLight", "Y"};
    check_order("sectionB", sec_b_ins, 4, sec_b_want);
    static const char *const sec_0_ins[] = {"block_states", "biomes", "Y"};
    check_order("section0", sec_0_ins, 3, sec_0_ins);

    /* block_states / 팔레트 엔트리 / 바이옴 */
    static const char *const bs_ins[] = {"palette", "data"};
    static const char *const bs_want[] = {"data", "palette"};
    check_order("block_states", bs_ins, 2, bs_want);
    static const char *const pe_ins[] = {"Name", "Properties"};
    static const char *const pe_want[] = {"Properties", "Name"};
    check_order("palette_entry", pe_ins, 2, pe_want);

    /* Heightmaps 4종 */
    static const char *const hm_ins[] = {"WORLD_SURFACE", "OCEAN_FLOOR",
                                         "MOTION_BLOCKING",
                                         "MOTION_BLOCKING_NO_LEAVES"};
    static const char *const hm_want[] = {"OCEAN_FLOOR",
                                          "MOTION_BLOCKING_NO_LEAVES",
                                          "MOTION_BLOCKING", "WORLD_SURFACE"};
    check_order("heightmaps", hm_ins, 4, hm_want);

    /* structures */
    static const char *const st_ins[] = {"starts", "References"};
    static const char *const st_want[] = {"References", "starts"};
    check_order("structures", st_ins, 2, st_want);

    /* 틱 엔트리 (충돌: i 가 y 보다 먼저 삽입) */
    static const char *const tk_ins[] = {"i", "p", "t", "x", "y", "z"};
    static const char *const tk_want[] = {"p", "t", "x", "i", "y", "z"};
    check_order("tick", tk_ins, 6, tk_want);

    /* Properties — 삽입은 이름 역정렬 (실측 89/89 블록 재현 규칙) */
    static const char *const lv_ins[] = {"waterlogged", "persistent",
                                         "distance"};
    static const char *const lv_want[] = {"waterlogged", "distance",
                                          "persistent"};
    check_order("props_leaves", lv_ins, 3, lv_want);
    static const char *const gl_ins[] = {"west", "waterlogged", "up", "south",
                                         "north", "east", "down"};
    static const char *const gl_want[] = {"east", "waterlogged", "south",
                                          "north", "west", "up", "down"};
    check_order("props_glow_lichen", gl_ins, 7, gl_want);
    static const char *const bb_ins[] = {"stage", "leaves", "age"};
    static const char *const bb_want[] = {"stage", "leaves", "age"};
    check_order("props_bamboo", bb_ins, 3, bb_want);
    static const char *const stair_ins[] = {"waterlogged", "shape", "half",
                                            "facing"};
    static const char *const stair_want[] = {"waterlogged", "half", "shape",
                                             "facing"};
    check_order("props_stairs", stair_ins, 4, stair_want);
    static const char *const door_ins[] = {"powered", "open", "hinge", "half",
                                           "facing"};
    static const char *const door_want[] = {"hinge", "half", "powered",
                                            "facing", "open"};
    check_order("props_door", door_ins, 5, door_want);
    static const char *const tw_ins[] = {"west", "south", "powered", "north",
                                         "east", "disarmed", "attached"};
    static const char *const tw_want[] = {"disarmed", "east", "powered",
                                          "south", "north", "west", "attached"};
    check_order("props_tripwire", tw_ins, 7, tw_want);
}

static void expect_bytes(const char *label, const hc_nbt_t *root,
                         const uint8_t *want, size_t want_n) {
    uint8_t   buf[512];
    ptrdiff_t n = hc_nbt_write(root, buf, sizeof buf);
    if (n < 0 || (size_t)n != want_n || memcmp(buf, want, want_n) != 0) {
        fprintf(stderr, "FAIL %s: got %td bytes want %zu\n", label, n, want_n);
        if (n > 0) {
            for (ptrdiff_t i = 0; i < n; i++)
                fprintf(stderr, "%02x ", buf[i]);
            fprintf(stderr, "\n");
        }
        g_fail = 1;
        return;
    }
    printf("ok  bytes %s (%zu)\n", label, want_n);
}

static void test_bytes(void) {
    hc_arena_t *a = &g_arena;

    /* 빈 루트: 0A 0000 | 00 */
    {
        hc_nbt_t *r = hc_nbt_compound(a);
        static const uint8_t want[] = {0x0A, 0x00, 0x00, 0x00};
        expect_bytes("empty_root", r, want, sizeof want);
    }

    /* 스칼라 + 문자열 + 빈 리스트 + 중첩 리스트.
     * 키 순서: cap16 에서 "b"(98&15=2) < "s"(115&15=3) < "L"(76&15=12)
     * < "e"(101&15=5)... 계산 대신 단일 키 컴파운드로 순서 민감성 제거. */
    {
        hc_nbt_t *r = hc_nbt_compound(a);
        hc_nbt_put(a, r, "b", hc_nbt_byte(a, -2));
        static const uint8_t want[] = {0x0A, 0x00, 0x00,       /* 루트 */
                                       0x01, 0x00, 0x01, 'b',  /* Byte b */
                                       0xFE, 0x00};
        expect_bytes("byte", r, want, sizeof want);
    }
    {
        hc_nbt_t *r = hc_nbt_compound(a);
        hc_nbt_put(a, r, "s", hc_nbt_string(a, "minecraft:full"));
        static const uint8_t want[] = {0x0A, 0x00, 0x00, 0x08, 0x00, 0x01,
                                       's',  0x00, 0x0E, 'm',  'i',  'n',
                                       'e',  'c',  'r',  'a',  'f',  't',
                                       ':',  'f',  'u',  'l',  'l',  0x00};
        expect_bytes("string", r, want, sizeof want);
    }
    {
        /* PostProcessing 형태: 리스트(리스트) — 빈 내부 리스트 etag=End */
        hc_nbt_t *r = hc_nbt_compound(a);
        hc_nbt_t *outer = hc_nbt_list(a);
        hc_nbt_t *in0 = hc_nbt_list(a);
        hc_nbt_t *in1 = hc_nbt_list(a);
        hc_nbt_add(a, outer, in0);
        hc_nbt_add(a, outer, in1);
        hc_nbt_put(a, r, "P", outer);
        static const uint8_t want[] = {
            0x0A, 0x00, 0x00,                         /* 루트 */
            0x09, 0x00, 0x01, 'P',                    /* List P */
            0x09, 0x00, 0x00, 0x00, 0x02,             /* etag=List, n=2 */
            0x00, 0x00, 0x00, 0x00, 0x00,             /* 빈 리스트: End,0 */
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        expect_bytes("list_of_empty_lists", r, want, sizeof want);
    }
    {
        /* Long/LongArray/ByteArray 빅엔디언 */
        hc_nbt_t *r = hc_nbt_compound(a);
        static const int64_t longs[2] = {0x0102030405060708LL, -1};
        hc_nbt_put(a, r, "L", hc_nbt_long_array(a, longs, 2));
        static const uint8_t want[] = {
            0x0A, 0x00, 0x00, 0x0C, 0x00, 0x01, 'L',
            0x00, 0x00, 0x00, 0x02,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        expect_bytes("long_array", r, want, sizeof want);
    }
    {
        hc_nbt_t *r = hc_nbt_compound(a);
        static const uint8_t data[3] = {0x00, 0x7F, 0xFF};
        hc_nbt_put(a, r, "B", hc_nbt_byte_array(a, data, 3));
        static const uint8_t want[] = {0x0A, 0x00, 0x00, 0x07, 0x00, 0x01,
                                       'B',  0x00, 0x00, 0x00, 0x03, 0x00,
                                       0x7F, 0xFF, 0x00};
        expect_bytes("byte_array", r, want, sizeof want);
    }
    {
        /* 컴파운드 값 + Short/Int/Long 스칼라 */
        hc_nbt_t *r = hc_nbt_compound(a);
        hc_nbt_t *c = hc_nbt_compound(a);
        hc_nbt_put(a, c, "i", hc_nbt_int(a, -2));
        hc_nbt_put(a, r, "c", c);
        static const uint8_t want[] = {0x0A, 0x00, 0x00, 0x0A, 0x00, 0x01,
                                       'c',  0x03, 0x00, 0x01, 'i',  0xFF,
                                       0xFF, 0xFF, 0xFE, 0x00, 0x00};
        expect_bytes("nested_compound", r, want, sizeof want);
    }

    /* 버퍼 초과 → -1 */
    {
        hc_nbt_t *r = hc_nbt_compound(a);
        hc_nbt_put(a, r, "s", hc_nbt_string(a, "0123456789"));
        uint8_t   tiny[8];
        ptrdiff_t n = hc_nbt_write(r, tiny, sizeof tiny);
        if (n != -1) {
            fprintf(stderr, "FAIL overflow: got %td want -1\n", n);
            g_fail = 1;
        } else {
            printf("ok  overflow -1\n");
        }
    }
}

int main(void) {
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    test_orders();
    test_bytes();
    if (g_fail) {
        fprintf(stderr, "test_nbt: FAIL\n");
        return 1;
    }
    printf("test_nbt: PASS\n");
    return 0;
}
