# M3 Vulkan ICD probe

`native/tools/vk_probe/vk_probe.c` is an aarch64 Android executable that `dlopen`s
`libvulkan_radeon.so` and drives the ICD through `vk_icdGetInstanceProcAddr`, bypassing the
Android system loader. It negotiates `vk_icdNegotiateLoaderICDInterfaceVersion`, tries Vulkan
1.3, 1.2 then 1.1, and prints one `RESULT key=value` line per fact.

The x18 canary reads x18 before and after every ICD entry point. Any change prints
`RESULT x18_clobber=<entry>` and exits 3, because x18 carries the Windows TEB in the ARM64EC
Wine process. A clean run prints `RESULT x18_clobbers=0`.

For each physical device the probe prints name, type, API/driver versions, driver name and
info, queue families, memory heaps/types and `VK_EXT_memory_budget` values when exposed.
`VK_EXT_pci_bus_info` is printed when advertised. Render nodes are identified from raw
`/sys/class/drm/renderD{128,129}/device/{uevent,vendor,device}` values, a read-only `/dev/dri`
open with `errno`, and a direct `DRM_IOCTL_VERSION`; `matches_vk=1` means the node's
vendor/device equal an ICD `vendorID/deviceID`, or, when the GPU is a platform device with no
PCI ids, that the node's DRM driver is the one the Vulkan driver sits on (RADV: `amdgpu`).

A pass is exit 0 with `RESULT probe.exit=0`, `RESULT probe.dlopen=ok`,
`RESULT probe.instance=ok`, a non-empty `RESULT device.0.name`,
`RESULT x18_clobbers=0` and `RESULT drm.readable=1`. This proves instance creation and
physical-device enumeration only: no `VkDevice`, no rendering, no DXVK/VKD3D.

## A55 result (2026-11-17)

`tests/vk/run.sh` passes on the SM-A556E from a Termux app process, 33 x18 checks, 0
clobbers. The ICD is driven directly, so the Android loader and root are both out of the
path.

| Fact | Value |
| --- | --- |
| `device.0.name` | `AMD Radeon Graphics (RADV TITAN)` |
| `device.0.driver_info` | `Mesa 27.0.0-devel (git-cc87fdf712)` |
| `device.0.vendor_id` / `device_id` | `0x144d` / `0x73a0` |
| `device.0.api_version` | `1.4.368` |
| queue families | 3 (1 graphics+compute, 3 compute, 1 transfer-only) |
| memory | 2 heaps, 9 types, heap 0 = 5,810,323,456 B |
| `renderD128` | `DRIVER=sgpu`, `OF_COMPATIBLE_0=samsung-sgpu,samsung-sgpu`, DRM `amdgpu` 3.42.0 |
| `renderD129` | `DRIVER=exynos-drm`, DRM `exynos-drmdpu` 1.0.0 |

`renderD128` is the Xclipse 530; `renderD129` is the display controller. The raw log is
`probe-out/m3-vk-probe.log`.

`linker64` rejects relative program paths and `dlopen` does not search the working directory,
so the runner passes absolute paths for both the probe and the ICD.
