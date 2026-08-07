#include "hc_sha256.h"

/* SHA-NI 블록 압축 백엔드 (P2-5) — FIPS 180-4 sha256 의 하드웨어 구현
 * (sha256rnds2/sha256msg1/sha256msg2, Intel SHA Extensions 레퍼런스 코드
 * 구조). SHA-256 은 정수 순수함수라 소프트웨어 경로와 비트 동일이
 * 정의로 성립하고, test_sha256 (KAT + 이중 백엔드 배터리) 와
 * check_sha_equiv.sh (풀 리전 양 백엔드 canonical) 가 구현 오류를 잡는다.
 *
 * TU 격리: 이 파일만 -msha -msse4.1 로 컴파일 (core/CMakeLists.txt —
 * df_simd_avx2.c 와 동일 정책). 진입은 cpuid 디스패치
 * (sha256.c hc_sha256_ni_active) 뒤에만 — 다른 TU 에 SHA 인트린식 금지
 * (부재 호스트 SIGILL, P2-4 함정 기록과 동일).
 *
 * K 상수 벡터는 sha256.c K[64] 와 같은 값의 사본 (FIPS 180-4 §4.2.2) —
 * 불일치는 KAT 가 즉시 잡는다 (GRAD4 사본 규약과 동일). */

#if defined(__x86_64__) || defined(__i386__)

#include <immintrin.h>

void hc_sha256_blocks_ni(uint32_t h[8], const uint8_t *p, size_t nblocks) {
    /* h = {a..h} 워드 배열 → STATE0=ABEF, STATE1=CDGH (rnds2 레이아웃) */
    __m128i tmp = _mm_loadu_si128((const __m128i *)&h[0]);
    __m128i state1 = _mm_loadu_si128((const __m128i *)&h[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xB1);       /* CDAB */
    state1 = _mm_shuffle_epi32(state1, 0x1B); /* EFGH */
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);    /* ABEF */
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);         /* CDGH */

    /* 입력 워드 BE→LE (32비트 레인 내 바이트 반전) */
    const __m128i mask =
        _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

    while (nblocks--) {
        __m128i abef_save = state0;
        __m128i cdgh_save = state1;
        __m128i msg, msg0, msg1, msg2, msg3;

        /* 라운드 0-3 */
        msg = _mm_loadu_si128((const __m128i *)(p + 0));
        msg0 = _mm_shuffle_epi8(msg, mask);
        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0xE9B5DBA5B5C0FBCFULL,
                                                 0x71374491428A2F98ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        /* 라운드 4-7 */
        msg1 = _mm_loadu_si128((const __m128i *)(p + 16));
        msg1 = _mm_shuffle_epi8(msg1, mask);
        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0xAB1C5ED5923F82A4ULL,
                                                 0x59F111F13956C25BULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg0 = _mm_sha256msg1_epu32(msg0, msg1);

        /* 라운드 8-11 */
        msg2 = _mm_loadu_si128((const __m128i *)(p + 32));
        msg2 = _mm_shuffle_epi8(msg2, mask);
        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0x550C7DC3243185BEULL,
                                                 0x12835B01D807AA98ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg1 = _mm_sha256msg1_epu32(msg1, msg2);

        /* 라운드 12-15 */
        msg3 = _mm_loadu_si128((const __m128i *)(p + 48));
        msg3 = _mm_shuffle_epi8(msg3, mask);
        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0xC19BF1749BDC06A7ULL,
                                                 0x80DEB1FE72BE5D74ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg3, msg2, 4);
        msg0 = _mm_add_epi32(msg0, tmp);
        msg0 = _mm_sha256msg2_epu32(msg0, msg3);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg2 = _mm_sha256msg1_epu32(msg2, msg3);

        /* 라운드 16-19 */
        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0x240CA1CC0FC19DC6ULL,
                                                 0xEFBE4786E49B69C1ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg0, msg3, 4);
        msg1 = _mm_add_epi32(msg1, tmp);
        msg1 = _mm_sha256msg2_epu32(msg1, msg0);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg3 = _mm_sha256msg1_epu32(msg3, msg0);

        /* 라운드 20-23 */
        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0x76F988DA5CB0A9DCULL,
                                                 0x4A7484AA2DE92C6FULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg1, msg0, 4);
        msg2 = _mm_add_epi32(msg2, tmp);
        msg2 = _mm_sha256msg2_epu32(msg2, msg1);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg0 = _mm_sha256msg1_epu32(msg0, msg1);

        /* 라운드 24-27 */
        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0xBF597FC7B00327C8ULL,
                                                 0xA831C66D983E5152ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg2, msg1, 4);
        msg3 = _mm_add_epi32(msg3, tmp);
        msg3 = _mm_sha256msg2_epu32(msg3, msg2);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg1 = _mm_sha256msg1_epu32(msg1, msg2);

        /* 라운드 28-31 */
        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0x1429296706CA6351ULL,
                                                 0xD5A79147C6E00BF3ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg3, msg2, 4);
        msg0 = _mm_add_epi32(msg0, tmp);
        msg0 = _mm_sha256msg2_epu32(msg0, msg3);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg2 = _mm_sha256msg1_epu32(msg2, msg3);

        /* 라운드 32-35 */
        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0x53380D134D2C6DFCULL,
                                                 0x2E1B213827B70A85ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg0, msg3, 4);
        msg1 = _mm_add_epi32(msg1, tmp);
        msg1 = _mm_sha256msg2_epu32(msg1, msg0);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg3 = _mm_sha256msg1_epu32(msg3, msg0);

        /* 라운드 36-39 */
        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0x92722C8581C2C92EULL,
                                                 0x766A0ABB650A7354ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg1, msg0, 4);
        msg2 = _mm_add_epi32(msg2, tmp);
        msg2 = _mm_sha256msg2_epu32(msg2, msg1);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg0 = _mm_sha256msg1_epu32(msg0, msg1);

        /* 라운드 40-43 */
        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0xC76C51A3C24B8B70ULL,
                                                 0xA81A664BA2BFE8A1ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg2, msg1, 4);
        msg3 = _mm_add_epi32(msg3, tmp);
        msg3 = _mm_sha256msg2_epu32(msg3, msg2);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg1 = _mm_sha256msg1_epu32(msg1, msg2);

        /* 라운드 44-47 */
        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0x106AA070F40E3585ULL,
                                                 0xD6990624D192E819ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg3, msg2, 4);
        msg0 = _mm_add_epi32(msg0, tmp);
        msg0 = _mm_sha256msg2_epu32(msg0, msg3);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg2 = _mm_sha256msg1_epu32(msg2, msg3);

        /* 라운드 48-51 */
        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0x34B0BCB52748774CULL,
                                                 0x1E376C0819A4C116ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg0, msg3, 4);
        msg1 = _mm_add_epi32(msg1, tmp);
        msg1 = _mm_sha256msg2_epu32(msg1, msg0);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg3 = _mm_sha256msg1_epu32(msg3, msg0);

        /* 라운드 52-55 (메시지 스케줄 종료 구간) */
        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0x682E6FF35B9CCA4FULL,
                                                 0x4ED8AA4A391C0CB3ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg1, msg0, 4);
        msg2 = _mm_add_epi32(msg2, tmp);
        msg2 = _mm_sha256msg2_epu32(msg2, msg1);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        /* 라운드 56-59 */
        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0x8CC7020884C87814ULL,
                                                 0x78A5636F748F82EEULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg2, msg1, 4);
        msg3 = _mm_add_epi32(msg3, tmp);
        msg3 = _mm_sha256msg2_epu32(msg3, msg2);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        /* 라운드 60-63 */
        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0xC67178F2BEF9A3F7ULL,
                                                 0xA4506CEB90BEFFFAULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        state0 = _mm_add_epi32(state0, abef_save);
        state1 = _mm_add_epi32(state1, cdgh_save);
        p += 64;
    }

    /* ABEF/CDGH → {a..h} 역배열 저장 */
    tmp = _mm_shuffle_epi32(state0, 0x1B);       /* FEBA */
    state1 = _mm_shuffle_epi32(state1, 0xB1);    /* DCHG */
    state0 = _mm_blend_epi16(tmp, state1, 0xF0); /* DCBA */
    state1 = _mm_alignr_epi8(state1, tmp, 8);    /* HGFE */
    _mm_storeu_si128((__m128i *)&h[0], state0);
    _mm_storeu_si128((__m128i *)&h[4], state1);
}

#else /* 비-x86: 디스패치가 소프트웨어를 돌려 도달 불가 — 링크 스텁 */

#include <stdlib.h>

void hc_sha256_blocks_ni(uint32_t h[8], const uint8_t *p, size_t nblocks) {
    (void)h;
    (void)p;
    (void)nblocks;
    abort();
}

#endif
