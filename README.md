# TITAN-NT

Windows runtime for the Samsung Galaxy A55 (`SM-A556E`, Exynos 1480, Xclipse 530).
`PLAN.md` is the binding contract.

| Milestone | Scope | State |
| --- | --- | --- |
| M0 | CI, upstream pins, device probe | gate ran, findings recorded |
| M1 | `libtitan_forkd.so`, `x18_guard`, signal stress | gate met; results recorded |
| M2 | Wine ARM64EC + FEXCore, headless Win32/Win64 | not started |
| M3 | `radv-xclipse`, `winandroid.drv`, DX11/DX12 | not started |
| M4 | FSR1/CAS, AOT warmer, ADPF, HUD | not started |

Builds run only on GitHub Actions. Self-built arm64 code uses
`-ffixed-x18 -fno-sanitize=shadow-call-stack -march=armv8.2-a+lse` and reaches
bionic through `x18_guard`. Pins: `docs/upstream_manifest.md`. Measured device
data and plan deviations: `docs/m0-findings.md`, `docs/m1-results.md`.
