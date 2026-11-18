# M3 Vulkan loader injection

`native/vk_inject/vk_inject.c` builds `libtitan_vk.so`, a loader-shaped shim that
Wine `dlopen`s through `TITAN_VULKAN_LIB`. It loads the stock
`/system/lib64/libvulkan.so` and swaps that library's `android_dlopen_ext` GOT
slot so the one vendor-ICD open becomes `TITAN_ICD`. Everything else stays the
platform loader: instance creation, extension filtering, Android WSI and the
swapchain implementation.

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
`/vendor/lib64/hw/vulkan.${ro.hardware.vulkan}.so`; on the SM-A556E
`ro.hardware.vulkan` is `samsung`, so it opens
`/vendor/lib64/hw/vulkan.samsung.so` with
`android_dlopen_ext(path, RTLD_NOW, &extinfo)` carrying the vendor namespace.

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
- The hook changes one pointer. Extension filtering, WSI, layer enumeration and
  perf-hint plumbing stay byte-identical to the vendor build.

## How the hook works

`libvulkan.so` imports `android_dlopen_ext` through its PLT (verified on the
device binary with `llvm-readelf -d/--dyn-syms`). On the first entrypoint call,
the shim:

1. `dlopen`s `/system/lib64/libvulkan.so` with `RTLD_NOW | RTLD_LOCAL`.
2. Finds the object through `dl_iterate_phdr`, walks `PT_DYNAMIC` for
   `DT_STRTAB`, `DT_SYMTAB`, `DT_JMPREL`/`DT_PLTRELSZ` (and `DT_RELA` for
   `-fno-plt` builds).
3. Rewrites the `R_AARCH64_JUMP_SLOT` (and `R_AARCH64_GLOB_DAT`) slots whose
   symbol is `android_dlopen_ext`. The page is `mprotect`ed read-write for the
   store and restored to the segment's protection, RELRO-aware.
4. Resolves the real `android_dlopen_ext` from `libdl.so` for pass-through.

The slot is written before the platform loader's first API call, so it does not
matter whether the loader is built `-z now` or lazily binds. The AOSP loader
opens its ICD lazily, on the first Vulkan call, which the shim's own entrypoints
precede.

The replacement matches `/vendor/lib64/hw/vulkan.*.so`. For that path it opens
`TITAN_ICD` with plain `dlopen` in the caller's namespace; the platform loader's
vendor namespace cannot see `/data/app/.../lib/arm64-v8a`. Every other path,
including layers, goes to the real `android_dlopen_ext` with its original
arguments.

## Fallback

The shim never makes Vulkan worse than stock:

- `TITAN_ICD` unset, not absolute, unreadable, or the `dlopen` fails: log the
  reason and call the real `android_dlopen_ext`. The stock vendor ICD loads and
  the driver works as before; only `device.0.name` stops containing `RADV`.
- Platform loader missing, `android_dlopen_ext` unresolvable, or no matching
  relocation: log the reason and forward every entrypoint unchanged. No partial
  install is reported as success; `titan_vk_inject_state()` leaves the hook bit
  clear.
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

- `/system/lib64/libvulkan.so` is the public platform loader and imports
  `android_dlopen_ext` through a relocation. Verified on the SM-A556E binary;
  the hook fails closed if the relocation disappears on another build.
- The vendor path built from `ro.hardware.vulkan` matches
  `/vendor/lib64/hw/vulkan.<name>.so`. The shim matches the prefix and the `.so`
  suffix, not the vendor name.
- The platform loader opens its ICD on the first API call, after the shim's
  entrypoints ran. If some Android release opened the ICD at library load time,
  the hook would be too late and the test would report the vendor device.
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
- `inject.ext.VK_KHR_surface=1`, `inject.ext.VK_KHR_android_surface=1`,
  `inject.ext.VK_KHR_swapchain=1`

`logcat -s titan_vk` shows the hook and the substitution with both absolute
paths.

Not covered by this artifact: surface creation (it needs a real `ANativeWindow`),
swapchain acquire/present, DXVK/VKD3D, and the Wine side of
`VK_KHR_android_surface`. The test stops at physical-device enumeration and
device-extension enumeration.
