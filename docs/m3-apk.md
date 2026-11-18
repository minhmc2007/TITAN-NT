# M3 test APK

`app/` is a single-module Gradle project that packages the M3 Wine runtime and
`wineandroid.drv` into a debug APK. It is Java only, has no AndroidX or Kotlin
dependency, and is built only in CI (`ci-m3-apk.yml`). The asset tree is
generated from published dependency artifacts and is never committed. The
launcher label is `TITAN NT`; the package id stays `dev.titan.nt` because
`WineActivity`'s JNI contract and the app-data path both depend on it.

## Why `WineActivity` and `targetSdk 28`

`dlls/ntdll/unix/loader.c` hard-codes `WINE_JAVA_CLASS` as
`org/winehq/wine/WineActivity` and registers `wine_init` on that class in
`JNI_OnLoad`. `wineandroid.drv` registers `wine_desktop_changed`,
`wine_config_changed`, `wine_surface_changed`, `wine_motion_event` and
`wine_keyboard_event` on the same class at unixlib init. Any other activity
would not receive those natives, so `app/src/main/java/org/winehq/wine/WineActivity.java`
is Wine's reference host with only the asset paths, the optional
`TITAN_VULKAN_LIB` environment entry and the launch-target selection changed.

`targetSdk 28` is deliberate: API 29+ SELinux W^X blocks `dlopen` and
file-backed executable mappings of files under app data, which is where Wine's
unix libraries and PE files live. Raising it requires moving every ELF file
into `lib/<abi>/` and replacing the file-backed PE mapping path first.

## Asset layout

```
app/src/main/assets/
├── files.sum                       sha256 of every asset, relative to assets/
├── sums.sum                        sha256 of files.sum
└── wine/
    ├── arm64-v8a/                  Wine install tree + FEX overlay
    │   ├── bin/wine                loader copy (see below)
    │   ├── bin/wineserver
    │   ├── lib/wine/aarch64-unix/  ntdll.so, win32u.so, wineandroid.so, winevulkan.so,
    │   │                           libarm64ecfex.so, libwow64fex.so, ...
    │   ├── lib/wine/aarch64-windows/  Wine builtins + wineandroid.drv + FEX modules
    │   │                           + DXVK d3d11/dxgi + VKD3D d3d12/d3d12core
    │   ├── lib/wine/i386-windows/  WoW64 builtins, required by wineboot
    │   └── share/wine/            wine.inf, nls/, winmd/
    └── lib/titan/
        ├── vulkan.radeon.so       radv-xclipse ICD
        ├── libvulkan.so           platform-loader injection shim
        └── meta.json
```

`WineActivity.copyAssetFiles()` extracts the tree to `getFilesDir()` on first
start and on every asset change, so the device layout is
`<filesDir>/wine/arm64-v8a/...` and `<filesDir>/wine/lib/titan/...`. The
`files.sum`/`sums.sum` pair is the incremental-extraction contract: it must
list every copied asset and must not list itself.

`isFileWanted()` accepts the `wine/` prefix; `isFileExecutable()` marks
everything outside `wine/arm64-v8a/share/` executable because `wineserver`,
the loader and the unix libraries are exec'd from app data.

The install tree ships the loader as `lib/wine/aarch64-unix/wine` and leaves
`bin/*` as dangling symlinks to a missing `bin/wine`. The assembly step drops
the symlinks and copies the loader to `bin/wine`, which is what the reference
activity passes as `WINELOADER`.

## Environment

| Variable | Value |
| --- | --- |
| `WINELOADER` | `<filesDir>/wine/arm64-v8a/bin/wine` |
| `WINEPREFIX` | `<filesDir>/prefix` |
| `WINEDLLPATH` | `<filesDir>/wine/arm64-v8a/lib/wine` |
| `LD_LIBRARY_PATH` | `<filesDir>/wine/arm64-v8a/lib:<nativeLibraryDir>` |
| `TITAN_VULKAN_LIB` | `<filesDir>/wine/lib/titan/libvulkan.so`, only when that file exists |
| `TITAN_ICD` | `<filesDir>/wine/lib/titan/vulkan.radeon.so`, only when that file exists |

`wine/lib/titan/libvulkan.so` is the injection shim from `ci-m3-inject`
(`native/vk_inject`). It loads the platform `/system/lib64/libvulkan.so` and
redirects the one vendor-ICD open to `TITAN_ICD`, so the device loader's
Android WSI (`VK_KHR_android_surface`, `VK_KHR_swapchain`) drives radv instead
of the Samsung driver. The ICD path has to be absolute because the shim opens
it with plain `dlopen` in the app namespace; the loader's `sphal` namespace
cannot see app data. Both variables are set from files, never from
`VK_ICD_FILENAMES` or `LD_PRELOAD`.

The launched command is the fourth `explorer.exe /desktop=shell,,android`
argument. It comes from the `cmdline` string extra on the launch intent, else
the first line of `<filesDir>/cmdline`, else the Wine default
(`c:\windows\system32\wineconsole.exe`). The debug build is `run-as`-able, so
the file is writable without root.

## Build

```sh
gh workflow run ci-m3-apk.yml --ref m3/apk \
  -f cpu_run_id=<ci-deps-cpu run id> \
  -f gpu_run_id=<ci-deps-gpu run id> \
  -f inject_run_id=<ci-m3-inject run id>
gh run watch
gh run download <run id> -n titan-m3-apk
```

A push that touches `app/**` or the workflow also builds, with every run id
resolved from the newest successful run of its workflow.

The verified artifact is 216,682,696 bytes (207 MiB); `unzip -l` reports
952,833,480 bytes uncompressed across 1671 entries, built by run 44311263044
from `ci-deps-cpu` run 44055288694 and `ci-deps-gpu` run 44058448003.

The job unpacks `wine-aarch64` and `fex-aarch64` from the CPU run,
`radv-xclipse`, `dxvk-arm64ec` and `vkd3d-arm64x` from the GPU run and
`titan-m3-inject` from the inject run, merges the FEX overlay the same way
`ci-m2-win.yml` does, installs Gradle 8.7 and AGP 8.5.2 (JDK 17, no Android
Studio), builds `assembleDebug`, checks the required asset entries and
`targetSdkVersion:'28'` with `aapt2`, then uploads `titan-m3.apk`. The inject
run resolves by run name because `ci-m3-inject.yml` is not on the default
branch yet.

## Safe trims

| Dropped | Size (raw) | Why |
| --- | --- | --- |
| `include/` | ~65 MB | development headers, no runtime consumer |
| `*.a` import libraries | ~33 MB | link-time only, both PE arches |
| dangling `bin/*` symlinks | ~0 | point at a `bin/wine` the Android install omits |
| FEX `bin/FEXOfflineCompiler*.exe` | ~7 MB | not copied; the overlay only needs the four modules |

`lib/wine/i386-windows` (196 MB raw) stays: `wineboot` populates `syswow64`
from it even when no 32-bit application is launched. `share/` (11 MB) stays:
`wine.inf` and the NLS tables are required to create a prefix.

## Install and launch

```sh
adb install -r titan-m3.apk

# default command (wineconsole inside the Android desktop)
adb shell am start -n dev.titan.nt/org.winehq.wine.WineActivity

# choose a different program; singleInstance means force-stop first
adb shell am force-stop dev.titan.nt
adb shell am start -n dev.titan.nt/org.winehq.wine.WineActivity \
  --es cmdline 'c:\windows\system32\wineconsole.exe'

adb logcat -s wine:V
```

The first start extracts roughly 1 GB into
`/data/user/0/dev.titan.nt/files/` and creates the prefix under
`files/prefix`; it needs that much free space and several minutes. A failed
launch leaves the extraction progress dialog up, and the reason is in the
`wine` logcat tag.

## First device launch (Android 17, SM-A556E)

Evidence: `docs/evidence/m3-apk-first-launch.log`.

The Wine side starts: `ntdll.so` loads from app data through `linker64` (with
the API 37 warning `Attempt to load writable file`), `start_server` spawns the
server, `WineActivity` and the `Wine` progress dialog each draw a first frame
with the Samsung Vulkan render engine, and `explorer.exe` begins its display
init. Then the process aborts:

```
V XGL  : Entering: Create                                  (thread 5240, Wine)
V XGL  : Samsung::Vulkan::OpenDevice                       (threads 5295, 5302)
D dev.titan.nt: [set] Decode return value is -2            (thread 5235, HWUI render)
F libc : FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x729f37649c)
```

This build had no shim, so `TITAN_VULKAN_LIB` was unset and Wine's win32u
display init enumerated GPUs through the platform loader, opening two more
`vulkan.samsung.so` devices inside a process whose HWUI already owns one. The
aborting thread is HWUI's render thread and the mutex is a bionic mutex, so the
fault is in process-local state, not in Wine's PE side.

Not root-caused. The tombstone is required before claiming a cause:

```sh
adb shell dumpsys dropbox --print data_app_native_crash
adb logcat -b crash -d
```

The next launch uses the shim + radv (`TITAN_ICD`), which removes the
second-client overlap on the vendor driver entirely: Wine's Vulkan calls then
land on radv, and the Samsung driver keeps only HWUI as a client.

## Not verified in CI

- `targetSdk 28` is what allows `dlopen` and file-backed exec from app data;
  only a device run can confirm the A55's SELinux policy and the loader path
  actually permit it.
- `wineboot` creating the prefix and `explorer.exe /desktop=shell,,android`
  attaching the desktop are runtime-only.
- There is no Vulkan ICD in the path yet. With no `libvulkan.so` shim the
  activity leaves `TITAN_VULKAN_LIB` unset, Wine uses the platform loader and
  the Xclipse driver is not selected; D3D12/D3D11 can boot but will not get a
  RADV device until the loader workstream installs
  `wine/lib/titan/libvulkan.so`. The APK does not bundle a substitute.

