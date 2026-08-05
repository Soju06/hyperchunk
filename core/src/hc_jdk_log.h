#ifndef HC_JDK_LOG_H
#define HC_JDK_LOG_H

/* JDK HotSpot x86-64 Math.log 인트린식 스텁 (Intel LIBM dlog) 의
 * 비트 정확 C 이식 — jdk25u src/hotspot/cpu/x86/
 *   stubGenerator_x86_64_log.cpp (상수 _L_tbl/_log2/_coeff 도 같은 파일).
 * java.lang.Math.log 스펙은 1-ulp 라 libm 교체에 취약하다 —
 * MarsagliaPolarGaussian 의 Math.log(r²), r² ∈ (0,1) 은 glibc log() 와
 * 일부 입력에서 1 ulp 어긋나므로 이 스텁 출력과 비트 일치해야 한다.
 * 게이트: golden/rng/jdk_log.txt (tests/unit/test_jdk_log.c).
 *
 * 도메인 계약: 전 도메인 전사 (특수 경로 포함) — log(±0)=−Inf,
 * log(음수/−Inf)=QNaN(0xfff8…), log(+Inf)=+Inf, log(NaN)=x+x(quiet),
 * log(1)=+0, 서브노멀은 2^128 재정규화 후 메인 패스 재진입.
 * 주의: 스텁의 rcpps 근사 역수는 CPU 벤더 의존 — 이식은 같은 기계에서
 * 같은 명령을 실행한다 (골든도 이 기계 캡처; x86-64 전용). */

double hc_jdk_log(double x);

#endif /* HC_JDK_LOG_H */
