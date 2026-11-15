# TITAN-NT

Windows runtime for one device: Samsung Galaxy A55 (`SM-A556E`, Exynos 1480 / `s5e8845`,
Xclipse 530 / `TITAN`), unrooted, under external TEC cooling. `PLAN.md` is the binding
engineering contract; this repo only targets that silicon.

| Milestone | Scope | State |
| --- | --- | --- |
| M0 | CI bootstrap, upstream pins, device probe | probe awaiting A55 run |
| M1 | `libtitan_forkd.so`, `x18_guard`, signal stress harness | not started |
| M2 | Wine ARM64EC + FEXCore prebuilts, headless Win32/Win64 | not started |
| M3 | `radv-xclipse`, `winandroid.drv`, DX11/DX12 APK | not started |
| M4 | FSR1/CAS, AOT warmer, ADPF, HUD | not started |

## Ground rules

- Nothing is compiled on the local host. All native and APK builds run on GitHub Actions;
  the host only edits, commits, and drives `gh` and `adb`.
- Self-built code uses `-ffixed-x18 -fno-sanitize=shadow-call-stack -march=armv8.2-a+lse`
  and reaches bionic through `x18_guard`, because bionic keeps the Shadow Call Stack
  pointer in x18 that ARM64EC needs for the TEB.
- Pinned dependencies and their exact commits are in `docs/upstream_manifest.md`.

## M0 device probe

`native/tools/m0_probe/m0_probe.c` builds in CI and checks what the plan depends on but
cannot assume: `futex_waitv` (syscall 449) under seccomp, `memfd` sharing across `fork`,
free address space below 4 GB for the WoW64 reservation, and A55/A78 affinity masks.

```sh
gh run download -n m0-probe-arm64 -D dist/m0_probe
scripts/probe_device.sh --probe dist/m0_probe/m0_probe
```

The script writes `probe-out/device_profile.json`, `report.txt`, `probe.ndjson`, and
`dso/` with the pulled bionic DSOs plus an x18 Shadow Call Stack scan
(`scripts/dso_x18_scan.mjs`). The probe runs in the adb shell domain; `untrusted_app`
seccomp is only proven once `libtitan_forkd.so` execs it in Milestone 1.
