/* Task 14 — mineshaft (normal 타입) 조립 + postProcess 배치.
 *
 * 시맨틱 SoT: .hermes/notes/task14-fullregion/
 * R-mineshaft-dungeon-placement.md §A.1~§A.4 (26.2 디컴파일 핀). 디컴파일
 * 재확인 출처 (본 파일 인용의 파일:라인):
 *  - tools/golden/work/task14-decomp/structures/MineshaftPieces.java
 *  - tools/golden/work/task14-decomp/structures/MineshaftStructure.java
 *  - tools/golden/work/task14-decomp/StructurePiece.java
 *  - tools/golden/work/task14-decomp/pieces/StructurePiecesBuilder.java
 *  - tools/golden/work/task14-decomp/ChunkGenerator.java (getWritableArea)
 *
 * RNG 이원화 (§A.1/§C.7):
 *  - 조립 = WorldgenRandom(LegacyRandomSource) setLargeFeatureSeed(seed,
 *    scx, scz) — 순수 48bit LCG (hc_lcg_t; hc_lcg_init 은 자바 setSeed
 *    스크램블 (seed^0x5DEECE66D)&mask 포함 — rng.c:144-146 확인).
 *  - 배치 = 데코 워크 공유 hc_wgr_t (setFeatureSeed 후 피스들 이어쓰기,
 *    호출자 hc_structures_step 책임).
 *
 * normal 타입 전용 — mesa 분기 (MineshaftStructure.java:62-68,
 * dark_oak + WORLD_SURFACE_WG 드로우) 는 이 리전 밖이라 미구현. */

#include "hc_structures.h"

#include "features_internal.h" /* hc_featx_die / hc_featx_face_sturdy_full */

#include <string.h>

#define die hc_featx_die

/* Direction 인코딩 = get2DDataValue (S=0, W=1, N=2, E=3).
 * TODO(계약): hc_spiece_t.ms_dir 주석의 "아래 표" 가 헤더에 없다 —
 * o 필드와 동일한 get2DDataValue 인코딩을 채택 (조립/배치가 전부 이
 * 파일 안이라 내부 일관성으로 충분하고, 직렬화의 피스 "O" 태그와
 * Crossing "D" 태그 (Direction.LEGACY_ID_CODEC_2D, MineshaftPieces
 * .java:637) 도 같은 값이다). */
enum { MS_DIR_S = 0, MS_DIR_W = 1, MS_DIR_N = 2, MS_DIR_E = 3 };

/* ---------- bbox 유틸 (BoundingBox 대응; 레이아웃 = hc_spiece_t.bb) ---------- */

static void bb_set6(int32_t b[6], int32_t x0, int32_t y0, int32_t z0,
                    int32_t x1, int32_t y1, int32_t z1) {
    b[0] = x0;
    b[1] = y0;
    b[2] = z0;
    b[3] = x1;
    b[4] = y1;
    b[5] = z1;
}

static void bb_move(int32_t b[6], int32_t dx, int32_t dy, int32_t dz) {
    b[0] += dx;
    b[1] += dy;
    b[2] += dz;
    b[3] += dx;
    b[4] += dy;
    b[5] += dz;
}

/* BoundingBox.intersects (3D, BoundingBox.java:126-133) */
static int bb_intersects(const int32_t a[6], const int32_t b[6]) {
    return a[3] >= b[0] && a[0] <= b[3] && a[5] >= b[2] && a[2] <= b[5] &&
           a[4] >= b[1] && a[1] <= b[4];
}

static int32_t imin32(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t imax32(int32_t a, int32_t b) { return a > b ? a : b; }

/* ==========================================================================
 * 1. 조립 — hc_mineshaft_assemble (§A.1 findGenerationPoint + §A.2 DFS)
 * ========================================================================== */

enum { MS_MAX_PIECES = 2048, MS_MAX_ENTRANCES = 16 };

/* DFS 스크래치 — 단일 스레드 전제 (코어 관행: 정적 지연 캐시들과 동일).
 * 최종 결과만 arena 로 복사한다. */
static hc_spiece_t g_asm_pieces[MS_MAX_PIECES];
static int32_t     g_asm_entr[MS_MAX_ENTRANCES][6];

typedef struct {
    hc_lcg_t     r;      /* setLargeFeatureSeed 스트림 */
    hc_spiece_t *pc;     /* pieces (생성 순 = 자바 리스트 순) */
    int32_t      n;
    int32_t    (*entr)[6]; /* Room childEntranceBoxes (추가 순) */
    int32_t      n_entr;
} ms_asm_t;

static int ms_next_bool_lcg(ms_asm_t *c) { return hc_lcg_next(&c->r, 1) != 0; }

/* findCollisionPiece (StructurePiece.java:547-557) — 리스트 선형 스캔,
 * 첫 교차. 호출부는 null 여부만 쓰므로 발견 플래그만 돌려준다. */
static int ms_has_collision(const ms_asm_t *c, const int32_t box[6]) {
    for (int32_t i = 0; i < c->n; i++)
        if (bb_intersects(c->pc[i].bb, box))
            return 1;
    return 0;
}

/* findCrossing (MineshaftPieces.java:658-681):
 * draw nextInt(4)==0 ? y1=6 : y1=2; 방향별 로컬 박스 + foot 이동; 충돌 시
 * 실패 (드로우는 이미 소모). 자바 switch default = NORTH. */
static int ms_find_crossing(ms_asm_t *c, int32_t fx, int32_t fy, int32_t fz,
                            int dir, int32_t out[6]) {
    int32_t y1 = hc_lcg_next_int(&c->r, 4) == 0 ? 6 : 2;
    switch (dir) {
    case MS_DIR_S: bb_set6(out, -1, 0, 0, 3, y1, 4); break;
    case MS_DIR_W: bb_set6(out, -4, 0, -1, 0, y1, 3); break;
    case MS_DIR_E: bb_set6(out, 0, 0, -1, 4, y1, 3); break;
    case MS_DIR_N:
    default: bb_set6(out, -1, 0, -4, 3, y1, 0); break;
    }
    bb_move(out, fx, fy, fz);
    return !ms_has_collision(c, out);
}

/* findStairs (:1333-1350) — 드로우 0 */
static int ms_find_stairs(ms_asm_t *c, int32_t fx, int32_t fy, int32_t fz,
                          int dir, int32_t out[6]) {
    switch (dir) {
    case MS_DIR_S: bb_set6(out, 0, -5, 0, 2, 2, 8); break;
    case MS_DIR_W: bb_set6(out, -8, -5, 0, 0, 2, 2); break;
    case MS_DIR_E: bb_set6(out, 0, -5, 0, 8, 2, 2); break;
    case MS_DIR_N:
    default: bb_set6(out, 0, -5, -8, 2, 2, 0); break;
    }
    bb_move(out, fx, fy, fz);
    return !ms_has_collision(c, out);
}

/* findCorridorSize (:160-184): draw len = nextInt(3)+2; len..1 내림 루프
 * (추가 드로우 없음), 첫 비충돌 박스 채택. */
static int ms_find_corridor(ms_asm_t *c, int32_t fx, int32_t fy, int32_t fz,
                            int dir, int32_t out[6]) {
    for (int32_t len = hc_lcg_next_int(&c->r, 3) + 2; len > 0; len--) {
        int32_t bl = len * 5;
        switch (dir) {
        case MS_DIR_S: bb_set6(out, 0, 0, 0, 2, 2, bl - 1); break;
        case MS_DIR_W: bb_set6(out, -(bl - 1), 0, 0, 0, 2, 2); break;
        case MS_DIR_E: bb_set6(out, 0, 0, 0, bl - 1, 2, 2); break;
        case MS_DIR_N:
        default: bb_set6(out, 0, 0, -(bl - 1), 2, 2, 0); break;
        }
        bb_move(out, fx, fy, fz);
        if (!ms_has_collision(c, out))
            return 1;
    }
    return 0;
}

static hc_spiece_t *ms_gen_piece(ms_asm_t *c, int32_t fx, int32_t fy,
                                 int32_t fz, int dir, int32_t depth);

/* createRandomShaftPiece (:51-86): draw nextInt(100) → 종류 선택.
 * Corridor 생성자 (:141-157): setOrientation(dir); draw hasRails =
 * nextInt(3)==0; spider = !hasRails && nextInt(23)==0 (단락 — hasRails 면
 * nextInt(23) 미드로우!); numSections = (축 Z 면 ZSpan 아니면 XSpan)/5.
 * Crossing 생성자 (:647-656): 드로우 0; orientation 미설정 (o=-1);
 * isTwoFloored = YSpan>3. Stairs 생성자: setOrientation 만. */
static hc_spiece_t *ms_create_random(ms_asm_t *c, int32_t fx, int32_t fy,
                                     int32_t fz, int dir, int32_t gd) {
    if (c->n >= MS_MAX_PIECES)
        die("mineshaft piece scratch overflow", NULL);
    hc_spiece_t *p = &c->pc[c->n];
    memset(p, 0, sizeof *p);
    p->gd = gd;
    p->ms_dir = (uint8_t)dir;
    p->o = -1;
    int32_t sel = hc_lcg_next_int(&c->r, 100);
    int32_t box[6];
    if (sel >= 80) {
        if (!ms_find_crossing(c, fx, fy, fz, dir, box))
            return NULL;
        p->kind = HC_SP_MS_CROSSING;
        memcpy(p->bb, box, sizeof box);
        p->is_two_floored = (box[4] - box[1] + 1) > 3; /* y1==6 일 때만 */
    } else if (sel >= 70) {
        if (!ms_find_stairs(c, fx, fy, fz, dir, box))
            return NULL;
        p->kind = HC_SP_MS_STAIRS;
        memcpy(p->bb, box, sizeof box);
        p->o = (int8_t)dir; /* setOrientation(direction) */
    } else {
        if (!ms_find_corridor(c, fx, fy, fz, dir, box))
            return NULL;
        p->kind = HC_SP_MS_CORRIDOR;
        memcpy(p->bb, box, sizeof box);
        p->o = (int8_t)dir;
        p->has_rails = hc_lcg_next_int(&c->r, 3) == 0;
        p->spider = !p->has_rails && hc_lcg_next_int(&c->r, 23) == 0;
        int32_t span = (dir == MS_DIR_N || dir == MS_DIR_S)
                           ? box[5] - box[2] + 1
                           : box[3] - box[0] + 1;
        p->num_sections = span / 5;
    }
    return p;
}

/* MineShaftCorridor.addChildren (:186-368): draw endSel = nextInt(4);
 * orientation 별 끝단 연장 표 (§A.2) — y* = minY-1+nextInt(3) 는 브랜치
 * 선택 후 인자 평가 시점 드로우 (generateAndAddPiece 가 depth 초과로
 * 즉시 null 을 돌려줘도 이미 소모됨). 이어서 depth<8 이면 측면 스캔
 * (5칸 간격, 반복마다 draw nextInt(5); 0/1 만 분기). */
static void ms_corridor_children(ms_asm_t *c, hc_spiece_t *p) {
    int32_t        d = p->gd;
    const int32_t *b = p->bb;
    int32_t        es = hc_lcg_next_int(&c->r, 4);
    int32_t        ys;
    switch (p->o) {
    case MS_DIR_N:
    default:
        if (es <= 1) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[0], ys, b[2] - 1, MS_DIR_N, d);
        } else if (es == 2) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[0] - 1, ys, b[2], MS_DIR_W, d);
        } else {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[3] + 1, ys, b[2], MS_DIR_E, d);
        }
        break;
    case MS_DIR_S:
        if (es <= 1) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[0], ys, b[5] + 1, MS_DIR_S, d);
        } else if (es == 2) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[0] - 1, ys, b[5] - 3, MS_DIR_W, d);
        } else {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[3] + 1, ys, b[5] - 3, MS_DIR_E, d);
        }
        break;
    case MS_DIR_W:
        if (es <= 1) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[0] - 1, ys, b[2], MS_DIR_W, d);
        } else if (es == 2) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[0], ys, b[2] - 1, MS_DIR_N, d);
        } else {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[0], ys, b[5] + 1, MS_DIR_S, d);
        }
        break;
    case MS_DIR_E:
        if (es <= 1) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[3] + 1, ys, b[2], MS_DIR_E, d);
        } else if (es == 2) {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[3] - 3, ys, b[2] - 1, MS_DIR_N, d);
        } else {
            ys = b[1] - 1 + hc_lcg_next_int(&c->r, 3);
            ms_gen_piece(c, b[3] - 3, ys, b[5] + 1, MS_DIR_S, d);
        }
        break;
    }
    if (d < 8) {
        if (p->o != MS_DIR_N && p->o != MS_DIR_S) {
            /* E/W 코리도: x 스캔, 0→북 1→남 (:341-352) */
            for (int32_t x = b[0] + 3; x + 3 <= b[3]; x += 5) {
                int32_t sel = hc_lcg_next_int(&c->r, 5);
                if (sel == 0)
                    ms_gen_piece(c, x, b[1], b[2] - 1, MS_DIR_N, d + 1);
                else if (sel == 1)
                    ms_gen_piece(c, x, b[1], b[5] + 1, MS_DIR_S, d + 1);
            }
        } else {
            /* N/S 코리도: z 스캔, 0→서 1→동 (:354-365) */
            for (int32_t z = b[2] + 3; z + 3 <= b[5]; z += 5) {
                int32_t sel = hc_lcg_next_int(&c->r, 5);
                if (sel == 0)
                    ms_gen_piece(c, b[0] - 1, b[1], z, MS_DIR_W, d + 1);
                else if (sel == 1)
                    ms_gen_piece(c, b[3] + 1, b[1], z, MS_DIR_E, d + 1);
            }
        }
    }
}

/* MineShaftCrossing.addChildren (:683-869): 저층 3분기 (드로우 0) →
 * isTwoFloored 면 상층 4게이트 순서 N→W→E→S, 각각 draw nextBoolean. */
static void ms_crossing_children(ms_asm_t *c, hc_spiece_t *p) {
    int32_t        d = p->gd;
    const int32_t *b = p->bb;
    switch (p->ms_dir) {
    case MS_DIR_N:
    default:
        ms_gen_piece(c, b[0] + 1, b[1], b[2] - 1, MS_DIR_N, d);
        ms_gen_piece(c, b[0] - 1, b[1], b[2] + 1, MS_DIR_W, d);
        ms_gen_piece(c, b[3] + 1, b[1], b[2] + 1, MS_DIR_E, d);
        break;
    case MS_DIR_S:
        ms_gen_piece(c, b[0] + 1, b[1], b[5] + 1, MS_DIR_S, d);
        ms_gen_piece(c, b[0] - 1, b[1], b[2] + 1, MS_DIR_W, d);
        ms_gen_piece(c, b[3] + 1, b[1], b[2] + 1, MS_DIR_E, d);
        break;
    case MS_DIR_W:
        ms_gen_piece(c, b[0] + 1, b[1], b[2] - 1, MS_DIR_N, d);
        ms_gen_piece(c, b[0] + 1, b[1], b[5] + 1, MS_DIR_S, d);
        ms_gen_piece(c, b[0] - 1, b[1], b[2] + 1, MS_DIR_W, d);
        break;
    case MS_DIR_E:
        ms_gen_piece(c, b[0] + 1, b[1], b[2] - 1, MS_DIR_N, d);
        ms_gen_piece(c, b[0] + 1, b[1], b[5] + 1, MS_DIR_S, d);
        ms_gen_piece(c, b[3] + 1, b[1], b[2] + 1, MS_DIR_E, d);
        break;
    }
    if (p->is_two_floored) {
        /* minY + 3 + 1 (:823 등) */
        if (ms_next_bool_lcg(c))
            ms_gen_piece(c, b[0] + 1, b[1] + 4, b[2] - 1, MS_DIR_N, d);
        if (ms_next_bool_lcg(c))
            ms_gen_piece(c, b[0] - 1, b[1] + 4, b[2] + 1, MS_DIR_W, d);
        if (ms_next_bool_lcg(c))
            ms_gen_piece(c, b[3] + 1, b[1] + 4, b[2] + 1, MS_DIR_E, d);
        if (ms_next_bool_lcg(c))
            ms_gen_piece(c, b[0] + 1, b[1] + 4, b[5] + 1, MS_DIR_S, d);
    }
}

/* MineShaftStairs.addChildren (:1352-1407): 직진 1개, 드로우 0 */
static void ms_stairs_children(ms_asm_t *c, hc_spiece_t *p) {
    int32_t        d = p->gd;
    const int32_t *b = p->bb;
    switch (p->o) {
    case MS_DIR_S: ms_gen_piece(c, b[0], b[1], b[5] + 1, MS_DIR_S, d); break;
    case MS_DIR_W: ms_gen_piece(c, b[0] - 1, b[1], b[2], MS_DIR_W, d); break;
    case MS_DIR_E: ms_gen_piece(c, b[3] + 1, b[1], b[2], MS_DIR_E, d); break;
    case MS_DIR_N:
    default: ms_gen_piece(c, b[0], b[1], b[2] - 1, MS_DIR_N, d); break;
    }
}

/* generateAndAddPiece (:89-116) — 자식 생성의 관문. depth>8 / 80블록
 * 거리 검사 (드로우 전!) → createRandomShaftPiece → addPiece 후 즉시
 * addChildren (DFS = 리스트 순서). 거리 기준은 startPiece(=Room) bbox
 * 의 minX/minZ. */
static hc_spiece_t *ms_gen_piece(ms_asm_t *c, int32_t fx, int32_t fy,
                                 int32_t fz, int dir, int32_t depth) {
    if (depth > 8)
        return NULL;
    int32_t dx = fx - c->pc[0].bb[0];
    int32_t dz = fz - c->pc[0].bb[2];
    if (dx < 0)
        dx = -dx;
    if (dz < 0)
        dz = -dz;
    if (dx > 80 || dz > 80)
        return NULL;
    hc_spiece_t *p = ms_create_random(c, fx, fy, fz, dir, depth + 1);
    if (p) {
        c->n++; /* addPiece */
        switch (p->kind) {
        case HC_SP_MS_CORRIDOR: ms_corridor_children(c, p); break;
        case HC_SP_MS_CROSSING: ms_crossing_children(c, p); break;
        case HC_SP_MS_STAIRS: ms_stairs_children(c, p); break;
        default: die("mineshaft: unexpected child kind", NULL);
        }
    }
    return p;
}

static void ms_add_entrance(ms_asm_t *c, int32_t x0, int32_t y0, int32_t z0,
                            int32_t x1, int32_t y1, int32_t z1) {
    if (c->n_entr >= MS_MAX_ENTRANCES)
        die("mineshaft entrance scratch overflow", NULL); /* 벽당 최대 3 */
    bb_set6(c->entr[c->n_entr++], x0, y0, z0, x1, y1, z1);
}

/* MineShaftRoom.addChildren (:1124-1246): heightSpace = max(YSpan-4, 1);
 * 4벽 패스 순서 북→남→서→동. 각 패스:
 *   pos += nextInt(span) [draw]; pos+3 > span 이면 break;
 *   y = minY + nextInt(heightSpace) + 1 [draw, 인자 평가 시점];
 *   generateAndAddPiece(...); 성공 시 childEntranceBoxes 기록;
 *   실패해도 pos += 4. */
static void ms_room_children(ms_asm_t *c, hc_spiece_t *room) {
    int32_t        d = room->gd; /* 0 */
    const int32_t *b = room->bb;
    int32_t        hs = (b[4] - b[1] + 1) - 3 - 1;
    if (hs <= 0)
        hs = 1;
    int32_t xspan = b[3] - b[0] + 1, zspan = b[5] - b[2] + 1;
    int32_t pos;

    /* 북벽 */
    pos = 0;
    while (pos < xspan) {
        pos += hc_lcg_next_int(&c->r, xspan);
        if (pos + 3 > xspan)
            break;
        int32_t      y = b[1] + hc_lcg_next_int(&c->r, hs) + 1;
        hc_spiece_t *ch =
            ms_gen_piece(c, b[0] + pos, y, b[2] - 1, MS_DIR_N, d);
        if (ch)
            ms_add_entrance(c, ch->bb[0], ch->bb[1], b[2], ch->bb[3],
                            ch->bb[4], b[2] + 1);
        pos += 4;
    }
    /* 남벽 */
    pos = 0;
    while (pos < xspan) {
        pos += hc_lcg_next_int(&c->r, xspan);
        if (pos + 3 > xspan)
            break;
        int32_t      y = b[1] + hc_lcg_next_int(&c->r, hs) + 1;
        hc_spiece_t *ch =
            ms_gen_piece(c, b[0] + pos, y, b[5] + 1, MS_DIR_S, d);
        if (ch)
            ms_add_entrance(c, ch->bb[0], ch->bb[1], b[5] - 1, ch->bb[3],
                            ch->bb[4], b[5]);
        pos += 4;
    }
    /* 서벽 */
    pos = 0;
    while (pos < zspan) {
        pos += hc_lcg_next_int(&c->r, zspan);
        if (pos + 3 > zspan)
            break;
        int32_t      y = b[1] + hc_lcg_next_int(&c->r, hs) + 1;
        hc_spiece_t *ch =
            ms_gen_piece(c, b[0] - 1, y, b[2] + pos, MS_DIR_W, d);
        if (ch)
            ms_add_entrance(c, b[0], ch->bb[1], ch->bb[2], b[0] + 1,
                            ch->bb[4], ch->bb[5]);
        pos += 4;
    }
    /* 동벽 */
    pos = 0;
    while (pos < zspan) {
        pos += hc_lcg_next_int(&c->r, zspan);
        if (pos + 3 > zspan)
            break;
        int32_t      y = b[1] + hc_lcg_next_int(&c->r, hs) + 1;
        hc_spiece_t *ch =
            ms_gen_piece(c, b[3] + 1, y, b[2] + pos, MS_DIR_E, d);
        if (ch)
            ms_add_entrance(c, b[3] - 1, ch->bb[1], ch->bb[2], b[3],
                            ch->bb[4], ch->bb[5]);
        pos += 4;
    }
}

int32_t hc_mineshaft_assemble(hc_arena_t *a, int64_t seed, int32_t scx,
                              int32_t scz, hc_spiece_t **out,
                              int32_t *stub_x, int32_t *stub_y,
                              int32_t *stub_z) {
    ms_asm_t c;
    c.pc = g_asm_pieces;
    c.n = 0;
    c.entr = g_asm_entr;
    c.n_entr = 0;

    /* RNG = setLargeFeatureSeed(seed, scx, scz) (WorldgenRandom.java:39-46):
     * setSeed(seed); a=nextLong(); b=nextLong(); setSeed(scx*a ^ scz*b ^
     * seed). 곱/XOR 은 64bit 랩어라운드 — 무부호로 계산. */
    hc_lcg_init(&c.r, seed);
    int64_t ra = hc_lcg_next_long(&c.r);
    int64_t rb = hc_lcg_next_long(&c.r);
    hc_lcg_init(&c.r, (int64_t)((uint64_t)(int64_t)scx * (uint64_t)ra ^
                                (uint64_t)(int64_t)scz * (uint64_t)rb ^
                                (uint64_t)seed));

    /* ① findGenerationPoint 서두의 폐기 nextDouble (MineshaftStructure
     * .java:44 — 레거시 확률검사 잔재, 2 next 소모) */
    (void)hc_lcg_next_double(&c.r);

    /* ② Room bbox — BoundingBox(w,50,n, w+7+nextInt(6), 54+nextInt(6),
     * n+7+nextInt(6)); w=16*scx+2, n=16*scz+2 (chunkPos.getBlockX(2)/
     * getBlockZ(2)); 인자 평가 순서대로 draw dX, dY, dZ
     * (MineshaftPieces.java:1103-1117). orientation 미설정 (o=-1). */
    {
        hc_spiece_t *room = &c.pc[0];
        memset(room, 0, sizeof *room);
        int32_t west = 16 * scx + 2, north = 16 * scz + 2;
        int32_t dxr = hc_lcg_next_int(&c.r, 6);
        int32_t dyr = hc_lcg_next_int(&c.r, 6);
        int32_t dzr = hc_lcg_next_int(&c.r, 6);
        room->kind = HC_SP_MS_ROOM;
        room->o = -1;
        room->gd = 0;
        room->ms_dir = 0; /* Room 은 방향 없음 (미사용) */
        bb_set6(room->bb, west, 50, north, west + 7 + dxr, 54 + dyr,
                north + 7 + dzr);
        c.n = 1; /* builder.addPiece(room) */
    }

    /* ③ room.addChildren DFS (§A.2 전부) */
    ms_room_children(&c, &c.pc[0]);

    /* ④ moveBelowSeaLevel(seaLevel=63, minY=-64, r, offset=10)
     * (StructurePiecesBuilder.java:31-42):
     *   maxY = 63-10 = 53; bbox = 전 피스 encapsulation;
     *   y1Pos = YSpan + (-64) + 1; y1Pos < 53 이면 += nextInt(53-y1Pos)
     *   [조건부 1 드로우]; dy = y1Pos - bbox.maxY; 전 피스 move(0,dy,0)
     *   — Room 은 move 오버라이드로 childEntranceBoxes 도 이동
     *   (MineshaftPieces.java:1303-1309). */
    int32_t dy;
    {
        int32_t mny = c.pc[0].bb[1], mxy = c.pc[0].bb[4];
        for (int32_t i = 1; i < c.n; i++) {
            mny = imin32(mny, c.pc[i].bb[1]);
            mxy = imax32(mxy, c.pc[i].bb[4]);
        }
        int32_t y1pos = (mxy - mny + 1) + HC_MIN_Y + 1;
        if (y1pos < 53)
            y1pos += hc_lcg_next_int(&c.r, 53 - y1pos);
        dy = y1pos - mxy;
        for (int32_t i = 0; i < c.n; i++) {
            c.pc[i].bb[1] += dy;
            c.pc[i].bb[4] += dy;
        }
        for (int32_t i = 0; i < c.n_entr; i++) {
            c.entr[i][1] += dy;
            c.entr[i][4] += dy;
        }
    }

    /* ⑤ stub = startPos.offset(0, dy, 0), startPos = (16*scx+8, 50,
     * 16*scz) — x 는 중앙(+8), z 는 최소(+0) 비대칭 (MineshaftStructure
     * .java:46). 바이옴 검사는 호출자 몫. */
    *stub_x = 16 * scx + 8;
    *stub_y = 50 + dy;
    *stub_z = 16 * scz;

    /* arena 로 확정 복사 (생성 순서 그대로) */
    hc_spiece_t *arr = hc_arena_alloc(a, sizeof(hc_spiece_t) * (size_t)c.n,
                                      _Alignof(hc_spiece_t));
    if (!arr)
        return -1;
    memcpy(arr, c.pc, sizeof(hc_spiece_t) * (size_t)c.n);
    if (c.n_entr > 0) {
        int32_t(*eb)[6] = hc_arena_alloc(
            a, sizeof(int32_t[6]) * (size_t)c.n_entr, _Alignof(int32_t));
        if (!eb)
            return -1;
        memcpy(eb, c.entr, sizeof(int32_t[6]) * (size_t)c.n_entr);
        arr[0].entrances = eb;
        arr[0].n_entrances = c.n_entr;
    }
    *out = arr;
    return c.n;
}

/* ==========================================================================
 * 2. 배치 — hc_splace_mineshaft (§A.3 공통 헬퍼 + §A.4 postProcess)
 * ========================================================================== */

/* 배치 상수 상태 — hc_block_by_name 으로 최초 1회 해석해 정적 캐시
 * (normal 타입: oak 계열; NAMES 캐노니컬 문자열은 blocks.c). */
static struct {
    int      inited;
    uint16_t planks;       /* oak_planks */
    uint16_t fence;        /* oak_fence 기본 상태 (전부 false) */
    uint16_t fence_w;      /* oak_fence[west=true] */
    uint16_t fence_e;      /* oak_fence[east=true] */
    uint16_t chain;        /* iron_chain[axis=y,waterlogged=false] */
    uint16_t cobweb;       /* cobweb */
    uint16_t rail_ns;      /* rail[shape=north_south,waterlogged=false] */
    uint16_t rail_ew;      /* rail[shape=east_west,waterlogged=false] */
    uint16_t wall_torch_n; /* wall_torch[facing=north] */
    uint16_t wall_torch_s; /* wall_torch[facing=south] */
    uint16_t susp_sand;    /* suspicious_sand[dusted=0] (FallingBlock) */
} g_msb;

static uint16_t ms_resolve(const char *name) {
    int32_t id = hc_block_by_name(name, (int32_t)strlen(name));
    if (id < 0)
        die("mineshaft block state missing", name);
    return (uint16_t)id;
}

static void ms_blocks_init(void) {
    if (g_msb.inited)
        return;
    g_msb.planks = ms_resolve("minecraft:oak_planks");
    g_msb.fence = ms_resolve("minecraft:oak_fence[east=false,north=false,"
                             "south=false,waterlogged=false,west=false]");
    g_msb.fence_w = ms_resolve("minecraft:oak_fence[east=false,north=false,"
                               "south=false,waterlogged=false,west=true]");
    g_msb.fence_e = ms_resolve("minecraft:oak_fence[east=true,north=false,"
                               "south=false,waterlogged=false,west=false]");
    g_msb.chain = ms_resolve("minecraft:iron_chain[axis=y,waterlogged=false]");
    g_msb.cobweb = ms_resolve("minecraft:cobweb");
    g_msb.rail_ns =
        ms_resolve("minecraft:rail[shape=north_south,waterlogged=false]");
    g_msb.rail_ew =
        ms_resolve("minecraft:rail[shape=east_west,waterlogged=false]");
    g_msb.wall_torch_n = ms_resolve("minecraft:wall_torch[facing=north]");
    g_msb.wall_torch_s = ms_resolve("minecraft:wall_torch[facing=south]");
    g_msb.susp_sand = ms_resolve("minecraft:suspicious_sand[dusted=0]");
    g_msb.inited = 1;
}

/* 상태 base 이름 (== '[' 앞) 일치 — BlockState.is(Block) 대응 */
static int ms_base_is(uint16_t s, const char *base) {
    const char *n = hc_block_name(s);
    size_t      l = strlen(base);
    return strncmp(n, base, l) == 0 && (n[l] == '\0' || n[l] == '[');
}

typedef struct {
    hc_sctx_t        *sc;
    hc_feat_region_t *rg;
    hc_spiece_t      *p;
    hc_wgr_t         *rng;
    int32_t           cb[6]; /* chunkBB = getWritableArea (아래 주석) */
    int               mir, rot; /* HC_MIR_* / HC_ROT_* (o<0 → NONE) */
} ms_env_t;

static int ms_next_bool(ms_env_t *e) { return hc_wgr_next(e->rng, 1) != 0; }

/* Direction.values() 순서 D,U,N,S,W,E 의 오프셋/반대면 (features.c
 * face_sturdy 의 dir_mc 규약과 동일) */
static const int8_t MS_DIR_OFF[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                        {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
static const int8_t MS_DIR_OPP[6] = {1, 0, 3, 2, 5, 4};

/* --- getWorldPos (StructurePiece.java:140-174) ---
 * orientation null(o=-1) → 항등 (Room/Crossing 은 절대좌표);
 * N/S → minX+x / W → maxX-z / E → minX+z; y → minY+y;
 * z: N → maxZ-z / S → minZ+z / W,E → minZ+x. */
static int32_t ms_world_x(const ms_env_t *e, int32_t x, int32_t z) {
    switch (e->p->o) {
    case MS_DIR_N:
    case MS_DIR_S: return e->p->bb[0] + x;
    case MS_DIR_W: return e->p->bb[3] - z;
    case MS_DIR_E: return e->p->bb[0] + z;
    default: return x;
    }
}
static int32_t ms_world_y(const ms_env_t *e, int32_t y) {
    return e->p->o < 0 ? y : y + e->p->bb[1];
}
static int32_t ms_world_z(const ms_env_t *e, int32_t x, int32_t z) {
    switch (e->p->o) {
    case MS_DIR_N: return e->p->bb[5] - z;
    case MS_DIR_S: return e->p->bb[2] + z;
    case MS_DIR_W:
    case MS_DIR_E: return e->p->bb[2] + x;
    default: return z;
    }
}

/* chunkBB.isInside (3D, BoundingBox.java:237-239) */
static int ms_inside(const ms_env_t *e, int32_t x, int32_t y, int32_t z) {
    return x >= e->cb[0] && x <= e->cb[3] && z >= e->cb[2] && z <= e->cb[5] &&
           y >= e->cb[1] && y <= e->cb[4];
}

/* getBlock (StructurePiece.java:214-219): chunkBB 밖 → AIR */
static uint16_t ms_get_block(const ms_env_t *e, int32_t x, int32_t y,
                             int32_t z) {
    int32_t wx = ms_world_x(e, x, z), wy = ms_world_y(e, y),
            wz = ms_world_z(e, x, z);
    if (!ms_inside(e, wx, wy, wz))
        return HC_B_AIR;
    return hc_feat_get_block(e->rg, wx, wy, wz);
}

/* MineShaftPiece.canBeReplaced 오버라이드 (MineshaftPieces.java:1016-1022):
 * 기존 블록이 type 의 planks / wood(oak_log 전 상태) / fence(전 상태) /
 * IRON_CHAIN(전 상태) 이면 거부. getBlock 경유 — chunkBB 밖은 AIR 로
 * 읽혀 통과. (지시문에는 wood 가 빠져 있으나 디컴파일 :1019 에 명시 —
 * 디컴파일을 따른다.) */
static int ms_can_be_replaced(const ms_env_t *e, int32_t x, int32_t y,
                              int32_t z) {
    uint16_t s = ms_get_block(e, x, y, z);
    return !ms_base_is(s, "minecraft:oak_planks") &&
           !ms_base_is(s, "minecraft:oak_log") &&
           !ms_base_is(s, "minecraft:oak_fence") &&
           !ms_base_is(s, "minecraft:iron_chain");
}

/* SHAPE_CHECK_BLOCKS (StructurePiece.java:45-58) — 디컴파일 전수 12종.
 * 주의: IRON_BARS 이며 IRON_CHAIN 은 아니다 (지시문 표기와 다름 — §A.3
 * :407-409 재확인). RAIL/COBWEB 도 아님. */
static int ms_shape_check_block(uint16_t s) {
    static const char *const NAMES[] = {
        "minecraft:nether_brick_fence", "minecraft:torch",
        "minecraft:wall_torch",         "minecraft:oak_fence",
        "minecraft:spruce_fence",       "minecraft:dark_oak_fence",
        "minecraft:pale_oak_fence",     "minecraft:acacia_fence",
        "minecraft:birch_fence",        "minecraft:jungle_fence",
        "minecraft:ladder",             "minecraft:iron_bars",
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (ms_base_is(s, NAMES[i]))
            return 1;
    return 0;
}

/* setBlock 직후의 FluidState → 스케줄 주체 유체 kind (features.c 규약:
 * HC_TICK_WATER/LAVA/FLOWING_*). 소스/waterlogged → water|lava; 흐름
 * 상태(level 1..15) → flowing (월드젠 팔레트에는 등장하지 않지만
 * placeBlock 시맨틱 그대로 유지). -1 = 유체 없음. */
static int ms_fluid_tick_kind(uint16_t s) {
    if (s == HC_B_LAVA)
        return HC_TICK_LAVA;
    if (s == HC_B_WATER || hc_block_is_waterlogged(s))
        return HC_TICK_WATER;
    if (s >= HC_B_WATER_FLOW_BASE && s < HC_B_WATER_FLOW_BASE + 15)
        return HC_TICK_FLOWING_WATER;
    return -1;
}

/* 원시 level.setBlock(pos, state, 2) — ProtoChunk 쓰기 + FINAL 하이트맵
 * 유지 (hc_feat_set_block). 비-EntityBlock 상태로 덮인 pos 의 BE 레코드
 * 제거 (ProtoChunk.setBlockState 의 removeBlockEntity 대응 — 이 파일이
 * 놓는 상태 중 BE 는 SPAWNER 뿐이고 그 경로는 별도). */
static void ms_raw_set(ms_env_t *e, int32_t wx, int32_t wy, int32_t wz,
                       uint16_t s) {
    hc_feat_set_block(e->rg, wx, wy, wz, s);
    hc_be_remove(&e->sc->be, wx, wy, wz);
}

/* placeBlock (StructurePiece.java:176-206):
 * ① chunkBB.isInside(3D) ② canBeReplaced ③ mirror 먼저, rotate 나중
 * (S → LEFT_RIGHT / W → LEFT_RIGHT+CW90 / E → CW90; setOrientation
 * :564-588. Room/Crossing 은 mirror/rotation 필드가 자바 null — 기본
 * Block.mirror/rotate 는 항등이므로 무변환) ④ setBlock flag 2
 * ⑤ 그 위치 유체 비어있지 않으면 scheduleTick(pos, 유체, 0)
 * ⑥ SHAPE_CHECK_BLOCKS 면 markPosForPostProcessing. */
static void ms_place_block(ms_env_t *e, uint16_t s, int32_t x, int32_t y,
                           int32_t z) {
    int32_t wx = ms_world_x(e, x, z), wy = ms_world_y(e, y),
            wz = ms_world_z(e, x, z);
    if (!ms_inside(e, wx, wy, wz))
        return;
    if (!ms_can_be_replaced(e, x, y, z))
        return;
    if (e->mir != HC_MIR_NONE)
        s = hc_state_mirror(s, e->mir);
    if (e->rot != HC_ROT_NONE)
        s = hc_state_rotate(s, e->rot);
    ms_raw_set(e, wx, wy, wz, s);
    int k = ms_fluid_tick_kind(s);
    if (k >= 0)
        hc_feat_schedule_tick(e->rg, wx, wy, wz, 0, k, 0);
    if (ms_shape_check_block(s))
        hc_feat_mark_pos(e->rg, wx, wy, wz);
}

/* isInterior (StructurePiece.java:221-226): pos = getWorldPos(x,y+1,z);
 * chunkBB 밖 → false; pos.y < level.getHeight(OCEAN_FLOOR_WG, ...).
 * 타입은 디컴파일 재확인 결과 OCEAN_FLOOR_WG 가 맞다 — 노트의 "라이브"
 * 는 기록 서버의 seq-9 저장/언로드로 *_WG 가 드롭된 뒤 청크·타입별
 * 첫-읽기 재프라임 후 동결되는 의미이고, 그 시맨틱은 hc_feat_height 의
 * wg_dropped 경로가 이미 구현한다 (hc_features.h 참조). */
static int ms_is_interior(ms_env_t *e, int32_t x, int32_t y, int32_t z) {
    int32_t wx = ms_world_x(e, x, z), wy = ms_world_y(e, y + 1),
            wz = ms_world_z(e, x, z);
    if (!ms_inside(e, wx, wy, wz))
        return 0;
    return wy < hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR_WG, wx, wz);
}

/* generateBox (StructurePiece.java:247-273): y 외측 → x → z, 쉘 =
 * edge / 내부 = fill, 전부 placeBlock. 드로우 0. */
static void ms_generate_box(ms_env_t *e, int32_t x0, int32_t y0, int32_t z0,
                            int32_t x1, int32_t y1, int32_t z1, uint16_t edge,
                            uint16_t fill, int skip_air) {
    for (int32_t y = y0; y <= y1; y++)
        for (int32_t x = x0; x <= x1; x++)
            for (int32_t z = z0; z <= z1; z++) {
                if (skip_air && hc_block_is_air(ms_get_block(e, x, y, z)))
                    continue;
                if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 &&
                    z != z1)
                    ms_place_block(e, fill, x, y, z);
                else
                    ms_place_block(e, edge, x, y, z);
            }
}

/* generateMaybeBox (:322-353): 같은 루프 순서, 셀마다 무조건 draw
 * nextFloat 후 > prob 스킵 (skipAir/hasToBeInside 검사는 드로우 뒤). */
static void ms_generate_maybe_box(ms_env_t *e, float prob, int32_t x0,
                                  int32_t y0, int32_t z0, int32_t x1,
                                  int32_t y1, int32_t z1, uint16_t edge,
                                  uint16_t fill, int skip_air,
                                  int has_to_be_inside) {
    for (int32_t y = y0; y <= y1; y++)
        for (int32_t x = x0; x <= x1; x++)
            for (int32_t z = z0; z <= z1; z++) {
                if (hc_wgr_next_float(e->rng) > prob)
                    continue;
                if (skip_air && hc_block_is_air(ms_get_block(e, x, y, z)))
                    continue;
                if (has_to_be_inside && !ms_is_interior(e, x, y, z))
                    continue;
                if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 &&
                    z != z1)
                    ms_place_block(e, fill, x, y, z);
                else
                    ms_place_block(e, edge, x, y, z);
            }
}

/* maybeGenerateBlock (:355-368): draw nextFloat < prob 이면 placeBlock */
static void ms_maybe_generate_block(ms_env_t *e, float prob, int32_t x,
                                    int32_t y, int32_t z, uint16_t s) {
    if (hc_wgr_next_float(e->rng) < prob)
        ms_place_block(e, s, x, y, z);
}

/* generateUpperHalfSphere (:370-407) — 드로우 0. float 스칼라식 그대로
 * (자바 float 연산과 비트 일치 — -ffp-contract=off). */
static void ms_generate_upper_half_sphere(ms_env_t *e, int32_t x0, int32_t y0,
                                          int32_t z0, int32_t x1, int32_t y1,
                                          int32_t z1, uint16_t fill,
                                          int skip_air) {
    float diag_x = (float)(x1 - x0 + 1);
    float diag_y = (float)(y1 - y0 + 1);
    float diag_z = (float)(z1 - z0 + 1);
    float cxf = (float)x0 + diag_x / 2.0f;
    float czf = (float)z0 + diag_z / 2.0f;
    for (int32_t y = y0; y <= y1; y++) {
        float ny = (float)(y - y0) / diag_y;
        for (int32_t x = x0; x <= x1; x++) {
            float nx = ((float)x - cxf) / (diag_x * 0.5f);
            for (int32_t z = z0; z <= z1; z++) {
                float nz = ((float)z - czf) / (diag_z * 0.5f);
                if (skip_air && hc_block_is_air(ms_get_block(e, x, y, z)))
                    continue;
                float d = nx * nx + ny * ny + nz * nz;
                if (d <= 1.05f)
                    ms_place_block(e, fill, x, y, z);
            }
        }
    }
}

/* isReplaceableByStructures (StructurePiece.java:426-428):
 * isAir || liquid() || GLOW_LICHEN || SEAGRASS || TALL_SEAGRASS.
 * liquid() = LiquidBlock 상태 — 월드젠 중엔 소스(level=0)뿐이므로
 * hc_block_is_fluid 로 충분 (흐름 상태는 postProcessGeneration 산물,
 * hc_blocks.h HC_B_WATER_FLOW_BASE 주석). */
static int ms_replaceable_by_structures(uint16_t s) {
    return hc_block_is_air(s) || hc_block_is_fluid(s) ||
           (s >= HC_B_GLOW_LICHEN_BASE && s < HC_B_GLOW_LICHEN_BASE + 126) ||
           s == HC_B_SEAGRASS || s == HC_B_TALL_SEAGRASS_LOWER ||
           s == HC_B_TALL_SEAGRASS_UPPER;
}

/* FallingBlock 판별 — 팔레트 등장분: sand/red_sand/gravel/suspicious_sand
 * (T14 names 전수; suspicious_gravel/콘크리트 파우더/모루는 부재) */
static int ms_is_falling_block(uint16_t s) {
    return s == HC_B_SAND || s == HC_B_RED_SAND || s == HC_B_GRAVEL ||
           s == g_msb.susp_sand;
}

/* canHangChainBelow (MineshaftPieces.java:562-564):
 * Block.canSupportCenter(level, posAbove, DOWN) && !FallingBlock.
 * canSupportCenter = #unstable_bottom_center(펜스게이트 — 팔레트 부재)
 * 검사 후 isFaceSturdy(DOWN, SupportType.CENTER).
 * TODO(계약): isFaceSturdy 는 features.c 관행대로
 * hc_featx_face_sturdy_full (완전 큐브 + azalea UP) 로 축약 — 하프슬랩/
 * 계단의 DOWN 완전면 (CENTER/FULL 모두 sturdy) 은 미포함. 체인업이
 * 위로 최대 50칸을 훑으므로 난파선 슬랩 밑을 지나는 극단 케이스에서
 * 갈릴 수 있다 — 골든 게이트에서 확인 필요. */
static int ms_can_hang_chain_below(uint16_t s) {
    return hc_featx_face_sturdy_full(s, 0 /* DOWN */) &&
           !ms_is_falling_block(s);
}

/* isInInvalidLocation (MineshaftPieces.java:1038-1087) — §A.4 공통 선행.
 * true 면 그 피스의 postProcess 전체 무동작 → 드로우 0 (RNG 정렬에
 * 결정적). bbox ±1 팽창 ∩ chunkBB 클립 후:
 * (1) 중심점 getBiome 이 #mineshaft_blocking (= deep_dark 단일) —
 *     getBiome = BiomeManager 지터 줌 (hc_biome_view_get: hc_biome_zoom
 *     + 쿼트 y 클램프 + 리전 쿼트 그리드; zoom seed 는 뷰에 있음);
 * (2) 상/하면 (x 외측→z 내측, 각 셀에서 y0 → y1 순),
 * (3) 북/남면 (x 외측→y 내측, z0 → z1 순),
 * (4) 서/동면 (z 외측→y 내측, x0 → x1 순) 에서 liquid() 발견 시 true.
 * 블록 읽기는 level 직접 (chunkBB 클립 아님 — 좌표가 이미 클립 결과). */
static int ms_is_in_invalid_location(ms_env_t *e) {
    const int32_t *bb = e->p->bb;
    int32_t x0 = imax32(bb[0] - 1, e->cb[0]);
    int32_t y0 = imax32(bb[1] - 1, e->cb[1]);
    int32_t z0 = imax32(bb[2] - 1, e->cb[2]);
    int32_t x1 = imin32(bb[3] + 1, e->cb[3]);
    int32_t y1 = imin32(bb[4] + 1, e->cb[4]);
    int32_t z1 = imin32(bb[5] + 1, e->cb[5]);
    /* 자바 int 나눗셈 = 0 방향 절사 (C 와 동일) */
    uint16_t bid = hc_biome_view_get(e->sc->view, (x0 + x1) / 2,
                                     (y0 + y1) / 2, (z0 + z1) / 2);
    const char *bname = e->sc->biomes->names[bid];
    if (bname && strcmp(bname, "minecraft:deep_dark") == 0)
        return 1;
    for (int32_t x = x0; x <= x1; x++)
        for (int32_t z = z0; z <= z1; z++) {
            if (hc_block_is_fluid(hc_feat_get_block(e->rg, x, y0, z)))
                return 1;
            if (hc_block_is_fluid(hc_feat_get_block(e->rg, x, y1, z)))
                return 1;
        }
    for (int32_t x = x0; x <= x1; x++)
        for (int32_t y = y0; y <= y1; y++) {
            if (hc_block_is_fluid(hc_feat_get_block(e->rg, x, y, z0)))
                return 1;
            if (hc_block_is_fluid(hc_feat_get_block(e->rg, x, y, z1)))
                return 1;
        }
    for (int32_t z = z0; z <= z1; z++)
        for (int32_t y = y0; y <= y1; y++) {
            if (hc_block_is_fluid(hc_feat_get_block(e->rg, x0, y, z)))
                return 1;
            if (hc_block_is_fluid(hc_feat_get_block(e->rg, x1, y, z)))
                return 1;
        }
    return 0;
}

/* setPlanksBlock (MineshaftPieces.java:1089-1097): isInterior 면, 기존
 * 상태의 윗면이 sturdy 아니면 원시 setBlock(planks, 2). 드로우 0. */
static void ms_set_planks_block(ms_env_t *e, uint16_t planks, int32_t x,
                                int32_t y, int32_t z) {
    if (!ms_is_interior(e, x, y, z))
        return;
    int32_t wx = ms_world_x(e, x, z), wy = ms_world_y(e, y),
            wz = ms_world_z(e, x, z);
    uint16_t cur = hc_feat_get_block(e->rg, wx, wy, wz);
    if (!hc_featx_face_sturdy_full(cur, 1 /* UP */))
        ms_raw_set(e, wx, wy, wz, planks);
}

/* isSupportingBox (:1028-1036): x0..x1 의 (x, y1+1, z) getBlock 이
 * 하나라도 isAir 면 false — chunkBB 밖은 AIR 로 읽혀 false. 드로우 0. */
static int ms_is_supporting_box(ms_env_t *e, int32_t x0, int32_t x1,
                                int32_t y1, int32_t z) {
    for (int32_t x = x0; x <= x1; x++)
        if (hc_block_is_air(ms_get_block(e, x, y1 + 1, z)))
            return 0;
    return 1;
}

static void ms_fill_column_between(ms_env_t *e, uint16_t s, int32_t wx,
                                   int32_t wz, int32_t bottom_incl,
                                   int32_t top_excl) {
    for (int32_t y = bottom_incl; y < top_excl; y++)
        ms_raw_set(e, wx, y, wz, s);
}

/* fillPillarDownOrChainUp (MineshaftPieces.java:512-548): 아래(≤20)로
 * 지지 탐색 → wood 기둥, 또는 위(≤50)로 행잉 탐색 → fence 1 +
 * IRON_CHAIN 체인. 전부 원시 setBlock flag 2, 드로우 0. 아래/위 검사가
 * 같은 dist 에서 번갈아 진행된다 (자바 루프 구조 그대로). */
static void ms_fill_pillar_down_or_chain_up(ms_env_t *e, uint16_t pillar,
                                            int32_t x, int32_t y, int32_t z) {
    int32_t wx = ms_world_x(e, x, z), wy = ms_world_y(e, y),
            wz = ms_world_z(e, x, z);
    if (!ms_inside(e, wx, wy, wz))
        return;
    int check_below = 1, check_above = 1;
    for (int32_t dist = 1; check_below || check_above; dist++) {
        if (check_below) {
            int32_t  yy = wy - dist;
            uint16_t s = hc_feat_get_block(e->rg, wx, yy, wz);
            /* is(Blocks.LAVA) — 월드젠 중 용암은 소스(level=0)뿐 */
            int empty = ms_replaceable_by_structures(s) && s != HC_B_LAVA;
            if (!empty && hc_featx_face_sturdy_full(s, 1 /* UP */)) {
                /* canPlaceColumnOnTopOf = isFaceSturdy(UP) (:558-560) */
                ms_fill_column_between(e, pillar, wx, wz, wy - dist + 1, wy);
                return;
            }
            check_below = dist <= 20 && empty && yy > HC_MIN_Y + 1;
        }
        if (check_above) {
            int32_t  yy = wy + dist;
            uint16_t s = hc_feat_get_block(e->rg, wx, yy, wz);
            int      empty = ms_replaceable_by_structures(s);
            if (!empty && ms_can_hang_chain_below(s)) {
                ms_raw_set(e, wx, wy + 1, wz, g_msb.fence);
                ms_fill_column_between(e, g_msb.chain, wx, wz, wy + 2,
                                       wy + dist);
                return;
            }
            check_above = dist <= 50 && empty && yy < HC_MAX_Y;
        }
    }
}

/* placeDoubleLowerOrUpperSupport (:480-490): (x,y,z)/(x+2,y,z) 가 planks
 * 블록이면 fillPillarDownOrChainUp(wood). 드로우 0. */
static void ms_place_double_support(ms_env_t *e, int32_t x, int32_t y,
                                    int32_t z) {
    /* wood = OAK_LOG.defaultBlockState() = oak_log[axis=y] */
    if (ms_get_block(e, x, y, z) == g_msb.planks)
        ms_fill_pillar_down_or_chain_up(e, HC_B_OAK_LOG_Y, x, y, z);
    if (ms_get_block(e, x + 2, y, z) == g_msb.planks)
        ms_fill_pillar_down_or_chain_up(e, HC_B_OAK_LOG_Y, x + 2, y, z);
}

/* hasSturdyNeighbours (:611-627): Direction.values (D,U,N,S,W,E) 순으로
 * 이웃이 chunkBB 안 && 반대면 sturdy 인 개수 ≥ count. 드로우 0. */
static int ms_has_sturdy_neighbours(ms_env_t *e, int32_t x, int32_t y,
                                    int32_t z, int count) {
    int32_t wx = ms_world_x(e, x, z), wy = ms_world_y(e, y),
            wz = ms_world_z(e, x, z);
    int n = 0;
    for (int d = 0; d < 6; d++) {
        int32_t nx = wx + MS_DIR_OFF[d][0];
        int32_t ny = wy + MS_DIR_OFF[d][1];
        int32_t nz = wz + MS_DIR_OFF[d][2];
        if (ms_inside(e, nx, ny, nz) &&
            hc_featx_face_sturdy_full(hc_feat_get_block(e->rg, nx, ny, nz),
                                      MS_DIR_OPP[d]))
            if (++n >= count)
                return 1;
    }
    return 0;
}

/* maybePlaceCobWeb (:603-609): isInterior 가 먼저 — false 면 드로우 0;
 * true 면 draw nextFloat < p, 통과 시 hasSturdyNeighbours(≥2, 드로우 0)
 * 성립하면 placeBlock(COBWEB). */
static void ms_maybe_place_cobweb(ms_env_t *e, float prob, int32_t x,
                                  int32_t y, int32_t z) {
    if (!ms_is_interior(e, x, y, z))
        return;
    if (!(hc_wgr_next_float(e->rng) < prob))
        return;
    if (!ms_has_sturdy_neighbours(e, x, y, z, 2))
        return;
    ms_place_block(e, g_msb.cobweb, x, y, z);
}

/* placeSupport (:566-601). 인자 순서는 자바 호출부 그대로
 * (x0, y0, z, y1, x1). isSupportingBox 실패 → 드로우 0 스킵. */
static void ms_place_support(ms_env_t *e, int32_t x0, int32_t y0, int32_t z,
                             int32_t y1, int32_t x1) {
    if (!ms_is_supporting_box(e, x0, x1, y1, z))
        return;
    /* 양쪽 fence 기둥 (드로우 0) — 상태 변환은 placeBlock 몫 (W/E
     * 코리도에서 fence[west/east=true] 가 north/south 로 회전) */
    ms_generate_box(e, x0, y0, z, x0, y1 - 1, z, g_msb.fence_w,
                    HC_B_CAVE_AIR, 0);
    ms_generate_box(e, x1, y0, z, x1, y1 - 1, z, g_msb.fence_e,
                    HC_B_CAVE_AIR, 0);
    if (hc_wgr_next_int(e->rng, 4) == 0) {
        /* 상단 판자 양끝만 (드로우 0 ×2 — 총 1드로우 경로) */
        ms_generate_box(e, x0, y1, z, x0, y1, z, g_msb.planks, HC_B_CAVE_AIR,
                        0);
        ms_generate_box(e, x1, y1, z, x1, y1, z, g_msb.planks, HC_B_CAVE_AIR,
                        0);
    } else {
        /* 상단 판자 전체 + 벽토치 2회 시도 (draw nextFloat ×2 — 총
         * 3드로우 경로). FACING 은 배치 시 회전/미러 적용. */
        ms_generate_box(e, x0, y1, z, x1, y1, z, g_msb.planks, HC_B_CAVE_AIR,
                        0);
        ms_maybe_generate_block(e, 0.05f, x0 + 1, y1, z - 1,
                                g_msb.wall_torch_s);
        ms_maybe_generate_block(e, 0.05f, x0 + 1, y1, z + 1,
                                g_msb.wall_torch_n);
    }
}

/* Corridor.createChest 오버라이드 (:370-396) — 마인카트 체스트.
 * 게이트 (chunkBB 안 && 그 칸 air && 아래 칸 비-air; level 직접 읽기)
 * 실패 시 드로우 0. 성공 시 draw nextBoolean (레일 방향; E/W 코리도는
 * placeBlock 의 CW90 로 shape 회전) → placeBlock(RAIL) →
 * CHEST_MINECART 엔티티 생성 (엔티티 자체 RNG 는 월드젠과 무관) 후
 * setLootTable(key, random.nextLong()) [draw nextLong = 2 next].
 * 엔티티는 리전 mca 밖 (entities/) — BE 기록 없음. */
static int ms_create_chest(ms_env_t *e, int32_t x, int32_t y, int32_t z) {
    int32_t wx = ms_world_x(e, x, z), wy = ms_world_y(e, y),
            wz = ms_world_z(e, x, z);
    if (!ms_inside(e, wx, wy, wz) ||
        !hc_block_is_air(hc_feat_get_block(e->rg, wx, wy, wz)) ||
        hc_block_is_air(hc_feat_get_block(e->rg, wx, wy - 1, wz)))
        return 0;
    uint16_t rail = ms_next_bool(e) ? g_msb.rail_ns : g_msb.rail_ew;
    ms_place_block(e, rail, x, y, z);
    (void)hc_wgr_next_long(e->rng); /* minecart_chest 루트 시드 소모 */
    return 1;
}

/* --- MineShaftRoom.postProcess (:1248-1301) — 드로우 0, 절대좌표 --- */
static void ms_post_room(ms_env_t *e) {
    if (ms_is_in_invalid_location(e))
        return; /* §A.4 공통 — 드로우 0 */
    const int32_t *b = e->p->bb;
    /* [1] 본체 하부 에어: (minX,minY+1,minZ)-(maxX, min(minY+3,maxY),
     * maxZ) CAVE_AIR */
    ms_generate_box(e, b[0], b[1] + 1, b[2], b[3], imin32(b[1] + 3, b[4]),
                    b[5], HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
    /* [2] childEntranceBoxes 각각 (x0, maxY-2, z0)-(x1, maxY, z1) */
    for (int32_t i = 0; i < e->p->n_entrances; i++) {
        const int32_t *eb = e->p->entrances[i];
        ms_generate_box(e, eb[0], eb[4] - 2, eb[2], eb[3], eb[4], eb[5],
                        HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
    }
    /* [3] 상부 반구 (skipAir=false) */
    ms_generate_upper_half_sphere(e, b[0], b[1] + 4, b[2], b[3], b[4], b[5],
                                  HC_B_CAVE_AIR, 0);
}

/* --- MineShaftCorridor.postProcess (:398-478) — §A.4 드로우 시퀀스 --- */
static void ms_post_corridor(ms_env_t *e) {
    hc_spiece_t *p = e->p;
    if (ms_is_in_invalid_location(e))
        return; /* §A.4 공통 — 드로우 0 */
    int32_t len = p->num_sections * 5 - 1;
    /* [1] 통로 에어 (0,0,0)-(2,1,len) — 드로우 0 */
    ms_generate_box(e, 0, 0, 0, 2, 1, len, HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
    /* [2] 천장 y=2 확률 에어 0.8F — draw nextFloat ×3×(len+1) */
    ms_generate_maybe_box(e, 0.8f, 0, 2, 0, 2, 2, len, HC_B_CAVE_AIR,
                          HC_B_CAVE_AIR, 0, 0);
    /* [3] spiderCorridor 거미줄 0.6F, hasToBeInside — draw ×6×(len+1)
     * (isInterior 는 드로우 뒤 검사) */
    if (p->spider)
        ms_generate_maybe_box(e, 0.6f, 0, 0, 0, 2, 1, len, g_msb.cobweb,
                              HC_B_CAVE_AIR, 0, 1);
    /* [4] 섹션 루프 z = 2+5s */
    for (int32_t s = 0; s < p->num_sections; s++) {
        int32_t z = 2 + s * 5;
        /* [4a] placeSupport(0,0,z,2,2) */
        ms_place_support(e, 0, 0, z, 2, 2);
        /* [4b] maybePlaceCobWeb ×8 — 순서 고정 */
        ms_maybe_place_cobweb(e, 0.1f, 0, 2, z - 1);
        ms_maybe_place_cobweb(e, 0.1f, 2, 2, z - 1);
        ms_maybe_place_cobweb(e, 0.1f, 0, 2, z + 1);
        ms_maybe_place_cobweb(e, 0.1f, 2, 2, z + 1);
        ms_maybe_place_cobweb(e, 0.05f, 0, 2, z - 2);
        ms_maybe_place_cobweb(e, 0.05f, 2, 2, z - 2);
        ms_maybe_place_cobweb(e, 0.05f, 0, 2, z + 2);
        ms_maybe_place_cobweb(e, 0.05f, 2, 2, z + 2);
        /* [4c] draw nextInt(100)==0 → 마인카트 체스트 (2,0,z-1) */
        if (hc_wgr_next_int(e->rng, 100) == 0)
            ms_create_chest(e, 2, 0, z - 1);
        /* [4d] draw nextInt(100)==0 → 마인카트 체스트 (0,0,z+1) */
        if (hc_wgr_next_int(e->rng, 100) == 0)
            ms_create_chest(e, 0, 0, z + 1);
        /* [4e] 스포너 — 래치 실패해도 nextInt(3) 은 소모, 래치는 유지 →
         * 다음 섹션/다음 청크에서 재시도 (spider_placed 는 피스 구조체
         * 필드 — 청크 간 공유). */
        if (p->spider && !p->spider_placed) {
            int32_t nz = z - 1 + hc_wgr_next_int(e->rng, 3);
            int32_t wx = ms_world_x(e, 1, nz), wy = ms_world_y(e, 0),
                    wz = ms_world_z(e, 1, nz);
            if (ms_inside(e, wx, wy, wz) && ms_is_interior(e, 1, 0, nz)) {
                p->spider_placed = 1;
                /* level.setBlock 직접 (placeBlock 아님 — canBeReplaced/
                 * 유체틱/마킹 없음) + SpawnerBlockEntity.setEntityId
                 * (CAVE_SPIDER, r) — 드로우 0 (빈 WeightedList,
                 * BaseSpawner.java:64-66/349-356). */
                hc_feat_set_block(e->rg, wx, wy, wz, HC_B_SPAWNER);
                hc_be_rec_t *rec = hc_be_record(&e->sc->be, wx, wy, wz,
                                                HC_BE_SPAWNER, HC_B_SPAWNER);
                if (!rec)
                    die("mineshaft: BE recorder full", NULL);
                rec->entity = "minecraft:cave_spider";
            }
        }
    }
    /* [5] 바닥 판자 x 외측 0..2 × z 0..len — 드로우 0 */
    for (int32_t x = 0; x <= 2; x++)
        for (int32_t z = 0; z <= len; z++)
            ms_set_planks_block(e, g_msb.planks, x, -1, z);
    /* [6] 지지 기둥/체인 (0,-1,2), numSections>1 이면 (0,-1,len-2) —
     * 드로우 0 */
    ms_place_double_support(e, 0, -1, 2);
    if (p->num_sections > 1)
        ms_place_double_support(e, 0, -1, len - 2);
    /* [7] hasRails: 바닥 (1,-1,z) 이 !isAir && isSolidRender 일 때만
     * p = isInterior ? 0.7F : 0.9F 로 draw nextFloat — 미충족 z 는
     * 드로우 없음. isSolidRender ≈ hc_block_is_full_cube (features.c
     * 관행). E/W 코리도는 placeBlock 의 CW90 로 shape 회전. */
    if (p->has_rails) {
        for (int32_t z = 0; z <= len; z++) {
            uint16_t fl = ms_get_block(e, 1, -1, z);
            if (!hc_block_is_air(fl) && hc_block_is_full_cube(fl)) {
                float prob = ms_is_interior(e, 1, 0, z) ? 0.7f : 0.9f;
                ms_maybe_generate_block(e, prob, 1, 0, z, g_msb.rail_ns);
            }
        }
    }
    /* fillColumnDown (Corridor 오버라이드 :492-510) 은 26.2 마인샤프트
     * postProcess 경로에 호출부가 없다 (디컴파일 전수) — 도달 불가,
     * 미구현. */
}

/* placeSupportPillar (:991-995): (x, y1+1, z) 가 air 아니면 판자 기둥 */
static void ms_place_support_pillar(ms_env_t *e, int32_t x, int32_t y0,
                                    int32_t z, int32_t y1) {
    if (!hc_block_is_air(ms_get_block(e, x, y1 + 1, z)))
        ms_generate_box(e, x, y0, z, x, y1, z, g_msb.planks, HC_B_CAVE_AIR,
                        0);
}

/* --- MineShaftCrossing.postProcess (:871-989) — 드로우 0, 절대좌표 --- */
static void ms_post_crossing(ms_env_t *e) {
    if (ms_is_in_invalid_location(e))
        return; /* §A.4 공통 — 드로우 0 */
    const int32_t *b = e->p->bb;
    if (e->p->is_two_floored) {
        /* 십자 에어박스 5개: 하층 2 + 상층 2 + 중간층 연결 1 */
        ms_generate_box(e, b[0] + 1, b[1], b[2], b[3] - 1, b[1] + 3 - 1,
                        b[5], HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
        ms_generate_box(e, b[0], b[1], b[2] + 1, b[3], b[1] + 3 - 1,
                        b[5] - 1, HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
        ms_generate_box(e, b[0] + 1, b[4] - 2, b[2], b[3] - 1, b[4], b[5],
                        HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
        ms_generate_box(e, b[0], b[4] - 2, b[2] + 1, b[3], b[4], b[5] - 1,
                        HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
        ms_generate_box(e, b[0] + 1, b[1] + 3, b[2] + 1, b[3] - 1, b[1] + 3,
                        b[5] - 1, HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
    } else {
        ms_generate_box(e, b[0] + 1, b[1], b[2], b[3] - 1, b[4], b[5],
                        HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
        ms_generate_box(e, b[0], b[1], b[2] + 1, b[3], b[4], b[5] - 1,
                        HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
    }
    /* placeSupportPillar ×4 (:977-980) */
    ms_place_support_pillar(e, b[0] + 1, b[1], b[2] + 1, b[4]);
    ms_place_support_pillar(e, b[0] + 1, b[1], b[5] - 1, b[4]);
    ms_place_support_pillar(e, b[3] - 1, b[1], b[2] + 1, b[4]);
    ms_place_support_pillar(e, b[3] - 1, b[1], b[5] - 1, b[4]);
    /* 바닥: 전 평면 y = minY-1 setPlanksBlock (:981-987) */
    for (int32_t x = b[0]; x <= b[3]; x++)
        for (int32_t z = b[2]; z <= b[5]; z++)
            ms_set_planks_block(e, g_msb.planks, x, b[1] - 1, z);
}

/* --- MineShaftStairs.postProcess (:1409-1426) — 드로우 0, 로컬좌표 --- */
static void ms_post_stairs(ms_env_t *e) {
    if (ms_is_in_invalid_location(e))
        return; /* §A.4 공통 — 드로우 0 */
    ms_generate_box(e, 0, 5, 0, 2, 7, 1, HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
    ms_generate_box(e, 0, 0, 7, 2, 2, 8, HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
    for (int32_t i = 0; i < 5; i++)
        ms_generate_box(e, 0, 5 - i - (i < 4 ? 1 : 0), 2 + i, 2, 7 - i,
                        2 + i, HC_B_CAVE_AIR, HC_B_CAVE_AIR, 0);
}

void hc_splace_mineshaft(hc_sctx_t *sc, hc_feat_region_t *rg,
                         hc_sstart_t *start, hc_spiece_t *p, hc_wgr_t *rng,
                         int32_t cx, int32_t cz) {
    (void)start; /* shipwreck 높이 래치용 — mineshaft 미사용 */
    ms_blocks_init();
    ms_env_t e;
    e.sc = sc;
    e.rg = rg;
    e.p = p;
    e.rng = rng;
    /* chunkBB = getWritableArea (ChunkGenerator.java:440-448):
     * (16cx, dimMinY+1, 16cz) ~ (16cx+15, dimMaxY, 16cz+15) */
    bb_set6(e.cb, cx * 16, HC_MIN_Y + 1, cz * 16, cx * 16 + 15, HC_MAX_Y,
            cz * 16 + 15);
    /* setOrientation → mirror/rotation (StructurePiece.java:564-588).
     * o=-1 (Room/Crossing) 은 자바 null 필드 — 항등 처리 (§A.3). */
    switch (p->o) {
    case MS_DIR_S:
        e.mir = HC_MIR_LEFT_RIGHT;
        e.rot = HC_ROT_NONE;
        break;
    case MS_DIR_W:
        e.mir = HC_MIR_LEFT_RIGHT;
        e.rot = HC_ROT_CW90;
        break;
    case MS_DIR_E:
        e.mir = HC_MIR_NONE;
        e.rot = HC_ROT_CW90;
        break;
    default: /* NORTH 또는 orientation null */
        e.mir = HC_MIR_NONE;
        e.rot = HC_ROT_NONE;
        break;
    }
    switch (p->kind) {
    case HC_SP_MS_ROOM: ms_post_room(&e); break;
    case HC_SP_MS_CORRIDOR: ms_post_corridor(&e); break;
    case HC_SP_MS_CROSSING: ms_post_crossing(&e); break;
    case HC_SP_MS_STAIRS: ms_post_stairs(&e); break;
    default: die("hc_splace_mineshaft: non-mineshaft piece", NULL);
    }
}
