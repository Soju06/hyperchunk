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
