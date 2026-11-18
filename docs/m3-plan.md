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

**The platform loader owns Android WSI.** A Khronos loader built at
`vulkan-sdk-1.4.363.0` loads an ICD manifest and enumerates the radv device
(222 device extensions, `docs/evidence/m3-vk-loader.log`), but exposes neither
`VK_KHR_surface` nor `VK_KHR_android_surface`, and radv-xclipse has no swapchain
backend to forward them to: `src/amd/vulkan/radv_wsi.h` defines
`RADV_USE_WSI_PLATFORM` only for Wayland/XCB/Xlib/Display, and `radv_android.c`
covers `ANDROID_native_buffer` alone. Android presentation lives in AOSP
`libvulkan` (`vulkan/libvulkan/swapchain.cpp`) on top of that native-buffer
extension, so the runtime loader is the platform `/system/lib64/libvulkan.so`
with the vendor ICD redirected to our driver. Building a loader cannot supply
`VK_KHR_swapchain` for an Android ICD.

**Headless Vulkan deadlocks on a stale display cache.** `vulkan_init_once`
resolves the driver funcs through `loaderdrv_VulkanInit`, which loads the
display driver; a stale display cache makes that path call
`d3dkmt_init_vulkan`, which calls `vulkan_instance_create`, which re-enters
`__wine_get_vulkan_driver` on the same `pthread_once` and waits forever.
`0016-win32u-vulkan-init-reentry` reports no driver while the once is running.
Without a display driver every prefix is stale, so this hits the first Vulkan
call in any console process.

**The APK must keep `targetSdk 28` for M3.** API 29+ puts the app in
`untrusted_app`, which denies `dlopen` and file-backed `PROT_EXEC` mappings
under app data; Wine's unix libraries and PE files live there, and Termux runs
for the same reason. A shipping APK would move every ELF into `lib/<abi>/` and
replace the file-backed PE mapping path, which the M3 test APK does not do.

## Open

- Which of `renderD128` and `renderD129` is the Xclipse 530; the probe prints
  the sysfs identity of both rather than guessing.
- How the platform loader is made to open `vulkan.radeon.so` instead of
  `/vendor/lib64/hw/vulkan.samsung.so`: a shipped PLT hook on `libvulkan`'s
  `android_dlopen_ext` import, or a rebuilt AOSP `libvulkan` with an ICD
  override. A loader we build cannot provide `VK_KHR_swapchain`.
