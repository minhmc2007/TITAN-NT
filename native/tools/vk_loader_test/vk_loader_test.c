#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

struct vk_api {
    PFN_vkCreateInstance create_instance;
    PFN_vkEnumerateInstanceExtensionProperties enum_instance_ext;
    PFN_vkEnumerateInstanceVersion enum_instance_version;
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
static PFN_vkGetInstanceProcAddr gipa;
static unsigned failures;

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

static void res_fixed(const char *key, const char *s, size_t n) {
    char buf[512];
    size_t len = 0;
    while (len < n && s[len] && len + 1 < sizeof(buf))
        len++;
    memcpy(buf, s, len);
    buf[len] = 0;
    res_fmt(key, "%s", buf);
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

static int has_ext(const VkExtensionProperties *exts, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, name) == 0)
            return 1;
    }
    return 0;
}

static PFN_vkVoidFunction get_proc(VkInstance instance, const char *name) {
    return gipa(instance, name);
}

static void entry_missing(const char *name) {
    char key[128];
    snprintf(key, sizeof(key), "loader.entry.%s", name);
    res_fmt(key, "missing");
    failures++;
}

static uint32_t enumerate_instance_ext(VkExtensionProperties **out) {
    *out = NULL;
    if (!api.enum_instance_ext)
        return 0;

    uint32_t count = 0;
    VkResult r = api.enum_instance_ext(NULL, &count, NULL);
    if (r != VK_SUCCESS) {
        res_fmt("loader.instance_extensions_result", "%d", (int)r);
        return 0;
    }
    if (count == 0)
        return 0;

    VkExtensionProperties *exts = calloc(count, sizeof(*exts));
    if (!exts) {
        res_fmt("loader.error", "instance_extensions_oom");
        failures++;
        return 0;
    }

    r = api.enum_instance_ext(NULL, &count, exts);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        res_fmt("loader.instance_extensions_result", "%d", (int)r);
        free(exts);
        return 0;
    }

    *out = exts;
    return count;
}

static VkInstance create_instance(uint32_t *chosen, int have_props2_ext) {
    const char *enabled[1];
    uint32_t enabled_count = 0;
    if (have_props2_ext)
        enabled[enabled_count++] = "VK_KHR_get_physical_device_properties2";

    static const uint32_t versions[] = {
        VK_MAKE_API_VERSION(0, 1, 4, 0),
        VK_MAKE_API_VERSION(0, 1, 3, 0),
        VK_MAKE_API_VERSION(0, 1, 2, 0),
        VK_MAKE_API_VERSION(0, 1, 1, 0),
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult last = VK_ERROR_INITIALIZATION_FAILED;

    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        VkApplicationInfo app;
        memset(&app, 0, sizeof(app));
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "titan-m3-vk-loader-test";
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

        last = api.create_instance(&info, NULL, &instance);
        if (last == VK_SUCCESS) {
            *chosen = versions[i];
            break;
        }
    }

    res_fmt("loader.instance_result", "%d", (int)last);
    return instance;
}

static void print_device(VkPhysicalDevice dev, uint32_t index) {
    char key[192];
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));
    api.get_props(dev, &props);

    snprintf(key, sizeof(key), "device.%u.name", index);
    res_fixed(key, props.deviceName, sizeof(props.deviceName));
    if (props.deviceName[0] == 0) {
        res_fmt("loader.error", "empty_device_name");
        failures++;
    }

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
    api.get_features(dev, &features);
    snprintf(key, sizeof(key), "device.%u.features", index);
    res_fmt(key, "ok");

    uint32_t qcount = 0;
    api.get_queues(dev, &qcount, NULL);
    snprintf(key, sizeof(key), "device.%u.queue_families_result", index);
    res_fmt(key, "ok");

    VkQueueFamilyProperties *queues = NULL;
    if (qcount) {
        queues = calloc(qcount, sizeof(*queues));
        if (!queues) {
            res_fmt("loader.error", "queue_families_oom");
            failures++;
            qcount = 0;
        }
    }
    if (qcount)
        api.get_queues(dev, &qcount, queues);
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
    api.get_memory(dev, &mem);
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
    VkResult r = api.enum_dev_ext(dev, NULL, &ext_count, NULL);
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
            res_fmt("loader.error", "device_extensions_oom");
            failures++;
            ext_count = 0;
        }
    }
    if (ext_count) {
        r = api.enum_dev_ext(dev, NULL, &ext_count, exts);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
            failures++;
            ext_count = 0;
        }
    }
    snprintf(key, sizeof(key), "device.%u.device_extensions", index);
    res_fmt(key, "%u", ext_count);

    int has_budget = has_ext(exts, ext_count, "VK_EXT_memory_budget");
    int has_pci = has_ext(exts, ext_count, "VK_EXT_pci_bus_info");
    int has_driver = has_ext(exts, ext_count, "VK_KHR_driver_properties");
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

        api.get_props2(dev, &props2);
        snprintf(key, sizeof(key), "device.%u.properties2", index);
        res_fmt(key, "ok");
        if (want_driver) {
            snprintf(key, sizeof(key), "device.%u.driver_name", index);
            res_fixed(key, drv.driverName, sizeof(drv.driverName));
            snprintf(key, sizeof(key), "device.%u.driver_info", index);
            res_fixed(key, drv.driverInfo, sizeof(drv.driverInfo));
            snprintf(key, sizeof(key), "device.%u.driver_id", index);
            res_fmt(key, "%d", (int)drv.driverID);
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

        api.get_memory2(dev, &mem2);
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

static int finish(void) {
    res_fmt("loader.failures", "%u", failures);
    res_fmt("loader.exit", "%d", failures ? 1 : 0);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vulkan-loader.so>\n", argv[0]);
        return 2;
    }

    res_fmt("loader.path", "%s", argv[1]);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        res_fmt("loader.dlopen", "fail");
        res_fmt("loader.dlopen_error", "%s", dlerror());
        failures++;
        return finish();
    }
    res_fmt("loader.dlopen", "ok");

    gipa = (PFN_vkGetInstanceProcAddr)(void *)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) {
        res_fmt("loader.gipa", "missing");
        failures++;
        dlclose(lib);
        return finish();
    }
    res_fmt("loader.gipa", "ok");

    api.enum_instance_version =
        (PFN_vkEnumerateInstanceVersion)(void *)get_proc(NULL, "vkEnumerateInstanceVersion");
    if (api.enum_instance_version) {
        uint32_t version = 0;
        VkResult r = api.enum_instance_version(&version);
        if (r == VK_SUCCESS) {
            res_fmt("loader.instance_version", "%u.%u.%u", VK_API_VERSION_MAJOR(version),
                    VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
        } else {
            res_fmt("loader.instance_version", "fail");
        }
    } else {
        res_fmt("loader.instance_version", "unavailable");
    }

    api.enum_instance_ext = (PFN_vkEnumerateInstanceExtensionProperties)(void *)get_proc(
        NULL, "vkEnumerateInstanceExtensionProperties");
    api.create_instance = (PFN_vkCreateInstance)(void *)get_proc(NULL, "vkCreateInstance");
    if (!api.create_instance) {
        entry_missing("vkCreateInstance");
        dlclose(lib);
        return finish();
    }

    VkExtensionProperties *instance_exts = NULL;
    uint32_t instance_ext_count = enumerate_instance_ext(&instance_exts);
    int have_props2_ext = has_ext(instance_exts, instance_ext_count,
                                  "VK_KHR_get_physical_device_properties2");
    res_fmt("loader.instance_extensions", "%u", instance_ext_count);
    res_fmt("loader.ext.VK_KHR_surface", "%d",
            has_ext(instance_exts, instance_ext_count, "VK_KHR_surface"));
    res_fmt("loader.ext.VK_KHR_android_surface", "%d",
            has_ext(instance_exts, instance_ext_count, "VK_KHR_android_surface"));
    free(instance_exts);

    uint32_t chosen = 0;
    VkInstance instance = create_instance(&chosen, have_props2_ext);
    if (!instance) {
        res_fmt("loader.instance", "fail");
        failures++;
        dlclose(lib);
        return finish();
    }
    res_fmt("loader.instance", "ok");
    res_fmt("loader.instance_api", "%u.%u", VK_API_VERSION_MAJOR(chosen),
            VK_API_VERSION_MINOR(chosen));

#define LOAD(field, name)                                                                          \
    do {                                                                                           \
        api.field = (__typeof__(api.field))(void *)get_proc(instance, name);                       \
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

    if (!api.enum_devices || !api.get_props || !api.get_features || !api.get_queues ||
        !api.get_memory || !api.enum_dev_ext || !api.destroy_instance) {
        res_fmt("loader.api", "incomplete");
        if (api.destroy_instance)
            api.destroy_instance(instance, NULL);
        dlclose(lib);
        return finish();
    }
    res_fmt("loader.api", "ok");

    uint32_t count = 0;
    VkResult r = api.enum_devices(instance, &count, NULL);
    res_fmt("loader.enumerate_devices_result", "%d", (int)r);
    if (r != VK_SUCCESS)
        count = 0;

    VkPhysicalDevice *devices = NULL;
    if (count) {
        devices = calloc(count, sizeof(*devices));
        if (!devices) {
            res_fmt("loader.error", "physical_devices_oom");
            failures++;
            count = 0;
        }
    }
    if (count) {
        r = api.enum_devices(instance, &count, devices);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
            res_fmt("loader.enumerate_devices_result", "%d", (int)r);
            failures++;
            count = 0;
        }
    }

    res_fmt("loader.physical_devices", "%u", count);
    if (count == 0) {
        res_fmt("loader.error", "no_devices");
        failures++;
    }
    for (uint32_t i = 0; i < count; i++)
        print_device(devices[i], i);
    free(devices);

    api.destroy_instance(instance, NULL);
    dlclose(lib);
    return finish();
}
