# R5 — StructureTemplate.updateShapeAtEdge (TreeFeature 마무리 패스), MC 26.2

R2 §8 의 "no-op 가정 [UNVERIFIED]" 을 golden 07 diff (여분 덩굴 13/골든 공기)
가 반증했다 — 나무 상자 경계의 vine 이 지지 재계산으로 삭제된다. 본 세션
recon 에이전트가 전부 바이트코드로 검증 (javap vs tools/golden/work/server).

## 1. 호출부 (TreeFeature.place 마지막)

shape = updateLeaves(...) 가 반환한 BitSetDiscreteVoxelShape (박스 상대좌표;
채움 = union(deco∪roots)∩box + BFS 가 처리한 모든 pos). flags=3 → 모든 쓰기는
flags&-2 = **2**.

## 2. updateShapeAtEdge — 면 이벤트당 2회 updateShape

```java
shape.forAllFaces((dir, fx, fy, fz) -> {
    pos = (minX+fx, minY+fy, minZ+fz);       // 채워진 셀
    npos = pos.relative(dir);                 // 빈 쪽
    s1 = getBlockState(pos); s2 = getBlockState(npos);
    s3 = s1.updateShape(level, level, pos, dir, npos, s2, level.getRandom());
    if (s1 != s3) setBlock(pos, s3, 2);
    s4 = s2.updateShape(level, level, npos, dir.getOpposite(), pos, s3, ...);
    //             ^ s2 는 사전 읽기값 (재읽기 없음); 이웃 인자는 s3 (갱신값)
    if (s2 != s4) setBlock(npos, s4, 2);
});
```
- updateOrDestroy/드랍 경로 없음 — AIR 는 그냥 setBlock.
- 앞선 면 이벤트의 쓰기가 뒤 이벤트의 읽기에 보인다 — 순서가 진리다.
- 참조 비교 s1 != s3 은 상태 인터닝으로 논리 비교와 동치.

## 3. forAllFaces 열거 순서 (AxisCycle NONE→FORWARD→BACKWARD)

1. **Z 패스**: for x { for y { z 를 0..sz 스캔 } } — rising→NORTH@(x,y,z),
   falling→SOUTH@(x,y,z-1)
2. **Y 패스**: for z { for x { y 스캔 } } — rising→DOWN, falling→UP@(x,y-1,z)
3. **X 패스**: for y { for z { x 스캔 } } — rising→WEST, falling→EAST@(x-1,y,z)

경계 밖은 무조건 빈 셀 (k==size 강제 false; prev=false 시작) — 박스 가장자리
의 채워진 셀은 바깥면을 낸다. isFull (경계검사 없음) 사용.

## 4. updateShape 오버라이드 (즉시 상태 변경만; 팔레트 내)

- **VineBlock**: dir==DOWN → 불변. 아니면 getUpdatedState: UP 면은
  isAcceptableNeighbour(above)(=canAttachTo: 지지||충돌 완전면 — 잎 통과);
  수평면 d 는 canAttachTo(pos+d) || (위가 vine 이고 같은 면). 면 0개 → AIR.
  수평 순서 N,E,S,W (단면 팔레트라 무관). RNG 0.
- **MultifaceBlock(glow_lichen)**: anyface 없으면 AIR; hasFace(dir) &&
  !canAttachTo(**전달된** 이웃 상태) → 그 면 제거 (0면 → AIR).
- **CocoaBlock**: dir==FACING && below-log 태그 실패 → AIR.
- **VegetationBlock**(grass/fern/poppy/dandelion/azalea류): !canSurvive → AIR
  (방향 무관!).
- **DoublePlantBlock**(tall_grass, small_dripleaf): Y축이고 (LOWER)==(dir==UP)
  이면 이웃이 같은 블록·반대 하프가 아니면 AIR; LOWER&&DOWN&&!canSurvive→AIR;
  이후 super(Vegetation) 의 !canSurvive→AIR (UPPER: below 같은블록+LOWER).
- **CarpetBlock**(moss_carpet): 아래 공기 → AIR.
- **GrowingPlantHead**(cave_vines, growth DOWN): dir==UP && canSurvive &&
  아래가 head|body → **BODY 로 전환** (BERRIES 보존); dir==DOWN && 이웃이
  head|body → BODY 전환. canSurvive: 위가 head|body || isFaceSturdy(위).
- **GrowingPlantBody**(cave_vines_plant): dir==DOWN && 이웃이 head|body 아님
  → HEAD 전환 + **region random nextInt(25)** (worldgen_region_random —
  피처 RNG 아님). 우리 포트는 도달 시 즉사 (그리드 미발화 확인).
- **BambooStalk**: dir==UP && 이웃 bamboo && 이웃 age>자기 age → age cycle.
- **BigDripleaf**: dir==DOWN&&!canSurvive→AIR; dir==UP&&이웃이 big_dripleaf →
  STEM 으로 (facing/wl 보존). Stem 은 tick-only (불변).
- **SporeBlossom**: dir==UP && !canSurvive → AIR.
- **잎/물/통나무 등**: tick 스케줄만 — 블록 불변 (잎 DISTANCE 는 즉시 변경
  없음 — 스케줄 틱은 월드젠에서 실행 안 됨).
- 소형 버섯(MushroomBlock)은 light 읽기 — 팔레트에 없음, 도달 시 즉사.

## 5. RandomSource

level.getRandom() = WorldGenRegion 의 `worldgen_region_random` 위치시드
(센터 청크 월드좌표) — 피처 WorldgenRandom 과 무관. 이 패스에서 드로우는
GrowingPlantBody→head 전환뿐. 전달 자체는 드로우 0.
