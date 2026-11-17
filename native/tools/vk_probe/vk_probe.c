#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

/* The Android loader only sees system ICDs, so custom drivers are driven
 * through the loader/ICD ABI directly. */
typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_vk_icdGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult(VKAPI_PTR *PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t *);

struct vk_api {
    PFN_vkCreateInstance create_instance;
    PFN_vkEnumerateInstanceExtensionProperties enum_instance_ext;
    PFN_vkEnumeratePhysicalDevices enum_devices;
    PFN_vkGetPhysicalDeviceProperties get_props;
    PFN_vkGetPhysicalDeviceFeatures get_features;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queues;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory;
    PFN_vkGetPhysicalDeviceProperties2 get_props2;
    PFN_vkGetPhysicalDeviceMemoryProperties2 get_memory2;
    PFN_vkEnumerateDeviceExtensionProperties enum_dev_ext;
    PFN_vkDestroyInstance destroy_instance;
};

static struct vk_api api;
static PFN_vk_icdGetInstanceProcAddr icd_gipa;
static unsigned x18_checks;
static unsigned failures;
static unsigned drm_facts;
static struct {
    uint32_t vendor;
    uint32_t device;
    int driver_id;
} vk_ids[8];
static unsigned vk_id_count;

static void res_fmt(const char *key, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("RESULT %s=", key);
    for (const char *p = buf; *p; p++)
        putchar(*p == '\n' || *p == '\r' ? ';' : *p);
    putchar('\n');
}

static inline uint64_t x18_read(void) {
    uint64_t v;
    asm volatile("mov %0, x18" : "=r"(v));
    return v;
}

static void x18_check(const char *where, uint64_t before) {
    uint64_t after = x18_read();
    x18_checks++;
    if (after == before)
        return;

    res_fmt("x18_clobber", "%s", where);
    res_fmt("x18_before", "0x%llx", (unsigned long long)before);
    res_fmt("x18_after", "0x%llx", (unsigned long long)after);
    res_fmt("x18_clobbers", "1");
    res_fmt("probe.exit", "3");
    fflush(stdout);
    _exit(3);
}

#define X18_CALL(name, expr)                                                                       \
    __extension__({                                                                                \
        uint64_t _x18 = x18_read();                                                                \
        __typeof__(expr) _ret = (expr);                                                            \
        x18_check((name), _x18);                                                                   \
        _ret;                                                                                      \
    })

#define X18_VOID(name, expr)                                                                       \
    do {                                                                                           \
        uint64_t _x18 = x18_read();                                                                \
        (void)(expr);                                                                              \
        x18_check((name), _x18);                                                                   \
    } while (0)

static PFN_vkVoidFunction get_proc(VkInstance instance, const char *name) {
    uint64_t before = x18_read();
    PFN_vkVoidFunction fn = icd_gipa(instance, name);
    x18_check("vk_icdGetInstanceProcAddr", before);
    return fn;
}

static void entry_missing(const char *name) {
    char key[128];
    snprintf(key, sizeof(key), "probe.entry.%s", name);
    res_fmt(key, "missing");
    failures++;
}

static int finish(int code) {
    res_fmt("x18_checks", "%u", x18_checks);
    res_fmt("x18_clobbers", "0");
    res_fmt("probe.failures", "%u", failures);
    res_fmt("probe.exit", "%d", code);
    return code;
}

static const char *device_type_name(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "other";
    }
}

static void res_fixed(const char *key, const char *s, size_t n) {
    char buf[512];
    size_t len = strnlen(s, n);
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    res_fmt(key, "%s", buf);
}

static int has_ext(const VkExtensionProperties *exts, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, name) == 0)
            return 1;
    }
    return 0;
}

static uint32_t enumerate_instance_ext(VkExtensionProperties **out) {
    *out = NULL;
    if (!api.enum_instance_ext)
        return 0;

    uint32_t count = 0;
    VkResult r = X18_CALL("vkEnumerateInstanceExtensionProperties",
                          api.enum_instance_ext(NULL, &count, NULL));
    if (r != VK_SUCCESS) {
        res_fmt("probe.instance_extensions_result", "%d", (int)r);
        return 0;
    }
    if (count == 0)
        return 0;

    VkExtensionProperties *exts = calloc(count, sizeof(*exts));
    if (!exts) {
        failures++;
        return 0;
    }

    r = X18_CALL("vkEnumerateInstanceExtensionProperties",
                 api.enum_instance_ext(NULL, &count, exts));
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        res_fmt("probe.instance_extensions_result", "%d", (int)r);
        free(exts);
        return 0;
    }

    *out = exts;
    return count;
}

static VkInstance create_instance(uint32_t *chosen) {
    VkExtensionProperties *exts = NULL;
    uint32_t ext_count = enumerate_instance_ext(&exts);
    res_fmt("probe.instance_extension_count", "%u", ext_count);

    const char *enabled[1];
    uint32_t enabled_count = 0;
    int have_props2_ext =
        has_ext(exts, ext_count, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (have_props2_ext)
        enabled[enabled_count++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    res_fmt("probe.ext.get_physical_device_properties2", "%d", have_props2_ext);

    const uint32_t versions[] = {
        VK_API_VERSION_1_3,
        VK_API_VERSION_1_2,
        VK_API_VERSION_1_1,
    };
    VkInstance instance = VK_NULL_HANDLE;

    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        VkApplicationInfo app;
        memset(&app, 0, sizeof(app));
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "titan-m3-vk-probe";
        app.applicationVersion = 1;
        app.pEngineName = "titan";
        app.engineVersion = 1;
        app.apiVersion = versions[i];

        VkInstanceCreateInfo info;
        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = enabled_count;
        info.ppEnabledExtensionNames = enabled_count ? enabled : NULL;

        VkResult r = X18_CALL("vkCreateInstance", api.create_instance(&info, NULL, &instance));
        char key[64];
        snprintf(key, sizeof(key), "probe.instance.%u.%u", VK_API_VERSION_MAJOR(versions[i]),
                 VK_API_VERSION_MINOR(versions[i]));
        res_fmt(key, "%s", r == VK_SUCCESS ? "ok" : "fail");
        snprintf(key, sizeof(key), "probe.instance.%u.%u.result", VK_API_VERSION_MAJOR(versions[i]),
                 VK_API_VERSION_MINOR(versions[i]));
        res_fmt(key, "%d", (int)r);

        if (r == VK_SUCCESS) {
            *chosen = versions[i];
            break;
        }
    }

    free(exts);
    return instance;
}

static void probe_device(VkPhysicalDevice dev, uint32_t index) {
    char key[192];
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));

    X18_VOID("vkGetPhysicalDeviceProperties", api.get_props(dev, &props));

    if (vk_id_count < sizeof(vk_ids) / sizeof(vk_ids[0])) {
        vk_ids[vk_id_count].vendor = props.vendorID;
        vk_ids[vk_id_count].device = props.deviceID;
        vk_ids[vk_id_count].driver_id = -1;
        vk_id_count++;
    }

    snprintf(key, sizeof(key), "device.%u.name", index);
    res_fixed(key, props.deviceName, sizeof(props.deviceName));
    snprintf(key, sizeof(key), "device.%u.type", index);
    res_fmt(key, "%d", (int)props.deviceType);
    snprintf(key, sizeof(key), "device.%u.type_name", index);
    res_fmt(key, "%s", device_type_name(props.deviceType));
    snprintf(key, sizeof(key), "device.%u.api_version", index);
    res_fmt(key, "%u.%u.%u", VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    snprintf(key, sizeof(key), "device.%u.api_version_raw", index);
    res_fmt(key, "0x%08x", props.apiVersion);
    snprintf(key, sizeof(key), "device.%u.driver_version", index);
    res_fmt(key, "%u.%u.%u", VK_VERSION_MAJOR(props.driverVersion),
            VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
    snprintf(key, sizeof(key), "device.%u.driver_version_raw", index);
    res_fmt(key, "0x%08x", props.driverVersion);
    snprintf(key, sizeof(key), "device.%u.vendor_id", index);
    res_fmt(key, "0x%04x", props.vendorID);
    snprintf(key, sizeof(key), "device.%u.device_id", index);
    res_fmt(key, "0x%04x", props.deviceID);

    VkPhysicalDeviceFeatures features;
    memset(&features, 0, sizeof(features));
    X18_VOID("vkGetPhysicalDeviceFeatures", api.get_features(dev, &features));
    snprintf(key, sizeof(key), "device.%u.features", index);
    res_fmt(key, "ok");

    uint32_t qcount = 0;
    X18_VOID("vkGetPhysicalDeviceQueueFamilyProperties", api.get_queues(dev, &qcount, NULL));
    snprintf(key, sizeof(key), "device.%u.queue_families_result", index);
    res_fmt(key, "ok");

    VkQueueFamilyProperties *queues = NULL;
    if (qcount) {
        queues = calloc(qcount, sizeof(*queues));
        if (!queues) {
            failures++;
            qcount = 0;
        }
    }
    if (qcount) {
        X18_VOID("vkGetPhysicalDeviceQueueFamilyProperties",
                 api.get_queues(dev, &qcount, queues));
    }
    snprintf(key, sizeof(key), "device.%u.queue_families", index);
    res_fmt(key, "%u", qcount);

    for (uint32_t i = 0; i < qcount; i++) {
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.flags", index, i);
        res_fmt(key, "0x%x", queues[i].queueFlags);
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.queue_count", index, i);
        res_fmt(key, "%u", queues[i].queueCount);
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.graphics", index, i);
        res_fmt(key, "%d", !!(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT));
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.compute", index, i);
        res_fmt(key, "%d", !!(queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT));
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.transfer", index, i);
        res_fmt(key, "%d", !!(queues[i].queueFlags & VK_QUEUE_TRANSFER_BIT));
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.sparse_binding", index, i);
        res_fmt(key, "%d", !!(queues[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT));
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.timestamp_bits", index, i);
        res_fmt(key, "%u", queues[i].timestampValidBits);
        snprintf(key, sizeof(key), "device.%u.queue_family.%u.min_transfer_granularity", index, i);
        res_fmt(key, "%ux%ux%u", queues[i].minImageTransferGranularity.width,
                queues[i].minImageTransferGranularity.height,
                queues[i].minImageTransferGranularity.depth);
    }
    free(queues);

    VkPhysicalDeviceMemoryProperties mem;
    memset(&mem, 0, sizeof(mem));
    X18_VOID("vkGetPhysicalDeviceMemoryProperties", api.get_memory(dev, &mem));
    snprintf(key, sizeof(key), "device.%u.memory_properties", index);
    res_fmt(key, "ok");

    snprintf(key, sizeof(key), "device.%u.memory_heaps", index);
    res_fmt(key, "%u", mem.memoryHeapCount);
    snprintf(key, sizeof(key), "device.%u.memory_types", index);
    res_fmt(key, "%u", mem.memoryTypeCount);

    for (uint32_t i = 0; i < mem.memoryHeapCount; i++) {
        snprintf(key, sizeof(key), "device.%u.heap.%u.size", index, i);
        res_fmt(key, "%llu", (unsigned long long)mem.memoryHeaps[i].size);
        snprintf(key, sizeof(key), "device.%u.heap.%u.flags", index, i);
        res_fmt(key, "0x%x", mem.memoryHeaps[i].flags);
        snprintf(key, sizeof(key), "device.%u.heap.%u.device_local", index, i);
        res_fmt(key, "%d", !!(mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT));
    }
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        snprintf(key, sizeof(key), "device.%u.memory_type.%u.flags", index, i);
        res_fmt(key, "0x%x", mem.memoryTypes[i].propertyFlags);
        snprintf(key, sizeof(key), "device.%u.memory_type.%u.heap", index, i);
        res_fmt(key, "%u", mem.memoryTypes[i].heapIndex);
    }

    uint32_t ext_count = 0;
    VkResult r = X18_CALL("vkEnumerateDeviceExtensionProperties",
                 api.enum_dev_ext(dev, NULL, &ext_count, NULL));
    snprintf(key, sizeof(key), "device.%u.device_extensions_result", index);
    res_fmt(key, "%d", (int)r);
    if (r != VK_SUCCESS) {
        failures++;
        ext_count = 0;
    }

    VkExtensionProperties *exts = NULL;
    if (ext_count) {
        exts = calloc(ext_count, sizeof(*exts));
        if (!exts) {
            failures++;
            ext_count = 0;
        }
    }
    if (ext_count) {
        r = X18_CALL("vkEnumerateDeviceExtensionProperties",
                     api.enum_dev_ext(dev, NULL, &ext_count, exts));
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
            failures++;
            ext_count = 0;
        }
    }
    snprintf(key, sizeof(key), "device.%u.device_extensions", index);
    res_fmt(key, "%u", ext_count);

    int has_budget = has_ext(exts, ext_count, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    int has_pci = has_ext(exts, ext_count, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    int has_driver = has_ext(exts, ext_count, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    snprintf(key, sizeof(key), "device.%u.ext.memory_budget", index);
    res_fmt(key, "%d", has_budget);
    snprintf(key, sizeof(key), "device.%u.ext.pci_bus_info", index);
    res_fmt(key, "%d", has_pci);
    snprintf(key, sizeof(key), "device.%u.ext.driver_properties", index);
    res_fmt(key, "%d", has_driver);

    if (!api.get_props2) {
        snprintf(key, sizeof(key), "device.%u.properties2", index);
        res_fmt(key, "missing");
        failures++;
    } else {
        VkPhysicalDeviceDriverProperties drv;
        memset(&drv, 0, sizeof(drv));
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        VkPhysicalDevicePCIBusInfoPropertiesEXT pci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;

        VkPhysicalDeviceProperties2 props2;
        memset(&props2, 0, sizeof(props2));
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

        int want_driver = has_driver || props.apiVersion >= VK_API_VERSION_1_2;
        if (want_driver)
            props2.pNext = &drv;
        if (has_pci) {
            pci.pNext = props2.pNext;
            props2.pNext = &pci;
        }

        X18_VOID("vkGetPhysicalDeviceProperties2", api.get_props2(dev, &props2));
        snprintf(key, sizeof(key), "device.%u.properties2", index);
        res_fmt(key, "ok");
        if (want_driver) {
            snprintf(key, sizeof(key), "device.%u.driver_name", index);
            res_fixed(key, drv.driverName, sizeof(drv.driverName));
            snprintf(key, sizeof(key), "device.%u.driver_info", index);
            res_fixed(key, drv.driverInfo, sizeof(drv.driverInfo));
            snprintf(key, sizeof(key), "device.%u.driver_id", index);
            res_fmt(key, "%d", (int)drv.driverID);
            if (index < vk_id_count) vk_ids[index].driver_id = (int)drv.driverID;
            snprintf(key, sizeof(key), "device.%u.driver_properties", index);
            res_fmt(key, "1");
        } else {
            snprintf(key, sizeof(key), "device.%u.driver_properties", index);
            res_fmt(key, "0");
        }

        if (has_pci) {
            snprintf(key, sizeof(key), "device.%u.pci.domain", index);
            res_fmt(key, "%u", pci.pciDomain);
            snprintf(key, sizeof(key), "device.%u.pci.bus", index);
            res_fmt(key, "%u", pci.pciBus);
            snprintf(key, sizeof(key), "device.%u.pci.device", index);
            res_fmt(key, "%u", pci.pciDevice);
            snprintf(key, sizeof(key), "device.%u.pci.function", index);
            res_fmt(key, "%u", pci.pciFunction);
            snprintf(key, sizeof(key), "device.%u.pci_bus_info", index);
            res_fmt(key, "1");
        } else {
            snprintf(key, sizeof(key), "device.%u.pci_bus_info", index);
            res_fmt(key, "0");
        }
    }

    if (has_budget && api.get_memory2) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
        memset(&budget, 0, sizeof(budget));
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        VkPhysicalDeviceMemoryProperties2 mem2;
        memset(&mem2, 0, sizeof(mem2));
        mem2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        mem2.pNext = &budget;

        X18_VOID("vkGetPhysicalDeviceMemoryProperties2", api.get_memory2(dev, &mem2));
        snprintf(key, sizeof(key), "device.%u.memory_budget", index);
        res_fmt(key, "ok");
        for (uint32_t i = 0; i < mem.memoryHeapCount; i++) {
            snprintf(key, sizeof(key), "device.%u.heap.%u.budget", index, i);
            res_fmt(key, "%llu", (unsigned long long)budget.heapBudget[i]);
            snprintf(key, sizeof(key), "device.%u.heap.%u.usage", index, i);
            res_fmt(key, "%llu", (unsigned long long)budget.heapUsage[i]);
        }
    } else {
        snprintf(key, sizeof(key), "device.%u.memory_budget", index);
        res_fmt(key, "0");
    }

    free(exts);
}

struct drm_version_ioctl {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

#define DRM_IOCTL_VERSION _IOWR('d', 0x00, struct drm_version_ioctl)

static int read_drm_attr(const char *node, const char *attr, char *buf, size_t cap) {
    char path[256];
    char key[160];
    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/%s", node, attr);
    snprintf(key, sizeof(key), "drm.%s.%s", node, attr);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        res_fmt(key, "unreadable");
        snprintf(key, sizeof(key), "drm.%s.%s.errno", node, attr);
        res_fmt(key, "%d", err);
        buf[0] = 0;
        return 0;
    }

    ssize_t n = read(fd, buf, cap - 1);
    int err = errno;
    close(fd);
    if (n < 0) {
        res_fmt(key, "unreadable");
        snprintf(key, sizeof(key), "drm.%s.%s.errno", node, attr);
        res_fmt(key, "%d", err);
        buf[0] = 0;
        return 0;
    }

    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    if (n > 0)
        drm_facts++;
    res_fmt(key, "%s", buf);
    return n > 0;
}

static void probe_drm_open(const char *node, char *driver, size_t driver_cap) {
    char path[64];
    char key[160];
    snprintf(path, sizeof(path), "/dev/dri/%s", node);

    errno = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int err = errno;
    snprintf(key, sizeof(key), "drm.%s.open", node);
    if (fd < 0) {
        res_fmt(key, "fail");
        snprintf(key, sizeof(key), "drm.%s.open_errno", node);
        res_fmt(key, "%d", err);
        return;
    }
    drm_facts++;
    res_fmt(key, "ok");

    struct drm_version_ioctl v;
    memset(&v, 0, sizeof(v));
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) != 0) {
        err = errno;
        snprintf(key, sizeof(key), "drm.%s.version_errno", node);
        res_fmt(key, "%d", err);
        close(fd);
        return;
    }

    char name[256] = {0};
    char date[256] = {0};
    char desc[512] = {0};
    v.name = name;
    v.name_len = sizeof(name);
    v.date = date;
    v.date_len = sizeof(date);
    v.desc = desc;
    v.desc_len = sizeof(desc);

    if (ioctl(fd, DRM_IOCTL_VERSION, &v) != 0) {
        err = errno;
        snprintf(key, sizeof(key), "drm.%s.version_errno", node);
        res_fmt(key, "%d", err);
        close(fd);
        return;
    }

    snprintf(key, sizeof(key), "drm.%s.version", node);
    res_fmt(key, "%d.%d.%d", v.version_major, v.version_minor, v.version_patchlevel);
    snprintf(key, sizeof(key), "drm.%s.driver_name", node);
    res_fmt(key, "%s", name);
    snprintf(driver, driver_cap, "%s", name);
    snprintf(key, sizeof(key), "drm.%s.driver_date", node);
    res_fmt(key, "%s", date);
    snprintf(key, sizeof(key), "drm.%s.driver_desc", node);
    res_fmt(key, "%s", desc);
    close(fd);
}

static void probe_drm_node(const char *node) {
    char uevent[1024] = {0};
    char vendor[64] = {0};
    char device[64] = {0};

    (void)read_drm_attr(node, "uevent", uevent, sizeof(uevent));
    int have_vendor_id = read_drm_attr(node, "vendor", vendor, sizeof(vendor));
    int have_device_id = read_drm_attr(node, "device", device, sizeof(device));

    char key[160];
    char drm_driver[256] = {0};

    probe_drm_open(node, drm_driver, sizeof(drm_driver));

    if (vk_id_count) {
        int match = 0;
        if (have_vendor_id && have_device_id) {
            unsigned long v = strtoul(vendor, NULL, 0);
            unsigned long d = strtoul(device, NULL, 0);
            for (unsigned i = 0; i < vk_id_count; i++) {
                if (v == vk_ids[i].vendor && d == vk_ids[i].device) {
                    match = 1;
                    break;
                }
            }
        }
        /* Platform GPUs expose no PCI ids; RADV always sits on the amdgpu
         * DRM driver, so the driver name is the identity that survives. */
        if (!match && drm_driver[0]) {
            for (unsigned i = 0; i < vk_id_count; i++) {
                if (vk_ids[i].driver_id == VK_DRIVER_ID_MESA_RADV &&
                    strcmp(drm_driver, "amdgpu") == 0) {
                    match = 1;
                    break;
                }
            }
        }
        snprintf(key, sizeof(key), "drm.%s.matches_vk", node);
        res_fmt(key, "%d", match);
    } else {
        snprintf(key, sizeof(key), "drm.%s.matches_vk", node);
        res_fmt(key, "unknown");
    }
}

static void probe_drm(void) {
    probe_drm_node("renderD128");
    probe_drm_node("renderD129");
    res_fmt("drm.facts", "%u", drm_facts);
    if (drm_facts == 0) {
        res_fmt("drm.readable", "0");
        failures++;
    } else {
        res_fmt("drm.readable", "1");
    }
}

static void load_api(VkInstance instance) {
#define LOAD(field, name)                                                                          \
    do {                                                                                           \
        api.field = (__typeof__(api.field))get_proc(instance, name);                               \
        if (!api.field)                                                                            \
            entry_missing(name);                                                                   \
    } while (0)

    LOAD(enum_devices, "vkEnumeratePhysicalDevices");
    LOAD(get_props, "vkGetPhysicalDeviceProperties");
    LOAD(get_features, "vkGetPhysicalDeviceFeatures");
    LOAD(get_queues, "vkGetPhysicalDeviceQueueFamilyProperties");
    LOAD(get_memory, "vkGetPhysicalDeviceMemoryProperties");
    LOAD(get_props2, "vkGetPhysicalDeviceProperties2");
    LOAD(get_memory2, "vkGetPhysicalDeviceMemoryProperties2");
    LOAD(enum_dev_ext, "vkEnumerateDeviceExtensionProperties");
    LOAD(destroy_instance, "vkDestroyInstance");
#undef LOAD
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vulkan-icd.so>\n", argv[0]);
        return 2;
    }

    res_fmt("probe.icd_path", "%s", argv[1]);
    uint64_t x18_start = x18_read();
    res_fmt("x18_initial", "0x%llx", (unsigned long long)x18_start);

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        res_fmt("probe.dlopen", "fail");
        res_fmt("probe.dlopen_error", "%s", dlerror());
        failures++;
        return finish(1);
    }
    x18_check("dlopen", x18_start);
    res_fmt("probe.dlopen", "ok");

    uint64_t x18_before_sym = x18_read();
    icd_gipa =
        (PFN_vk_icdGetInstanceProcAddr)(void *)dlsym(lib, "vk_icdGetInstanceProcAddr");
    x18_check("dlsym.vk_icdGetInstanceProcAddr", x18_before_sym);
    if (!icd_gipa) {
        res_fmt("probe.gipa", "missing");
        failures++;
        dlclose(lib);
        return finish(1);
    }
    res_fmt("probe.gipa", "ok");

    x18_before_sym = x18_read();
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion negotiate =
        (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(void *)dlsym(
            lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    x18_check("dlsym.vk_icdNegotiateLoaderICDInterfaceVersion", x18_before_sym);
    if (negotiate) {
        uint32_t version = 5;
        VkResult r = X18_CALL("vk_icdNegotiateLoaderICDInterfaceVersion", negotiate(&version));
        res_fmt("probe.icd_interface_negotiated", "1");
        res_fmt("probe.icd_interface_version", "%u", version);
        res_fmt("probe.icd_interface_result", "%d", (int)r);
        if (r != VK_SUCCESS)
            failures++;
    } else {
        res_fmt("probe.icd_interface_negotiated", "0");
        res_fmt("probe.icd_interface_version", "0");
    }

    api.create_instance = (PFN_vkCreateInstance)get_proc(NULL, "vkCreateInstance");
    if (!api.create_instance) {
        res_fmt("probe.entry.vkCreateInstance", "missing");
        failures++;
        dlclose(lib);
        return finish(1);
    }
    api.enum_instance_ext = (PFN_vkEnumerateInstanceExtensionProperties)get_proc(
        NULL, "vkEnumerateInstanceExtensionProperties");

    PFN_vkEnumerateInstanceVersion enum_version =
        (PFN_vkEnumerateInstanceVersion)get_proc(NULL, "vkEnumerateInstanceVersion");
    if (enum_version) {
        uint32_t v = 0;
        VkResult r = X18_CALL("vkEnumerateInstanceVersion", enum_version(&v));
        if (r == VK_SUCCESS) {
            res_fmt("probe.instance_version", "%u.%u.%u", VK_API_VERSION_MAJOR(v),
                    VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
        } else {
            res_fmt("probe.instance_version", "fail");
        }
    } else {
        res_fmt("probe.instance_version", "unavailable");
    }

    uint32_t chosen = 0;
    VkInstance instance = create_instance(&chosen);
    if (!instance) {
        res_fmt("probe.instance", "fail");
        failures++;
        dlclose(lib);
        return finish(1);
    }
    res_fmt("probe.instance", "ok");
    res_fmt("probe.instance_api", "%u.%u", VK_API_VERSION_MAJOR(chosen),
            VK_API_VERSION_MINOR(chosen));

    load_api(instance);
    if (!api.enum_devices || !api.get_props || !api.get_features || !api.get_queues ||
        !api.get_memory || !api.get_props2 || !api.get_memory2 || !api.enum_dev_ext ||
        !api.destroy_instance) {
        res_fmt("probe.api", "incomplete");
        X18_VOID("vkDestroyInstance", api.destroy_instance ? api.destroy_instance(instance, NULL)
                                                           : (void)0);
        dlclose(lib);
        return finish(1);
    }
    res_fmt("probe.api", "ok");

    uint32_t count = 0;
    VkResult r = X18_CALL("vkEnumeratePhysicalDevices", api.enum_devices(instance, &count, NULL));
    if (r != VK_SUCCESS) {
        res_fmt("probe.enumerate_devices_result", "%d", (int)r);
        failures++;
        count = 0;
    }

    VkPhysicalDevice *devices = NULL;
    if (count) {
        devices = calloc(count, sizeof(*devices));
        if (!devices) {
            failures++;
            count = 0;
        }
    }
    if (count) {
        r = X18_CALL("vkEnumeratePhysicalDevices", api.enum_devices(instance, &count, devices));
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
            res_fmt("probe.enumerate_devices_result", "%d", (int)r);
            failures++;
            count = 0;
        }
    }
    res_fmt("probe.physical_devices", "%u", count);
    if (count == 0)
        failures++;

    for (uint32_t i = 0; i < count; i++)
        probe_device(devices[i], i);
    free(devices);

    probe_drm();

    X18_VOID("vkDestroyInstance", api.destroy_instance(instance, NULL));
    X18_VOID("dlclose", dlclose(lib));

    return finish(failures ? 1 : 0);
}
