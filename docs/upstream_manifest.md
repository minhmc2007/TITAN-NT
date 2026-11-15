# Upstream manifest

Pins verified with `git ls-remote` and `gh api` on 2026-11-15. Every CI job builds from
these commits, not from a branch head.

| Dependency | Repository | Tag | Commit | License |
| --- | --- | --- | --- | --- |
| RADV for Xclipse | `JimVulkan/radv-xclipse` | `v1.9.0` | `cc87fdf71267dd0acc3b470920a6566a1a46f0be` | MIT |
| CPU translator | `FEX-Emu/FEX` | `FEX-2611.1` | `4f9c1f1e36443d22d7655477e34f51b95b685bf1` | MIT |
| Wine (ARM64EC) | `ValveSoftware/wine` | `proton-wine-11.0-2e` | `e60892cc2398b0a83c332a2078c7b85e47bdb42b` | LGPL-2.1+ |
| D3D11 | `doitsujin/dxvk` | `v3.1.1` | `b1a1c99ab52b687cf950d62c88bc2fa316b41663` | Zlib |
| D3D12 | `HansKristian-Work/vkd3d-proton` | `v3.1` | `4522cc1a8b1347f1ab1b0f7b14a23ed9444d95d1` | LGPL-2.1 |
| Allocator | `microsoft/mimalloc` | `v3.6.1` | `56dc5eb80145f9a3ba7ca23d47c6a248b9d8586c` | MIT |

Re-verify with:

```sh
# Peeled tag commit, not the tag object: annotated tags print both lines.
git ls-remote https://github.com/microsoft/mimalloc.git 'refs/tags/v3.6.1*'
gh api repos/HansKristian-Work/vkd3d-proton/releases/latest --jq .tag_name
```

## Notes that affect the plan

- `radv-xclipse` targets Mesa 27.0.0-devel and lists Xclipse 530 (`TITAN`, Galaxy A55) as
  supported with no hardware ray tracing. `v1.9.0` and `main` are the same commit as of the
  pin date, so no fork is needed to start Milestone 3. The driver's own build script is
  tested against NDK r29; TITAN code stays on r27+ as `PLAN.md` requires.
- The driver converts BC4-BC7 in place (EAC/ASTC/BC3) and exposes
  `debug.radv_xclipse_no_bc_emu` to hide those formats, which confirms the plan's rule that
  DXVK must not run its own BCn emulation. The alias switches
  (`radv_xclipse_bc5_alias`, `bc7_alias`, `bc7srgb_rgba8`) trade memory for exactness and
  matter for the 2560 MB `dxgi.maxDeviceMemory` budget.
- `FEXCore/HostFeatures.h` carries `SupportsRCPC` and `Source/Windows/ARM64EC/` exists, so
  the Cortex-A55 feature gate and the ARM64EC entry point named in `PLAN.md` are real
  surfaces, not assumptions.
- Current Wine trees ship no Android display driver, so `winandroid.drv` is a from-scratch
  driver rather than a port. Wine-on-Bionic stays the largest unproven assumption in the
  plan; Milestone 2 proves it with a headless `cmd.exe` before any GPU work.

## Fork policy

No fork exists yet. Forks are created only when a milestone needs a patch, and the patch
commit must be pinned here before CI consumes it. Dependencies are fetched at the pinned
commit inside CI; they are not vendored and not added as submodules, so the local Arch ARM
host never holds multi-gigabyte trees.
