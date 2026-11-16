# Milestone 3 plan and findings

Gate (PLAN.md): DX11 and DX12 samples render at 60/120 Hz on the A55, no black
screens, no `sync_file` fd leak over 15 min, `RADV_XCLIPSE_PERF=1` active.

Work runs in three branches so the CI builds do not share an index:

| Branch | Scope |
| --- | --- |
| `m3/gpu-deps` | `ci-deps-gpu.yml`: radv-xclipse, DXVK ARM64EC, VKD3D-Proton ARM64X |
| `m3/vk-probe` | native ICD probe, x18 canary, render-node identification |
| `m3/wine-android` | enable `wineandroid.drv` and `winevulkan`, Android Vulkan surface path |

## Findings

**DXVK v3.1.1 has no ARM64EC build.** Upstream ships only `build-win32.txt` and
`build-win64.txt`, and `meson.build` passes `-msse -msse2 -msse3 -mfpmath=sse`
unconditionally. The tree already carries `__arm64ec__` guards in
`src/util/util_bit.h`, `src/util/util_hotpatch.h` and `src/d3d9/d3d9_device.cpp`,
so the delta is small: gate the SSE flags to x86, add the `yield` spin, the
scalar `replaceNaN`, the `bcmpeq`/`tzcnt` fallbacks and two MinGW header
duplicate guards. `deps/patches/dxvk/0001-arm64ec.patch` carries it.

**The published radv-xclipse binary is not safe inside Wine.** The v1.9.0
`vulkan.radeon.so` uses x18 as a general register in a context-restore stub
(`ldp x18, x19, [x0, #0x90]` at 0x10c4390). In a Wine ARM64EC process x18 is the
TEB, so the shipped binary would corrupt the process. Our build adds
`-ffixed-x18 -fno-sanitize=shadow-call-stack` and the probe fails hard if x18
changes across a driver call.

**Android's Vulkan loader cannot be pointed at our ICD.** AOSP's
`vulkan/libvulkan` has no `VK_ICD_FILENAMES` or `VK_DRIVER_FILES` handling; it
enumerates `/vendor/lib64/hw/vulkan.*.so` from `ro.hardware.vulkan`. Unrooted
selection therefore needs a loader we ship. Wine dlopens `SONAME_LIBVULKAN` in
both `win32u` and `winevulkan`, so the runtime points `TITAN_VULKAN_LIB` at an
absolute path and the patch honours it.

**The surface hook already exists.** `win32u/vulkan.c` calls
`driver_funcs->p_vulkan_surface_create( hwnd, ... )`; `winex11.drv/vulkan.c` is
the template with `VkXlibSurfaceCreateInfoKHR`, and the Android driver needs
`VkAndroidSurfaceCreateInfoKHR` plus the `win32_surface` <-> `android_surface`
extension mapping. `p_map_device_extensions` must keep the
`external_memory/semaphore/fence` win32 <-> fd mapping or DXVK and vkd3d cannot
share resources.

**The APK cannot load code from app data.** A modern `targetSdk` blocks
`dlopen` and file-backed `PROT_EXEC` mappings outside `lib/<abi>/`. ELF
libraries (Wine unix libs, ICD, loader) go in `lib/arm64-v8a/`; PE files stay in
assets and are read by Wine's loader, which already carries the anon-remap
patches from M2. Termux's `targetSdk` 28 hid this; the APK will not.

## Open

- Which of `renderD128` and `renderD129` is the Xclipse 530; the probe prints
  the sysfs identity of both rather than guessing.
- Whether Khronos' loader or a thin ICD shim is smaller to carry; both satisfy
  `TITAN_VULKAN_LIB`, the loader has to be built for Android.
