/* 10_spawn / 11_full 스테이지 진입점 (Task 11).
 *
 * 바닐라 대응 (26.2 바이트코드 — .hermes/notes/task11-spawnfull/
 * R1-bytecode-spawn-full.md):
 *  - 10 SPAWN: ChunkStatusTasks.generateSpawn = spawnOriginalMobs →
 *    NaturalSpawner.spawnMobsForChunkGeneration — CREATURE 카테고리 mob
 *    부기뿐. 블록/바이옴/하이트맵/라이트 쓰기 없음 (읽기 전용 접근만).
 *    golden 09→10 전 덤프 종류 바이트 동일 (핸드오프 실측). 여기서는
 *    상태 마커만 올린다.
 *  - 11 FULL: ChunkStatusTasks.full → LevelChunk(ServerLevel, ProtoChunk,
 *    ...) 전환. 하이트맵은 proto 맵을 순회하며 ChunkStatus.FULL
 *    .heightmapsAfter() == FINAL_HEIGHTMAPS (클라이언트 4종) 만
 *    setRawData 로 비트 복사 — 재계산 없음, 전환 시점 프라임 없음
 *    (없는 타입은 그대로 부재; 이후 첫 읽기에서 지연 프라임 —
 *    features.c 의 lazy 경로가 같은 의미). *_WG 두 종은 조용히 소멸.
 *    블록/바이옴 섹션은 참조 승계 (쓰기 없음).
 *
 * C 표현: heightmap_final[4] 가 곧 생존 집합이므로 복사는 표현상 무비용
 * — promoted=11 마커가 "*_WG 부재" 계약이다. 전환 후의 *_WG 읽기 (이웃
 * 데코가 ImposterProtoChunk 를 통해 읽는 경우) 는 현재 블록 첫-읽기
 * 재프라임 의미 — features.c 의 rg->wg_dropped 경로가 이미 같은 의미를
 * 구현한다 (리로드 웨이브와 FULL 전환이 같은 모델로 수렴). */

#include "hc_features.h"

void hc_gen_spawn_stage(hc_chunk_t *c) {
    c->promoted = 10;
}

void hc_gen_full_stage(hc_chunk_t *c) {
    c->promoted = 11;
}
