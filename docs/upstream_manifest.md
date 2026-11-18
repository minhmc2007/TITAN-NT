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
| `0010-wineandroid-driver-api-port` | TITAN | upstream's win32u drawable refactor left the unbuilt driver with a stale prototype and surface-create signature |
| `0011-wineandroid-start-device-callback` | TITAN | the unixlib init params carry the start-device callback as a client pointer, not a function pointer |
| `0012-winevulkan-android-surface` | TITAN | the Android driver needs `vkCreateAndroidSurfaceKHR`, which `make_vulkan` drops with the other platform extensions |
| `0013-wineandroid-vulkan-surface` | TITAN | create the host Android surface from the registered client `ANativeWindow` |
| `0014-wineandroid-native-window-ops` | TITAN | forward the dataspace/usage/refresh/timeout perform calls the loader's WSI makes |
| `0015-win32u-titan-vulkan-lib` | TITAN | `TITAN_VULKAN_LIB` selects a custom-driver loader instead of the AOSP SONAME |
| `0016-win32u-vulkan-init-reentry` | TITAN | the display-driver load inside `vulkan_init_once` enumerates Vulkan GPUs, which re-enters the same `pthread_once` and deadlocks |

`dlls/wineandroid.drv` is compiled in the `ci-deps-cpu` wine job since Milestone 3. It is
not a default display driver: `programs/explorer/desktop.c` keeps `mac,x11,wayland` as its
default list, so `wineboot -i` and console tests stay on the null driver unless explorer is
started with `/desktop=...,android` (which the Java host does). Wine's Vulkan stack
(`winevulkan.dll`/`winevulkan.so` plus the `vulkan-1.dll` loader) is built in the same job;
`win32u.so` dlopens the host loader rather than linking it.

The Android driver wraps the registered client window in an `ANativeWindow` and calls the
host loader's `vkCreateAndroidSurfaceKHR`; `map_instance_extensions` exposes
`VK_KHR_android_surface` to the client as `VK_KHR_win32_surface` and clears the host flag
before building the host enable list, so applications keep using Win32 surfaces. If the
loader does not advertise `VK_KHR_android_surface`, the driver logs an ERR, drops the client
Win32 surface extension and returns `VK_ERROR_EXTENSION_NOT_PRESENT`; Wine does not fake
Android WSI itself. The loader must also accept the perform calls the WSI makes
(`SET_BUFFERS_DATASPACE`, `SET_SHARED_BUFFER_MODE`, `SET_AUTO_REFRESH`,
`GET_REFRESH_CYCLE_DURATION`, `SET_USAGE64`, `GET_CONSUMER_USAGE64`,
`SET_DEQUEUE_TIMEOUT`); the fake window forwards them to the Java surface.

`TITAN_VULKAN_LIB` must be an absolute path to the loader; `win32u` dlopens it instead of
`SONAME_LIBVULKAN`, and a relative value logs an ERR and disables Vulkan. The radv-xclipse
Android package is an adrenotools-style custom-driver zip (`meta.json`, `vulkan.radeon.so`,
`NOTICE.txt`, `minApi: 34`) consumed by a custom-driver loader, not an AOSP ICD. The ICD
enables the device-side `ANDROID_native_buffer` and
`ANDROID_external_memory_android_hardware_buffer` (`src/amd/vulkan/radv_physical_device.c`)
but no `VK_KHR_android_surface`, so the loader owns `vkCreateAndroidSurfaceKHR` and Android
WSI, and the swapchain passes `buffer->handle` in `VkNativeBufferANDROID` for
`radv_android.c` to import as a dma_buf fd. Wine only dlopens that loader; whether it
advertises `VK_KHR_android_surface` is an on-device check, not something CI covers.

`deps/patches/fex/` is `unixlib-android.patch` (no `librt`, `shm_open`/`shm_unlink`
stubs) plus upstream `71444c94` (threads killed by `NtTerminateProcess` left FEX mutexes
held), both from `amphora-engine`.


## GPU dependency build (`ci-deps-gpu.yml`)

| Item | Version | Source |
| --- | --- | --- |
| Android NDK | `30.0.16248370` | preinstalled on `ubuntu-latest` |
| llvm-mingw (DXVK) | `20260721` (ucrt, x86_64 host) | `mstorsjo/llvm-mingw` release asset |
| llvm-mingw (vkd3d) | `20260908` (ucrt, x86_64 host) | `mstorsjo/llvm-mingw` release asset |
| RADV target | `aarch64-linux-android34` | `radv-xclipse` `build.sh` recipe |
| DXVK target | `arm64ec-w64-mingw32` | llvm-mingw clang wrappers |
| vkd3d target | `arm64ec-w64-mingw32` with `-marm64x` | llvm-mingw clang wrappers |

`radv-xclipse` `v1.9.0` is built with the driver's own `build.sh` options except
`-Dbuildtype=release` and the TITAN code-generation flags
`-march=armv8.2-a -ffixed-x18 -fno-sanitize=shadow-call-stack`. The published
`v1.9.0` binary does use x18 (a context-restore stub at `0x10c4390` reads
`ldp x18, x19, [x0, #0x90]`), so the CI job disassembles both the unstripped and
stripped driver and fails if any `x18`/`w18` operand remains. The NDK's
`libunwind.a` saves and restores x18 in `UnwindRegistersSave.S.o` and
`UnwindRegistersRestore.S.o`; the job rewrites those two instructions in the
archive clang actually links before building. The archive uses the release
`meta.json` contract (`schemaVersion: 1`, `libraryName: "vulkan.radeon.so"`)
and the job fails on any NEEDED entry outside
`libhardware.so liblog.so libnativewindow.so libsync.so libm.so libdl.so libc.so`.

DXVK `v3.1.1` has no ARM64EC machine file and `meson.build` passes
`-msse -msse2 -msse3 -mfpmath=sse` unconditionally. `deps/patches/dxvk/0001-arm64ec.patch`
gates those flags to x86/x86_64 and adds `__arm64ec__` to the `DXVK_ARCH_ARM64`
branch so the ARM64 `yield` path is used. It is adapted from
`orchardxr/varbor-engines` commit `4c78a2e7a8585c21d76dc75679bf251363ac02c3`,
`patches/dxvk/cxaddon-1.10.3/0002-arm64ec.patch` (written for DXVK 1.10.3). The
other hunks in that patch are not needed here: v3.1.1 already guards
`__arm64ec__` in `util_bit.h`, `util_hotpatch.h` and `d3d9_device.cpp`, keeps
`D3DDEVINFO_RESOURCEMANAGER` inside `namespace dxvk`, includes `<algorithm>` in
`src/util/config/config.cpp`, and no longer has `replaceNaN` or
`d3d10_interfaces.h`. The job runs `arm64ec-w64-mingw32-strip --strip-all` on the
staged DLLs because meson's `--strip` applies at install time and the DLLs are
copied from the build tree.

vkd3d-proton `v3.1` ships `build-arm64x.txt` but names
`arm64ec-w64-mingw32-gcc`/`g++`, and `meson.build` passes `-msse`/`-msse2`
unconditionally. `deps/patches/vkd3d/0001-arm64ec.patch` switches the machine file
to the llvm-mingw clang wrappers, keeps `-marm64x`, and gates the SSE flags to x86.
llvm-mingw `20260908` has no `arm64ec-w64-mingw32` sysroot; its hybrid ARM64X
libraries live under `aarch64-w64-mingw32`. The job shadows the cross file's
`arm64ec-w64-mingw32-clang`/`clang++` names with wrappers that exec the `aarch64`
compilers and drop `-marm64x` for preprocess-only runs (`-E`, `-M`, `-MM`, `-S`,
`-fsyntax-only`), where clang rejects it as unused. The `widl` requirement is met
by llvm-mingw's `arm64ec-w64-mingw32-widl` through `widl-mingw-tools-fallback`; no
Wine build is needed in the job.

All three jobs publish workflow artifacts only (`retention-days: 30`); nothing is
attached to a GitHub Release.


## Fork policy

No fork exists yet. Forks are created only when a milestone needs a patch, and the patch
commit must be pinned here before CI consumes it. Dependencies are fetched at the pinned
commit inside CI; they are not vendored and not added as submodules, so the local Arch ARM
host never holds multi-gigabyte trees.
