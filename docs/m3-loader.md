# M3 Vulkan loader

The loader is the Khronos `Vulkan-Loader` pinned in `docs/upstream_manifest.md`
(`vulkan-sdk-1.4.363.0`, `fa218055ff4948b159f31d43f800c808a13d51df`), built for
`aarch64-linux-android34` with the Android WSI path enabled. It replaces the
AOSP `libvulkan.so` for the TITAN runtime.

## Why the AOSP loader cannot see the ICD

AOSP's `vulkan/libvulkan` enumerates `/vendor/lib64/hw/vulkan.*.so` and selects
the vendor library from `ro.hardware.vulkan`. It has no `VK_ICD_FILENAMES` or
`VK_DRIVER_FILES` handling, so an unrooted app cannot point it at an
app-private `vulkan.radeon.so`. The `radv-xclipse` package is an
adrenotools-style custom driver (`meta.json`, `vulkan.radeon.so`, `minApi: 34`),
not an AOSP ICD in `/vendor`; the platform loader therefore never sees it.

The Khronos loader reads `VK_DRIVER_FILES` first and `VK_ICD_FILENAMES` second.
The override replaces the normal search path, so the loader does not also scan
`/vendor/lib64/hw`. It parses the JSON manifest, resolves `library_path` (the
runner writes an absolute path), `dlopen`s the ICD, and negotiates the
loader/ICD interface version.

Upstream keeps the Android WSI implementation but `CMakeLists.txt` rejects
`ANDROID` with `message(FATAL_ERROR "Android build not supported!")` since
`2896ff0b6075`. `deps/patches/vulkan-loader/0001-cmake-android-wsi.patch`
restores it behind `BUILD_WSI_ANDROID_SUPPORT` (default `ON`), which defines
`VK_USE_PLATFORM_ANDROID_KHR` for the loader. That exports
`vkCreateAndroidSurfaceKHR` and keeps `VK_KHR_surface` and
`VK_KHR_android_surface` in the loader's known-extension filter.

The filter is not an advertisement: `vkEnumerateInstanceExtensionProperties`
merges the extensions returned by each ICD and drops names outside
`LOADER_INSTANCE_EXTENSIONS`. If `vulkan.radeon.so` does not return
`VK_KHR_surface`/`VK_KHR_android_surface`, the loader cannot invent them. The
loader also does not replace the platform loader's `VK_KHR_swapchain`
implementation; RADV's Android path expects the platform loader to drive
`VK_ANDROID_native_buffer` (`src/amd/vulkan/radv_android.c`). The test records
both surface facts so this boundary is visible instead of assumed.

## The shim

`deps/patches/wine/0015-win32u-titan-vulkan-lib.patch` is the shim.
`win32u` normally `dlopen`s `SONAME_LIBVULKAN`, which resolves to the AOSP
loader. The patch makes it read `TITAN_VULKAN_LIB` and requires an absolute path;
a relative value logs an `ERR` and leaves Vulkan disabled. `winevulkan.so` links
`-lwin32u`, so the same override covers both the Win32 Vulkan surface path and
the Wine Vulkan ICD entry points. No other Wine module needs to know the loader
location.

The loader is not a second WSI implementation. It creates and owns the loader
side `VkIcdSurfaceAndroid`, but presentation still needs the ICD's surface and
swapchain support. `tests/vk/loader_run.sh` treats the two
`loader.ext.VK_KHR_*` facts as diagnostics for that requirement.

## Environment contract

| Variable | Required | Value |
| --- | --- | --- |
| `TITAN_VULKAN_LIB` | yes, for Wine | absolute path to the shipped `libvulkan.so` |
| `VK_ICD_FILENAMES` | yes, unless `VK_DRIVER_FILES` is set | absolute path to the ICD manifest JSON |
| `VK_DRIVER_FILES` | optional | absolute path to the ICD manifest JSON; wins over `VK_ICD_FILENAMES` |
| `LD_LIBRARY_PATH` | runner only | directory containing `libvulkan.so` and `vulkan.radeon.so` |
| `VK_LOADER_DEBUG` | optional | `error,warn,driver` for the loader log |

The APK must export `TITAN_VULKAN_LIB`, `VK_ICD_FILENAMES` (or
`VK_DRIVER_FILES`) and `LD_LIBRARY_PATH` into the Wine process before `win32u`
initializes Vulkan. `TITAN_VULKAN_LIB` and the manifest's `library_path` are
absolute because the loader's `dlopen` does not search the working directory
and the app's current directory is not stable.

`tests/vk/loader_run.sh` writes the manifest at runtime, never in the package:

```json
{"file_format_version":"1.0.0","ICD":{"library_path":"<dir>/vulkan.radeon.so","api_version":"1.4.368"}}
```

The pinned ICD reports Vulkan 1.4.368; the loader is 1.4.363. The loader accepts
a newer ICD patch version and uses the manifest's `api_version` to decide
whether the ICD supports Vulkan 1.1 entry points.

## Build and artifact

`deps/vulkan-loader.sh` configures the pinned source with the NDK toolchain,
`ANDROID_STL=none`, release mode, tests off, and the TITAN code-generation flags
`-ffixed-x18 -fno-sanitize=shadow-call-stack -march=armv8.2-a+lse`; the shared
linker gets `-Wl,-z,max-page-size=16384`. It strips `libvulkan.so`, scans the
disassembly for `x18`/`w18`, checks the AArch64 dynamic ELF, checks the exported
entry points, and fails on any `NEEDED` outside `libdl.so libm.so libc.so`. The
loader adds no vendor library.

`.github/workflows/ci-m3-loader.yml` installs the pinned Vulkan-Headers,
applies the loader patch, builds the loader and `vk_loader_test`, downloads the
`radv-xclipse` artifact from `ci-deps-gpu`, and packages:

```text
titan-m3-loader.tar.gz
  libvulkan.so
  vk_loader_test
  vulkan.radeon.so
  run.sh
  meta.json
```

`meta.json` records the loader/header pins, the loader's `NEEDED` list, the
SHA-256 of each ELF, the `ci-deps-gpu` run id and head SHA, the build commit,
the NDK version and the build time. It contains no ICD JSON.

The APK must place the ELF files under `lib/arm64-v8a/` (or another directory
visible to its linker namespace); a modern `targetSdk` blocks `dlopen` of
app-data ELFs. The Termux runner can use an app-private directory because its
`targetSdk` is low enough.

## Test and pass criteria

`native/tools/vk_loader_test/vk_loader_test.c` is an aarch64 PIE started through
`/system/bin/linker64`. It does not link `libvulkan` and does not set any
environment variable. It `dlopen`s the loader from its argument, gets
`vkGetInstanceProcAddr`, creates an instance (trying Vulkan 1.4, 1.3, 1.2, 1.1),
enumerates physical devices, and prints the same `device.*` facts as
`vk_probe` plus:

```text
RESULT loader.ext.VK_KHR_surface=1|0
RESULT loader.ext.VK_KHR_android_surface=1|0
```

A run passes when `loader_run.sh` sees:

- exit status 0 and `RESULT loader.exit=0`
- `RESULT loader.dlopen=ok`
- `RESULT loader.instance=ok`
- `RESULT loader.physical_devices=<n>` with `n >= 1`
- a non-empty `RESULT device.0.name`
- both `loader.ext.VK_KHR_surface` and `loader.ext.VK_KHR_android_surface`
  present as `0` or `1`
- no `RESULT loader.error=`

The extension facts do not fail the loader test by themselves. A `0` is a
finding: the loader is proven to read the manifest, load the ICD and enumerate
the device, but the Wine Android-WSI gate is not met until the ICD advertises
the extension. The M3 gate requires both facts to be `1`.

Run it after extracting the artifact:

```sh
tar -xzf titan-m3-loader.tar.gz -C /data/data/com.termux/files/home/m3-loader
/data/data/com.termux/files/home/m3-loader/run.sh
```

The log is `vk-loader.log` next to the extracted files.

## Not proven by this artifact

The CI job builds and packages, but does not execute the loader on a device.
The on-device run is the first proof that the AOSP namespace allows the
app-private `dlopen`, that the pinned ICD's `vk_icdNegotiateLoaderICDInterfaceVersion`
matches the loader, and whether `vulkan.radeon.so` advertises
`VK_KHR_surface`/`VK_KHR_android_surface`. A missing surface extension is an
ICD/platform-loader gap, not a loader build failure.
