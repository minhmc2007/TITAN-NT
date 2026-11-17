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
- `proton-wine-11.0-2e` already carries `wineandroid.drv` and the `linux-android*` host
  branch, and Wine's own PE ntdll implements the ARM64EC OS side (EC code bitmap, hybrid
  dispatch, `MEM_EXTENDED_PARAMETER_EC_CODE`), so FEX's ARM64EC backend runs under Wine
  without a Windows kernel. The remaining bionic delta is the patch stack in
  `deps/patches/wine/`, rebased from the Amphora tree (see below).

## CPU dependency build (`ci-deps-cpu.yml`)

| Item | Version | Source |
| --- | --- | --- |
| Android NDK | `30.0.16248370` | preinstalled on `ubuntu-latest` |
| llvm-mingw | `20260721` (ucrt, x86_64 host) | `mstorsjo/llvm-mingw` release asset |
| Target triple | `aarch64-linux-android30` | Amphora's proven baseline |
| PE archs | `arm64ec,aarch64,i386` (+ implied `x86_64`) | `--enable-archs` |

`deps/patches/wine/` started as the seven-commit bionic patch stack from
`amphora-dev/proton-wine` branch `amphora/exp-11.0-20261027b` (commit `7b3ceaf8644b`),
rebased onto `e60892cc`. Six of those survive; `ca552f7c9c` is replaced by TITAN's own
fsync patch because it only turned fsync off. All apply to `e60892cc` with offsets only;
provenance:

| Patch | Origin | Purpose |
| --- | --- | --- |
| `0001-configure-android-no-preloader` | Amphora `cc7a988069` | linker64 cannot load the static preloader |
| `0002-ntdll-android-linker64-exec` | Amphora `d64ec180bf` | run app-private ELFs through `/system/bin/linker64` |
| `0003-ntdll-android-execmod-anon-mremap` | Amphora `5220da919d` | SELinux denies `execmod` on relocated PE pages |
| `0004-ntdll-android-protect-exec-sections` | Amphora `d41f6df033` | relocate without writing RO/exec pages |
| `0005-fsync-android-epoch` | TITAN (replaces Amphora `ca552f7c9c`) | file-backed shm plus a wake epoch, since bionic has no `shm_open` and app seccomp traps `futex_waitv` |
| `0006-ntdll-android-no-m-perturb` | Amphora `0d15b86a21` | bionic has no `M_PERTURB` |
| `0007-server-android-nls-from-argv0` | Amphora `ad85a24b20` | `/proc/self/exe` is linker64, not wineserver |
| `0008-ntdll-android-no-userfaultfd` | TITAN | upstream hardcodes the x86_64 `userfaultfd` number for every arch; arm64 is 282 and Android traps it |
| `0009-wineandroid-no-gradle-apk` | TITAN | the cross build has no Android SDK/Gradle; the Java host APK belongs to `ci-m3-apk` |

`dlls/wineandroid.drv` is compiled in the `ci-deps-cpu` wine job since Milestone 3. It is
not a default display driver: `programs/explorer/desktop.c` keeps `mac,x11,wayland` as its
default list, so `wineboot -i` and console tests stay on the null driver unless explorer is
started with `/desktop=...,android` (which the Java host does). Wine's Vulkan stack
(`winevulkan.dll`/`winevulkan.so` plus the `vulkan-1.dll` loader) is built in the same job;
`win32u.so` dlopens the host loader rather than linking it.

`deps/patches/fex/` is `unixlib-android.patch` (no `librt`, `shm_open`/`shm_unlink`
stubs) plus upstream `71444c94` (threads killed by `NtTerminateProcess` left FEX mutexes
held), both from `amphora-engine`.


## Fork policy

No fork exists yet. Forks are created only when a milestone needs a patch, and the patch
commit must be pinned here before CI consumes it. Dependencies are fetched at the pinned
commit inside CI; they are not vendored and not added as submodules, so the local Arch ARM
host never holds multi-gigabyte trees.
