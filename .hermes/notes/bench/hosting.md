# Bench host provisioning notes (2026-08-06)

## Hetzner CLI
- hcloud v1.67 @ ~/.local/bin, token = Proton Pass `HCLOUD_TOKEN` (refs 등록, hermes-secret-run 경유)
- SSH key: hyperchunk-bench (~/.ssh/hyperchunk_bench_ed25519), hcloud ssh-key id 116536829

## Probe result: Hetzner Cloud CCX ≠ bench-grade
- ccx33 (fsn1) 실측: AMD EPYC-Milan **VM** (hypervisor flag), **AVX-512 없음** (avx2까지만)
- L3 32MiB 단일 인스턴스 보고, SMT 2/core — 토폴로지는 정상 보고되나 여전히 가상화
- 판정: ADR-004 AVX-512 디스패치 검증 + 사이클 벤치에 부적격. probe 서버는 삭제함
- 결론: 진짜 베어메탈 = Hetzner **Robot** 전용서버 (hcloud API 밖, 웹 주문, 월 단위)
  - AX102 (Ryzen 9 7950X3D, Zen4 = AVX-512 지원) 급이 벤치 타깃
