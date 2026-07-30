#include "hc_blocks.h"

#include <string.h>

/* 순서는 hc_blocks.h enum 과 1:1 */
static const char *const NAMES[HC_B_COUNT] = {
    "minecraft:air",
    "minecraft:stone",
    "minecraft:water[level=0]",
    "minecraft:lava[level=0]",
    "minecraft:copper_ore",
    "minecraft:raw_copper_block",
    "minecraft:granite",
    "minecraft:deepslate_iron_ore",
    "minecraft:raw_iron_block",
    "minecraft:tuff",
    "minecraft:bedrock",
    "minecraft:deepslate[axis=y]",
    "minecraft:dirt",
    "minecraft:grass_block[snowy=false]",
    "minecraft:coarse_dirt",
    "minecraft:sand",
    "minecraft:sandstone",
    "minecraft:red_sand",
    "minecraft:red_sandstone",
    "minecraft:gravel",
    "minecraft:mud",
    "minecraft:mycelium[snowy=false]",
    "minecraft:podzol[snowy=false]",
    "minecraft:snow_block",
    "minecraft:powder_snow",
    "minecraft:ice",
    "minecraft:packed_ice",
    "minecraft:calcite",
    "minecraft:terracotta",
    "minecraft:white_terracotta",
    "minecraft:orange_terracotta",
    "minecraft:yellow_terracotta",
    "minecraft:brown_terracotta",
    "minecraft:red_terracotta",
    "minecraft:light_gray_terracotta",
    "minecraft:sulfur",
    "minecraft:cinnabar",
};

const char *hc_block_name(uint16_t id) {
    return NAMES[id];
}

int32_t hc_block_by_name(const char *name, int32_t len) {
    for (int32_t i = 0; i < HC_B_COUNT; i++)
        if ((int32_t)strlen(NAMES[i]) == len &&
            memcmp(NAMES[i], name, (size_t)len) == 0)
            return i;
    return -1;
}

int hc_block_is_air(uint16_t id) {
    return id == HC_B_AIR;
}

int hc_block_is_fluid(uint16_t id) {
    return id == HC_B_WATER || id == HC_B_LAVA;
}

int hc_block_blocks_motion(uint16_t id) {
    return id != HC_B_AIR && id != HC_B_WATER && id != HC_B_LAVA &&
           id != HC_B_POWDER_SNOW;
}
