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
vendor/device equal an ICD `vendorID/deviceID`.

A pass is exit 0 with `RESULT probe.exit=0`, `RESULT probe.dlopen=ok`,
`RESULT probe.instance=ok`, a non-empty `RESULT device.0.name`,
`RESULT x18_clobbers=0` and `RESULT drm.readable=1`. The node with `matches_vk=1` is the
Xclipse 530; if sysfs has no PCI IDs, the raw `uevent` driver line identifies it. This proves
instance creation and physical-device enumeration only: no `VkDevice`, no rendering, no
DXVK/VKD3D.
