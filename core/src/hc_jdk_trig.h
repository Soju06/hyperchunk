#ifndef HC_JDK_TRIG_H
#define HC_JDK_TRIG_H

/* JDK HotSpot x86-64 Math.sin/Math.cos 인트린식 스텁 (Intel LIBM) 의
 * 비트 정확 C 이식 — jdk25u src/hotspot/cpu/x86/
 *   stubGenerator_x86_64_sin.cpp / _cos.cpp / _constants.cpp.
 * java.lang.Math.sin/cos 스펙은 1-ulp 라 libm 교체에 취약하다 —
 * OreFeature 각도 (task9a A3 §1) 는 이 스텁 출력과 비트 일치해야 한다.
 * 게이트: golden/rng/jdk_sincos.txt (tests/unit/test_features_rng.c).
 *
 * 도메인 계약: 유한 비음수 x, 스텁 메인 패스 문턱 미만 (지수장 기준
 * ~90112 = 0x40F5xxxx 상한; 2^-252 미만은 소인자 경로). 실호출 도메인은
 * ore 각도 [0, pi) 이고 x=+0.0 포함 (sin(+0)=+0, cos(+0)=1). 범위 밖
 * (거대 인자 Payne-Hanek 경로, NaN/Inf) 은 abort() 한다. */

double hc_jdk_sin(double x);
double hc_jdk_cos(double x);

#endif /* HC_JDK_TRIG_H */
