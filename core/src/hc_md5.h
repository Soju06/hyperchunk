#ifndef HC_MD5_H
#define HC_MD5_H

#include <stddef.h>
#include <stdint.h>

/* RFC 1321 MD5. 내부 전용 (core/src) — 공개 ABI 아님.
 *
 * 용도는 암호화가 아니라 바닐라 RandomSupport.seedFromHashOf 재현이다:
 * 노이즈 키 문자열("minecraft:temperature" 등)의 MD5 16바이트가
 * positional fork 시드가 된다. 여기가 어긋나면 모든 노이즈가 어긋난다. */
void hc_md5(const void *data, size_t len, uint8_t out[16]);

#endif /* HC_MD5_H */
