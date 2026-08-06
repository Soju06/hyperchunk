#include "hc_chunk_nbt.h"

#include <assert.h>
#include <string.h>

#include "hc_blocks.h"
#include "hc_nbt.h"
#include "hc_structures.h" /* Task 14: starts/References/BE 소비 */
#include "features_internal.h" /* hc_featx_die */

/* Mth.ceillog2 (n >= 1) */
static int ceil_log2(int32_t n) {
    int b = 0;
    while ((1 << b) < n)
        b++;
    return b;
}

/* SimpleBitStorage 패킹: LSB-first, 64/bits 개/long, 무스팬 (R-E §3).
 * 반환: long 수. */
static int32_t pack_bits(const uint16_t *vals, int32_t n, int bits,
                         int64_t *out) {
    int32_t vpl = 64 / bits;
    int32_t nlongs = (n + vpl - 1) / vpl;
    for (int32_t i = 0; i < nlongs; i++)
        out[i] = 0;
    for (int32_t i = 0; i < n; i++)
        out[i / vpl] |= (int64_t)((uint64_t)vals[i] << ((i % vpl) * bits));
    return nlongs;
}

/* 블록 상태 팔레트 엔트리: NAMES 문자열 파싱 → {Name[,Properties]}.
 * Properties 삽입은 이름 내림차순 (R-E §4 PairMapCodec 접힘) — 문자열이
 * 이미 오름차순이라 역순 스캔. */
static hc_nbt_t *palette_entry(hc_arena_t *a, uint16_t state) {
    const char *nm = hc_block_name(state);
    const char *br = strchr(nm, '[');
    hc_nbt_t   *ent = hc_nbt_compound(a);
    if (!ent)
        return NULL;
    if (!br) {
        if (hc_nbt_put(a, ent, "Name", hc_nbt_string(a, nm)) != 0)
            return NULL;
        return ent;
    }
    if (hc_nbt_put(a, ent, "Name",
                   hc_nbt_string_n(a, nm, (size_t)(br - nm))) != 0)
        return NULL;
    /* k=v 쌍 스팬 수집 (오름차순) */
    struct {
        const char *k, *v;
        size_t      kn, vn;
    } kv[16];
    int         nkv = 0;
    const char *p = br + 1;
    while (*p != ']') {
        const char *eq = strchr(p, '=');
        const char *end = eq + 1;
        while (*end != ',' && *end != ']')
            end++;
        assert(nkv < 16);
        kv[nkv].k = p;
        kv[nkv].kn = (size_t)(eq - p);
        kv[nkv].v = eq + 1;
        kv[nkv].vn = (size_t)(end - eq - 1);
        nkv++;
        p = *end == ',' ? end + 1 : end;
    }
    hc_nbt_t *props = hc_nbt_compound(a);
    if (!props)
        return NULL;
    for (int i = nkv - 1; i >= 0; i--) {
        char *key = hc_arena_alloc(a, kv[i].kn + 1, 1);
        if (!key)
            return NULL;
        memcpy(key, kv[i].k, kv[i].kn);
        key[kv[i].kn] = '\0';
        if (hc_nbt_put(a, props, key,
                       hc_nbt_string_n(a, kv[i].v, kv[i].vn)) != 0)
            return NULL;
    }
    if (hc_nbt_put(a, ent, "Properties", props) != 0)
        return NULL;
    return ent;
}

/* PalettedContainer.pack 등가 — 첫-등장 스캔 재팩 (R-E §2). n==4096 블록
 * (min 4bit) / n==64 바이옴 (무클램프). 실패 NULL. */
static hc_nbt_t *packed_container(hc_arena_t *a, const uint16_t *cells,
                                  int32_t n, int is_blocks,
                                  const hc_biome_reg_t *biomes) {
    /* 첫-등장 팔레트 */
    static const uint16_t NOPAL = 0xFFFF;
    uint16_t map[(int)HC_B_COUNT > (int)HC_BIOME_MAX ? (int)HC_B_COUNT
                                                     : (int)HC_BIOME_MAX];
    memset(map, 0xFF, sizeof map);
    uint16_t pal[512];
    uint16_t idx_static[4096];
    int32_t  npal = 0;
    for (int32_t i = 0; i < n; i++) {
        uint16_t v = cells[i];
        if (map[v] == NOPAL) {
            assert(npal < 512);
            map[v] = (uint16_t)npal;
            pal[npal++] = v;
        }
        idx_static[i] = map[v];
    }
    hc_nbt_t *cont = hc_nbt_compound(a);
    hc_nbt_t *plist = hc_nbt_list(a);
    if (!cont || !plist)
        return NULL;
    for (int32_t i = 0; i < npal; i++) {
        hc_nbt_t *ent = is_blocks
                            ? palette_entry(a, pal[i])
                            : hc_nbt_string(a, biomes->names[pal[i]]);
        if (hc_nbt_add(a, plist, ent) != 0)
            return NULL;
    }
    /* 삽입 순서: palette, data (코덱 레코드 순 — R-E §1) */
    if (hc_nbt_put(a, cont, "palette", plist) != 0)
        return NULL;
    int bits = ceil_log2(npal);
    if (is_blocks && bits != 0 && bits < 4)
        bits = 4;
    if (bits != 0) {
        int32_t  vpl = 64 / bits;
        int32_t  nlongs = (n + vpl - 1) / vpl;
        int64_t *longs = hc_arena_alloc(a, sizeof(int64_t) * (size_t)nlongs,
                                        _Alignof(int64_t));
        if (!longs)
            return NULL;
        pack_bits(idx_static, n, bits, longs);
        if (hc_nbt_put(a, cont, "data",
                       hc_nbt_long_array(a, longs, nlongs)) != 0)
            return NULL;
    }
    return cont;
}

/* 라이트 레이어 방출 (R-E §7f + Task 14):
 *  - sky: 등록 && 최종값>0 존재 (등록 시 생성 — createDataLayer).
 *  - block: 실체화 = >0 쓰기 이력 (blk_written) — 바닐라는 쓰기 시
 *    getDataLayer(create) 로 생성하고, 감쇠로 all-0 이 돼도 저장에
 *    남긴다 (실측 c.4.10: 버섯 배치-발광 후 엣지-사멸의 all-0 레이어). */
static hc_nbt_t *light_layer(hc_arena_t *a, const hc_light_chunk_t *ls,
                             int layer, int32_t sy) {
    if (!((ls->registered >> (sy - HC_LIGHT_SEC_MIN)) & 1u))
        return NULL;
    const uint8_t *cell = ls->light[layer] + (sy - HC_LIGHT_SEC_MIN) * 4096;
    if (layer == HC_LIGHT_BLOCK) {
        if (!((ls->blk_written >> (sy - HC_LIGHT_SEC_MIN)) & 1u))
            return NULL;
    } else {
        int nonzero = 0;
        for (int i = 0; i < 4096; i++)
            if (cell[i]) {
                nonzero = 1;
                break;
            }
        if (!nonzero)
            return NULL;
    }
    uint8_t *nib = hc_arena_alloc(a, 2048, 1);
    if (!nib)
        return NULL;
    for (int i = 0; i < 2048; i++)
        nib[i] = (uint8_t)((cell[2 * i] & 15) | (cell[2 * i + 1] & 15) << 4);
    return hc_nbt_byte_array(a, nib, 2048);
}

static const char *const HM_KEYS[HC_HMF_COUNT] = {
    "OCEAN_FLOOR",
    "WORLD_SURFACE",
    "MOTION_BLOCKING",
    "MOTION_BLOCKING_NO_LEAVES",
};
/* EnumMap 순회 = 서수 순: WS(1), OF(3), MB(4), MBNL(5) → HC_HMF 인덱스 */
static const int HM_ORDINAL[HC_HMF_COUNT] = {
    HC_HMF_WORLD_SURFACE,
    HC_HMF_OCEAN_FLOOR,
    HC_HMF_MOTION_BLOCKING,
    HC_HMF_MOTION_BLOCKING_NO_LEAVES,
};

static const char *tick_fluid_name(int kind) {
    switch (kind) {
    case HC_TICK_WATER:
        return "minecraft:water";
    case HC_TICK_FLOWING_WATER:
        return "minecraft:flowing_water";
    case HC_TICK_LAVA:
        return "minecraft:lava";
    default:
        return "minecraft:flowing_lava";
    }
}

/* SavedTick 코덱 삽입 순서 i,x,y,z,t,p (R-D §6) */
static hc_nbt_t *tick_entry(hc_arena_t *a, const hc_tick_rec_t *r) {
    hc_nbt_t *ent = hc_nbt_compound(a);
    if (!ent)
        return NULL;
    hc_nbt_t *id;
    if (r->kind == HC_TICK_BLOCK) {
        const char *nm = hc_block_name(r->block);
        const char *br = strchr(nm, '[');
        id = br ? hc_nbt_string_n(a, nm, (size_t)(br - nm))
                : hc_nbt_string(a, nm);
    } else {
        id = hc_nbt_string(a, tick_fluid_name(r->kind));
    }
    if (hc_nbt_put(a, ent, "i", id) != 0 ||
        hc_nbt_put(a, ent, "x", hc_nbt_int(a, r->x)) != 0 ||
        hc_nbt_put(a, ent, "y", hc_nbt_int(a, r->y)) != 0 ||
        hc_nbt_put(a, ent, "z", hc_nbt_int(a, r->z)) != 0 ||
        hc_nbt_put(a, ent, "t", hc_nbt_int(a, r->t)) != 0 ||
        hc_nbt_put(a, ent, "p", hc_nbt_int(a, 0)) != 0)
        return NULL;
    return ent;
}

/* ================= Task 14: 블록엔티티 방출 =================
 *
 * saveWithFullMetadata 재구성 (R-serialization §4.2/4.3): [saveAdditional
 * 필드…] → components{} (항상) → id → x → y → z → keepPacked(0).
 * 컨테이너: LootTable(+LootTableSeed if != 0) 또는 Items(빈 리스트 포함
 * 항상); hopper 는 그 뒤 TransferCooldown. 값 출처 = 템플릿 nbt ∪ 주입/
 * 설정 시드 (hc_be_rec_t). */

static const char *be_id_of_state(uint16_t state) {
    const char *nm = hc_block_name(state);
    const char *br = strchr(nm, '[');
    size_t      n = br ? (size_t)(br - nm) : strlen(nm);
#define IS(base) (n == strlen(base) && strncmp(nm, base, n) == 0)
    if (IS("minecraft:chest"))
        return "minecraft:chest";
    if (IS("minecraft:barrel"))
        return "minecraft:barrel";
    if (IS("minecraft:dispenser"))
        return "minecraft:dispenser";
    if (IS("minecraft:hopper"))
        return "minecraft:hopper";
    if (IS("minecraft:decorated_pot"))
        return "minecraft:decorated_pot";
    if (IS("minecraft:suspicious_sand") || IS("minecraft:suspicious_gravel"))
        return "minecraft:brushable_block";
    if (IS("minecraft:spawner"))
        return "minecraft:mob_spawner";
    if (IS("minecraft:trial_spawner"))
        return "minecraft:trial_spawner";
    if (IS("minecraft:vault"))
        return "minecraft:vault";
#undef IS
    hc_featx_die("block entity id unmapped for state", nm);
    return NULL;
}

/* 컨테이너 루트/아이템 파트: [LootTable(+seed) | Items] put */
static int be_put_container(hc_arena_t *a, hc_nbt_t *t,
                            const hc_be_rec_t *r) {
    const char     *loot = r->loot;
    const hc_nbt_t *tpl_items = NULL;
    if (r->tpl_nbt) {
        const hc_nbt_t *lt = hc_nbt_get(r->tpl_nbt, "LootTable");
        if (lt)
            loot = hc_nbt_str(lt);
        tpl_items = hc_nbt_get(r->tpl_nbt, "Items");
    }
    if (loot) {
        if (hc_nbt_put(a, t, "LootTable", hc_nbt_string(a, loot)) != 0)
            return -1;
        if (r->loot_seed != 0 &&
            hc_nbt_put(a, t, "LootTableSeed",
                       hc_nbt_long(a, r->loot_seed)) != 0)
            return -1;
        return 0;
    }
    /* no-loot: Items 항상 (빈 리스트 포함). 템플릿 Items 는 이 리전에서
     * 전부 빈 리스트 (아이템 보유 컨테이너는 전부 LootTable) — 비면
     * 그대로, 아니면 die (아이템 재직렬화 미구현). */
    if (tpl_items && hc_nbt_list_count(tpl_items) != 0)
        hc_featx_die("container with literal Items unimplemented", NULL);
    return hc_nbt_put(a, t, "Items", hc_nbt_list(a));
}

static hc_nbt_t *be_to_nbt(hc_arena_t *a, const hc_be_rec_t *r) {
    hc_nbt_t *t = hc_nbt_compound(a);
    if (!t)
        return NULL;
    const char *id = be_id_of_state(r->state);
    int         bad = 0;
    if (strcmp(id, "minecraft:chest") == 0 ||
        strcmp(id, "minecraft:barrel") == 0 ||
        strcmp(id, "minecraft:dispenser") == 0) {
        bad |= be_put_container(a, t, r);
    } else if (strcmp(id, "minecraft:hopper") == 0) {
        bad |= be_put_container(a, t, r);
        int32_t cd = -1;
        if (r->tpl_nbt) {
            const hc_nbt_t *v = hc_nbt_get(r->tpl_nbt, "TransferCooldown");
            if (v)
                cd = (int32_t)hc_nbt_i64(v);
        }
        bad |= hc_nbt_put(a, t, "TransferCooldown", hc_nbt_int(a, cd));
    } else if (strcmp(id, "minecraft:decorated_pot") == 0) {
        /* sherds (decorations != EMPTY 일 때만 — 템플릿 nbt 그대로) */
        const hc_nbt_t *sh =
            r->tpl_nbt ? hc_nbt_get(r->tpl_nbt, "sherds") : NULL;
        if (sh)
            bad |= hc_nbt_put(a, t, "sherds", (hc_nbt_t *)sh);
        bad |= be_put_container(a, t, r);
    } else if (strcmp(id, "minecraft:brushable_block") == 0) {
        if (r->loot) {
            bad |= hc_nbt_put(a, t, "LootTable", hc_nbt_string(a, r->loot));
            if (r->loot_seed != 0)
                bad |= hc_nbt_put(a, t, "LootTableSeed",
                                  hc_nbt_long(a, r->loot_seed));
        }
    } else if (strcmp(id, "minecraft:mob_spawner") == 0) {
        bad |= hc_nbt_put(a, t, "Delay", hc_nbt_short(a, 20));
        bad |= hc_nbt_put(a, t, "MinSpawnDelay", hc_nbt_short(a, 200));
        bad |= hc_nbt_put(a, t, "MaxSpawnDelay", hc_nbt_short(a, 800));
        bad |= hc_nbt_put(a, t, "SpawnCount", hc_nbt_short(a, 4));
        bad |= hc_nbt_put(a, t, "MaxNearbyEntities", hc_nbt_short(a, 6));
        bad |= hc_nbt_put(a, t, "RequiredPlayerRange", hc_nbt_short(a, 16));
        bad |= hc_nbt_put(a, t, "SpawnRange", hc_nbt_short(a, 4));
        hc_nbt_t *sd = hc_nbt_compound(a);
        hc_nbt_t *ent = hc_nbt_compound(a);
        if (!sd || !ent)
            return NULL;
        bad |= hc_nbt_put(a, ent, "id", hc_nbt_string(a, r->entity));
        bad |= hc_nbt_put(a, sd, "entity", ent);
        bad |= hc_nbt_put(a, t, "SpawnData", sd);
        bad |= hc_nbt_put(a, t, "SpawnPotentials", hc_nbt_list(a));
    } else if (strcmp(id, "minecraft:trial_spawner") == 0) {
        const hc_nbt_t *nc =
            r->tpl_nbt ? hc_nbt_get(r->tpl_nbt, "normal_config") : NULL;
        const hc_nbt_t *oc =
            r->tpl_nbt ? hc_nbt_get(r->tpl_nbt, "ominous_config") : NULL;
        if (nc)
            bad |= hc_nbt_put(a, t, "normal_config", (hc_nbt_t *)nc);
        if (oc)
            bad |= hc_nbt_put(a, t, "ominous_config", (hc_nbt_t *)oc);
    } else if (strcmp(id, "minecraft:vault") == 0) {
        /* VaultConfig.CODEC: loot_table 은 optionalFieldOf 기본값
         * (chests/trial_chambers/reward) 과 같으면 인코드 생략 —
         * ResourceKey 는 값-동등. key_item 기본값 비교는 ItemStack
         * 참조-동등이라 절대 생략 안 됨 (골든 실측: key_item 유지,
         * 기본 loot_table 탈락). 템플릿 config 를 필터해 재방출한다. */
        const hc_nbt_t *cfg =
            r->tpl_nbt ? hc_nbt_get(r->tpl_nbt, "config") : NULL;
        hc_nbt_t *fc = hc_nbt_compound(a);
        if (!fc)
            return NULL;
        if (cfg) {
            int32_t nk = hc_nbt_comp_count(cfg);
            for (int32_t ki = 0; ki < nk; ki++) {
                const char     *k = NULL;
                const hc_nbt_t *v = hc_nbt_comp_at(cfg, ki, &k);
                if (strcmp(k, "loot_table") == 0 &&
                    hc_nbt_tag(v) == HC_NBT_STRING &&
                    strcmp(hc_nbt_str(v),
                           "minecraft:chests/trial_chambers/reward") == 0)
                    continue;
                bad |= hc_nbt_put(a, fc, k, (hc_nbt_t *)v);
            }
        }
        bad |= hc_nbt_put(a, t, "config", fc);
        bad |= hc_nbt_put(a, t, "shared_data", hc_nbt_compound(a));
        bad |= hc_nbt_put(a, t, "server_data", hc_nbt_compound(a));
    } else {
        hc_featx_die("block entity emitter missing", id);
    }
    /* 공통 꼬리: components{} → id → x/y/z → keepPacked(false) */
    bad |= hc_nbt_put(a, t, "components", hc_nbt_compound(a));
    bad |= hc_nbt_put(a, t, "id", hc_nbt_string(a, id));
    bad |= hc_nbt_put(a, t, "x", hc_nbt_int(a, r->x));
    bad |= hc_nbt_put(a, t, "y", hc_nbt_int(a, r->y));
    bad |= hc_nbt_put(a, t, "z", hc_nbt_int(a, r->z));
    bad |= hc_nbt_put(a, t, "keepPacked", hc_nbt_byte(a, 0));
    return bad ? NULL : t;
}

ptrdiff_t hc_chunk_to_nbt(const hc_chunk_t *c, const hc_biome_reg_t *biomes,
                          const hc_light_chunk_t *ls,
                          const hc_tick_rec_t *ticks, int32_t n_ticks,
                          int64_t last_update, const struct hc_sctx *sctx,
                          hc_arena_t *scratch, uint8_t *out, size_t cap) {
    hc_arena_t *a = scratch;
    hc_nbt_t   *root = hc_nbt_compound(a);
    if (!root)
        return -1;

    /* 방출 시퀀스 = SerializableChunkData.write put 순서 (R-C §2).
     * 바이트 순서는 hc_nbt 의 HashMap 에뮬레이션이 결정한다. */
    int bad = 0;
    bad |= hc_nbt_put(a, root, "DataVersion", hc_nbt_int(a, 4903));
    bad |= hc_nbt_put(a, root, "xPos", hc_nbt_int(a, c->cx));
    bad |= hc_nbt_put(a, root, "yPos", hc_nbt_int(a, HC_MIN_Y >> 4));
    bad |= hc_nbt_put(a, root, "zPos", hc_nbt_int(a, c->cz));
    bad |= hc_nbt_put(a, root, "LastUpdate", hc_nbt_long(a, last_update));
    bad |= hc_nbt_put(a, root, "InhabitedTime", hc_nbt_long(a, 0));
    bad |= hc_nbt_put(a, root, "Status",
                      hc_nbt_string(a, "minecraft:full"));
    if (bad)
        return -1;

    /* sections: 라이트 섹션 범위 순회 (R-C §3, R-E §7a) */
    hc_nbt_t *sections = hc_nbt_list(a);
    if (!sections)
        return -1;
    for (int32_t sy = HC_LIGHT_SEC_MIN; sy <= HC_LIGHT_SEC_MAX; sy++) {
        int       in_range = sy >= (HC_MIN_Y >> 4) && sy <= (HC_MAX_Y >> 4);
        hc_nbt_t *sec = hc_nbt_compound(a);
        if (!sec)
            return -1;
        int any = 0;
        if (in_range) {
            const uint16_t *cells =
                c->states + (size_t)(sy - (HC_MIN_Y >> 4)) * 4096;
            hc_nbt_t *bs = packed_container(a, cells, 4096, 1, NULL);
            if (!bs || hc_nbt_put(a, sec, "block_states", bs) != 0)
                return -1;
            const uint16_t *quarts =
                c->biomes + (size_t)(sy - (HC_MIN_Y >> 4)) * 64;
            hc_nbt_t *bio = packed_container(a, quarts, 64, 0, biomes);
            if (!bio || hc_nbt_put(a, sec, "biomes", bio) != 0)
                return -1;
            any = 1;
        }
        /* 삽입 순서 BlockLight 먼저, SkyLight 다음 (write @247/@271) */
        hc_nbt_t *bl = light_layer(a, ls, HC_LIGHT_BLOCK, sy);
        if (bl) {
            if (hc_nbt_put(a, sec, "BlockLight", bl) != 0)
                return -1;
            any = 1;
        }
        hc_nbt_t *sl = light_layer(a, ls, HC_LIGHT_SKY, sy);
        if (sl) {
            if (hc_nbt_put(a, sec, "SkyLight", sl) != 0)
                return -1;
            any = 1;
        }
        if (!any)
            continue;
        if (hc_nbt_put(a, sec, "Y", hc_nbt_byte(a, sy)) != 0 ||
            hc_nbt_add(a, sections, sec) != 0)
            return -1;
    }
    if (hc_nbt_put(a, root, "sections", sections) != 0)
        return -1;

    if (ls->enabled &&
        hc_nbt_put(a, root, "isLightOn", hc_nbt_byte(a, 1)) != 0)
        return -1;
    /* block_entities (Task 14 — 순서는 hc_structures_chunk_bes 가
     * HashSet<BlockPos>/fastutil 순회 에뮬로 재구성) */
    {
        hc_nbt_t *bes = hc_nbt_list(a);
        if (!bes)
            return -1;
        if (sctx) {
            const hc_be_rec_t *recs[64];
            int32_t n = hc_structures_chunk_bes(sctx, c->cx, c->cz, recs, 64);
            for (int32_t i = 0; i < n; i++) {
                hc_nbt_t *t = be_to_nbt(a, recs[i]);
                if (!t || hc_nbt_add(a, bes, t) != 0)
                    return -1;
            }
        }
        if (hc_nbt_put(a, root, "block_entities", bes) != 0)
            return -1;
    }

    /* 틱: 레코더 배열에서 이 청크 소속만, 기록순 (R-D §4) */
    hc_nbt_t *bt = hc_nbt_list(a);
    hc_nbt_t *ft = hc_nbt_list(a);
    if (!bt || !ft)
        return -1;
    for (int32_t i = 0; i < n_ticks; i++) {
        const hc_tick_rec_t *r = &ticks[i];
        if ((r->x >> 4) != c->cx || (r->z >> 4) != c->cz)
            continue;
        hc_nbt_t *ent = tick_entry(a, r);
        if (!ent ||
            hc_nbt_add(a, r->kind == HC_TICK_BLOCK ? bt : ft, ent) != 0)
            return -1;
    }
    if (hc_nbt_put(a, root, "block_ticks", bt) != 0 ||
        hc_nbt_put(a, root, "fluid_ticks", ft) != 0)
        return -1;

    /* PostProcessing: 24 x 빈 리스트 (R-C §5; 골든 리전 전체 클리어 —
     * postProcessGeneration 이 비운다) */
    hc_nbt_t *pp = hc_nbt_list(a);
    if (!pp)
        return -1;
    for (int i = 0; i < HC_HEIGHT / 16; i++)
        if (hc_nbt_add(a, pp, hc_nbt_list(a)) != 0)
            return -1;
    if (hc_nbt_put(a, root, "PostProcessing", pp) != 0)
        return -1;

    /* Heightmaps: FULL = FINAL 4종, EnumMap 서수 순 삽입 (R-C §2/17) */
    hc_nbt_t *hm = hc_nbt_compound(a);
    if (!hm)
        return -1;
    for (int k = 0; k < HC_HMF_COUNT; k++) {
        int      type = HM_ORDINAL[k];
        int64_t *longs =
            hc_arena_alloc(a, sizeof(int64_t) * 37, _Alignof(int64_t));
        if (!longs)
            return -1;
        uint16_t raw[256];
        for (int col = 0; col < 256; col++) {
            int32_t v = c->heightmap_final[type][col];
            raw[col] = (uint16_t)(v - HC_MIN_Y); /* 빈 컬럼(-64) → 0 */
        }
        pack_bits(raw, 256, 9, longs);
        if (hc_nbt_put(a, hm, HM_KEYS[type],
                       hc_nbt_long_array(a, longs, 37)) != 0)
            return -1;
    }
    if (hc_nbt_put(a, root, "Heightmaps", hm) != 0)
        return -1;

    /* structures: starts, References — 빈 컴파운드라도 항상 (R-C §8).
     * starts 값은 골든 파스 트리 재방출 (파일 순서 put — HashMap 순회
     * 항등, nbt_read 왕복 게이트가 증명), References 는 파생 배열. */
    hc_nbt_t *st = hc_nbt_compound(a);
    if (!st)
        return -1;
    const hc_nbt_t *starts_tag =
        sctx ? hc_structures_starts_tag(sctx, c->cx, c->cz) : NULL;
    if (hc_nbt_put(a, st, "starts",
                   starts_tag ? (hc_nbt_t *)starts_tag
                              : hc_nbt_compound(a)) != 0)
        return -1;
    hc_nbt_t *refs = hc_nbt_compound(a);
    if (!refs)
        return -1;
    if (sctx) {
        const char    *names[8];
        const int64_t *arrays[8];
        int32_t        lens[8];
        int32_t nr = hc_structures_references((const hc_sctx_t *)sctx, a,
                                              c->cx, c->cz, names, arrays,
                                              lens, 8);
        for (int32_t i = 0; i < nr; i++)
            if (hc_nbt_put(a, refs, names[i],
                           hc_nbt_long_array(a, arrays[i], lens[i])) != 0)
                return -1;
    }
    if (hc_nbt_put(a, st, "References", refs) != 0 ||
        hc_nbt_put(a, root, "structures", st) != 0)
        return -1;

    return hc_nbt_write(root, out, cap);
}
