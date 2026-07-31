#ifndef HC_REGION_H
#define HC_REGION_H

#include <stddef.h>
#include <stdint.h>

/* Anvil 리전(.mca) 이미지 조립 — 내부 전용 (core/src), 메모리 버퍼만
 * 다룬다 (파일 I/O 는 호출자 몫, ADR-003 D1).
 *
 * 게이트(compare_regions.py --canonical-hash)는 zlib 해제 페이로드만
 * 보므로 섹터 배치/타임스탬프/압축 레벨은 자유다 (Task-12 핸드오프).
 * 여기서는 무의존성 원칙에 따라 zlib 컨테이너를 stored-block DEFLATE
 * (무압축) + adler32 수제로 쓴다 — 표준 zlib 파서가 그대로 해제한다.
 *
 * 컨테이너 규약 (26.2 실측):
 *  - 헤더 섹터 2개: 오프셋 테이블(1024 x u32be: sector<<8|count),
 *    타임스탬프 테이블(1024 x u32be)
 *  - 청크 엔트리: u32be length(= 압축길이+1) + u8 compression(2=zlib)
 *    + 압축 바이트, 4096 배수로 0 패딩
 *  - 부재 청크 = 오프셋 엔트리 0 */

typedef struct {
    int32_t        x, z;     /* 리전 내 청크 좌표 [0,32) */
    const uint8_t *payload;  /* 무압축 NBT 페이로드 */
    size_t         len;
} hc_region_chunk_t;

/* .mca 이미지를 out 에 조립. 성공 시 파일 총 길이, 초과/오류 시 -1.
 * timestamp 는 전 청크 공통 (canonical 게이트 무관, 비제로 권장). */
ptrdiff_t hc_region_write(const hc_region_chunk_t *chunks, int n,
                          uint32_t timestamp, uint8_t *out, size_t cap);

#endif /* HC_REGION_H */
