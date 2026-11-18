# M3 Vulkan loader injection

`native/vk_inject/vk_inject.c` builds `libtitan_vk.so`, a loader-shaped shim that
Wine `dlopen`s through `TITAN_VULKAN_LIB`. It loads the stock
`/system/lib64/libvulkan.so` and patches that library's ICD-open imports so the
vendor-ICD open becomes `TITAN_ICD`. Everything else stays the platform loader:
instance creation, extension filtering, Android WSI and the swapchain
implementation.

## Why the platform loader

Presentation is not in the ICD. `radv-xclipse` enables `RADV_USE_WSI_PLATFORM`
only for Wayland/XCB/Xlib/Display (`src/amd/vulkan/radv_wsi.h`) and its Android
file (`radv_android.c`) implements `VK_ANDROID_native_buffer` only. A Khronos
loader built with Android WSI therefore sees the RADV device but advertises
neither `VK_KHR_surface` nor `VK_KHR_android_surface` (measured,
`docs/evidence/m3-vk-loader.log`), and no loader can invent a swapchain backend
the ICD does not have.

Android presentation lives in AOSP `vulkan/libvulkan` (`swapchain.cpp`) on top of
`VK_ANDROID_native_buffer`. That is the only loader on the device that can
present with this ICD, so it is the loader the runtime uses. It ignores
`VK_ICD_FILENAMES`/`VK_DRIVER_FILES` and builds
`vulkan.${ro.hardware.vulkan}.so`; on the SM-A556E `ro.hardware.vulkan` is
`samsung`, so it asks for `vulkan.samsung.so`.

AOSP 14 `LoadDriver` passes that bare soname to
`android_load_sphal_library`, which selects the `sphal` namespace and calls
`android_dlopen_ext` with `ANDROID_DLEXT_USE_NAMESPACE`. The device binary
imports only `android_load_sphal_library`/`android_unload_sphal_library`; the
`android_dlopen_ext` call lives in `libvndksupport.so`. Other Android builds may
import `android_dlopen_ext` or `dlopen` directly, so the shim hooks all four and
patches whichever ones exist.

## Why a PLT hook instead of a rebuilt AOSP loader

- Unrooted: `/system/lib64/libvulkan.so` cannot be replaced. In-process
  redirection is the only option that keeps the stock binary.
- AOSP `libvulkan` is a platform component, not an NDK library. Rebuilding it
  means a full platform tree, matching the device's Android release, its
  `libui`/`libgui`/`libnativewindow` ABI and its linker namespace config. A
  vendored copy would drift from stock on every Android update and can break
  precisely the gralloc/surface plumbing the runtime needs.
- The Khronos loader cannot take its place: it has no `VK_KHR_swapchain` backend
  for an Android ICD, as the loader evidence shows.
- The hook changes a few GOT slots. Extension filtering, WSI, layer enumeration
  and perf-hint plumbing stay byte-identical to the vendor build.

## How the hook works

On the first entrypoint call, the shim:

1. `dlopen`s `/system/lib64/libvulkan.so` with `RTLD_NOW | RTLD_LOCAL`.
2. Finds the object at the load address `dladdr` reports for
   `vkGetInstanceProcAddr`, not by the `libvulkan.so` name: the shim is
   installed under that same name, so a name match patches the shim's own GOT
   whenever the shim is mapped before the platform loader. Then walks
   `PT_DYNAMIC` for `DT_STRTAB`, `DT_SYMTAB`, `DT_JMPREL`/`DT_PLTRELSZ` (and
   `DT_RELA` for `-fno-plt` builds).
3. Rewrites every `R_AARCH64_JUMP_SLOT` (and `R_AARCH64_GLOB_DAT`) slot whose
   symbol is `android_load_sphal_library`, `android_unload_sphal_library`,
   `android_dlopen_ext` or `dlopen`. Whichever of the four the loader imports
   are patched; no single-symbol assumption. The page is `mprotect`ed
   read-write for the store and restored to the segment's protection,
   RELRO-aware.
4. Resolves the real functions for pass-through:
   `android_load_sphal_library` and `android_unload_sphal_library` with
   `dlsym(g_platform, name)` (`libvndksupport` is a `DT_NEEDED` of the platform
   loader, so the handle search reaches it), and `android_dlopen_ext`/`dlopen`
   from `libdl.so`.

The slots are written before the platform loader's first API call, so it does
not matter whether the loader is built `-z now` or lazily binds. The AOSP loader
opens its ICD lazily, on the first Vulkan call, which the shim's own entrypoints
precede.

A replacement matches both the absolute vendor path
(`/vendor/lib64/hw/vulkan.*.so`, `/vendor/lib/hw/vulkan.*.so`) and the bare
`vulkan.*.so` soname AOSP passes. For a match it opens `TITAN_ICD` with plain
`dlopen(TITAN_ICD, RTLD_NOW)` in the caller's namespace; the platform loader's
vendor namespace cannot see `/data/app/.../lib/arm64-v8a`. Every other path,
including layers, goes to the real entrypoint with its original arguments.
`android_unload_sphal_library` closes only handles the shim returned; all other
handles go to the real unload.

## UI process gate

`vkCreateInstance` refuses to run when `libhwui.so` is already loaded, which
means the process owns an Android render thread. There the platform loader was
initialised by HWUI before the shim hooked anything, so its ICD stays the vendor
one and a second client re-initialises the vendor driver's global state; HWUI's
render thread then locks a destroyed mutex and the app aborts. Returning
`VK_ERROR_INCOMPATIBLE_DRIVER` leaves Wine without Vulkan in that one process,
which only loses win32u's adapter enumeration; the desktop and the program run in
child processes, where no HWUI is loaded and radv substitutes normally.

## Fallback

The shim never makes Vulkan worse than stock:

- `TITAN_ICD` unset, not absolute, unreadable, or the `dlopen` fails: log the
  reason and call the real entrypoint that was hooked
  (`android_load_sphal_library`, `android_dlopen_ext` or `dlopen`). The stock
  vendor ICD loads and the driver works as before; only `device.0.name` stops
  containing `RADV`.
- Platform loader missing, its load address unresolved, a real entrypoint
  unresolvable, or no matching relocation: log the reason and leave the
  unresolvable slot alone; if no slot at all is patched, forward every
  entrypoint unchanged. No partial install is reported as success;
  `titan_vk_inject_state()` leaves the hook bit clear.
- `mprotect` failure is reported as a hook failure, not as a working
  substitution.

`titan_vk_inject_state()` returns bit flags for diagnostics: `0x1` platform
loader loaded, `0x2` hook installed, `0x4` `TITAN_ICD` used, `0x8` fell back to
the vendor path.

## Environment contract

| Variable | Required | Value |
| --- | --- | --- |
| `TITAN_VULKAN_LIB` | yes, for Wine | absolute path to `libtitan_vk.so` |
| `TITAN_ICD` | yes | absolute path to a readable `vulkan.radeon.so` |

`TITAN_VULKAN_LIB` is consumed by `win32u` (patch
`0015-win32u-titan-vulkan-lib`) and must be absolute; a relative value disables
Vulkan in Wine. `TITAN_ICD` is read by the shim at the loader's ICD open and must
be absolute as well. `VK_ICD_FILENAMES`, `VK_DRIVER_FILES`, `LD_PRELOAD` and a
modified `ro.hardware.vulkan` are not used. `libtitan_vk.so` and the ICD belong
in the APK's `lib/arm64-v8a/`, which is the app namespace's permitted path.

`tests/vk/inject_run.sh` sets both variables for the on-device test:

```sh
export TITAN_VULKAN_LIB=<dir>/libtitan_vk.so
export TITAN_ICD=<dir>/vulkan.radeon.so
sh <dir>/run.sh <dir>
```

## Assumptions

- `/system/lib64/libvulkan.so` imports at least one of
  `android_load_sphal_library`, `android_unload_sphal_library`,
  `android_dlopen_ext`, `dlopen` through a relocation. Verified on the SM-A556E
  binary, which imports the sphal pair and reaches `android_dlopen_ext` only
  through `libvndksupport`; the hook fails closed if none is present on another
  build.
- The bare `vulkan.<name>.so` soname and the absolute
  `/vendor/lib64/hw/vulkan.<name>.so` / `/vendor/lib/hw/vulkan.<name>.so`
  paths both identify the vendor ICD. The shim matches the prefix and the `.so`
  suffix, not the vendor name.
- The platform loader opens its ICD on the first API call, after the shim's
  entrypoints ran. If some Android release opened the ICD at library load time,
  the hook would be too late and the test would report the vendor device.
- `android_unload_sphal_library` receives the handle the shim returned for the
  ICD; any other handle is forwarded to the real unload.
- The ICD's `DT_NEEDED` set resolves in the app namespace. CI enforces the
  radv-xclipse set (`libhardware`, `liblog`, `libnativewindow`, `libsync`,
  `libm`, `libdl`, `libc`).
- Wine's process uses x18 for the Windows TEB, so the shim is compiled
  `-ffixed-x18` and CI disassembles `.text` and fails on any `x18`/`w18`
  operand.

## What a pass looks like

`run.sh` writes `vk-inject.log`, prints every `RESULT` line and ends with a
PASS/FAIL summary. A pass needs:

- `runner.exit=0`, `inject.dlopen=ok`, `inject.instance=ok`,
  `inject.error=none`
- `inject.physical_devices=1`
- `device.0.name` containing `RADV` (for example
  `AMD Radeon Graphics (RADV TITAN)`)
- `inject.state=0x7`, `inject.hook=ok`, `inject.substituted=1`
- `inject.hook_symbol` naming the entrypoint that substituted
  (`android_load_sphal_library`, `android_dlopen_ext` or `dlopen`)
- `inject.hook_symbols` listing at least one patched slot, and
  `inject.matched_path` naming the first rewritten vendor path or soname
- `inject.ext.VK_KHR_surface=1`, `inject.ext.VK_KHR_android_surface=1`,
  `inject.ext.VK_KHR_swapchain=1`

`logcat -s titan_vk` shows the hook, the patched symbols, and the redirected
path.

Not covered by this artifact: surface creation (it needs a real `ANativeWindow`),
swapchain acquire/present, DXVK/VKD3D, and the Wine side of
`VK_KHR_android_surface`. The test stops at physical-device enumeration and
device-extension enumeration.
