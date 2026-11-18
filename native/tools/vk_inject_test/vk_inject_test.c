/* Drives libtitan_vk.so through the loader entrypoints Wine uses and reports
 * what the platform loader plus the injected ICD expose. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static unsigned (*shim_state)(void);
static int state_printed;

static void fact(const char *key, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("RESULT %s=", key);
    for (const char *p = buf; *p != '\0'; p++)
        putchar(*p == '\n' || *p == '\r' ? ';' : *p);
    putchar('\n');
}

static void print_state(void)
{
    unsigned bits;

    if (shim_state == NULL)
        return;
    if (state_printed)
        return;
    bits = shim_state();
    fact("inject.state", "0x%x", bits);
    fact("inject.hook", (bits & (1u << 1)) != 0 ? "ok" : "failed");
    fact("inject.substituted", (bits & (1u << 2)) != 0 ? "1" : "0");
    state_printed = 1;
}

static void error_exit(int code, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    print_state();
    fact("inject.error", "%s", buf);
    fflush(stdout);
    exit(code);
}

static int has_ext(const VkExtensionProperties *exts, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, name) == 0)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    VkExtensionProperties *exts;
    VkExtensionProperties *dev_exts;
    const char *enabled[2];
    uint32_t ext_count = 0;
    uint32_t dev_ext_count = 0;
    uint32_t enabled_count = 0;
    uint32_t device_count = 0;
    VkPhysicalDevice *devices;
    VkPhysicalDeviceProperties2 props = {0};
    VkPhysicalDeviceDriverProperties driver = {0};
    VkApplicationInfo app = {0};
    VkInstanceCreateInfo create_info = {0};
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkEnumerateInstanceExtensionProperties enum_instance_ext;
    PFN_vkCreateInstance create_instance;
    PFN_vkEnumeratePhysicalDevices enum_devices;
    PFN_vkGetPhysicalDeviceProperties2 get_props2;
    PFN_vkEnumerateDeviceExtensionProperties enum_dev_ext;
    PFN_vkDestroyInstance destroy_instance;
    VkResult result;
    void *shim;
    const char *icd;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <libtitan_vk.so>\n", argv[0]);
        return 2;
    }

    shim = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (shim == NULL) {
        fact("inject.dlopen", "failed");
        fact("inject.error", "dlopen: %s", dlerror());
        return 2;
    }
    fact("inject.dlopen", "ok");
    fact("inject.shim", "%s", argv[1]);
    icd = getenv("TITAN_ICD");
    fact("inject.icd", "%s", icd != NULL ? icd : "");

    gipa = (PFN_vkGetInstanceProcAddr)dlsym(shim, "vkGetInstanceProcAddr");
    enum_instance_ext = (PFN_vkEnumerateInstanceExtensionProperties)dlsym(
        shim, "vkEnumerateInstanceExtensionProperties");
    create_instance = (PFN_vkCreateInstance)dlsym(shim, "vkCreateInstance");
    shim_state = (unsigned (*)(void))dlsym(shim, "titan_vk_inject_state");
    if (gipa == NULL || enum_instance_ext == NULL || create_instance == NULL) {
        fact("inject.error", "shim does not export the loader entrypoints");
        return 2;
    }

    result = enum_instance_ext(NULL, &ext_count, NULL);
    if (result != VK_SUCCESS)
        error_exit(3, "vkEnumerateInstanceExtensionProperties %d", result);
    exts = calloc(ext_count != 0 ? ext_count : 1, sizeof(*exts));
    if (exts == NULL)
        error_exit(3, "out of memory");
    result = enum_instance_ext(NULL, &ext_count, exts);
    if (result != VK_SUCCESS)
        error_exit(3, "vkEnumerateInstanceExtensionProperties %d", result);
    fact("inject.instance_extensions", "%u", ext_count);
    fact("inject.ext.VK_KHR_surface", "%d", has_ext(exts, ext_count, "VK_KHR_surface"));
    fact("inject.ext.VK_KHR_android_surface", "%d",
         has_ext(exts, ext_count, "VK_KHR_android_surface"));
    if (has_ext(exts, ext_count, "VK_KHR_surface"))
        enabled[enabled_count++] = "VK_KHR_surface";
    if (has_ext(exts, ext_count, "VK_KHR_android_surface"))
        enabled[enabled_count++] = "VK_KHR_android_surface";
    free(exts);

    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vk_inject_test";
    app.applicationVersion = 1;
    app.pEngineName = "titan";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app;
    create_info.enabledExtensionCount = enabled_count;
    create_info.ppEnabledExtensionNames = enabled;

    result = create_instance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS)
        error_exit(4, "vkCreateInstance %d", result);
    print_state();
    fact("inject.instance", "ok");
    fact("inject.instance_api", "1.3");

    enum_devices = (PFN_vkEnumeratePhysicalDevices)gipa(instance,
                                                         "vkEnumeratePhysicalDevices");
    get_props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(
        instance, "vkGetPhysicalDeviceProperties2");
    enum_dev_ext = (PFN_vkEnumerateDeviceExtensionProperties)gipa(
        instance, "vkEnumerateDeviceExtensionProperties");
    destroy_instance = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    if (enum_devices == NULL || get_props2 == NULL || enum_dev_ext == NULL)
        error_exit(5, "instance dispatch is incomplete");
    fact("inject.gipa", "ok");

    result = enum_devices(instance, &device_count, NULL);
    if (result != VK_SUCCESS)
        error_exit(5, "vkEnumeratePhysicalDevices %d", result);
    fact("inject.physical_devices", "%u", device_count);
    if (device_count == 0)
        error_exit(5, "no physical devices");
    devices = calloc(device_count, sizeof(*devices));
    if (devices == NULL)
        error_exit(5, "out of memory");
    result = enum_devices(instance, &device_count, devices);
    if (result != VK_SUCCESS)
        error_exit(5, "vkEnumeratePhysicalDevices %d", result);

    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    props.pNext = &driver;
    get_props2(devices[0], &props);
    if (props.properties.deviceName[0] == '\0')
        error_exit(6, "physical device 0 has an empty name");
    fact("device.0.name", "%s", props.properties.deviceName);
    fact("device.0.driver_name", "%s", driver.driverName);
    fact("device.0.driver_info", "%s", driver.driverInfo);
    fact("device.0.api_version", "%u.%u.%u",
         VK_API_VERSION_MAJOR(props.properties.apiVersion),
         VK_API_VERSION_MINOR(props.properties.apiVersion),
         VK_API_VERSION_PATCH(props.properties.apiVersion));
    fact("device.0.vendor_id", "0x%04x", props.properties.vendorID);
    fact("device.0.device_id", "0x%04x", props.properties.deviceID);

    result = enum_dev_ext(devices[0], NULL, &dev_ext_count, NULL);
    if (result != VK_SUCCESS)
        error_exit(7, "vkEnumerateDeviceExtensionProperties %d", result);
    dev_exts = calloc(dev_ext_count != 0 ? dev_ext_count : 1, sizeof(*dev_exts));
    if (dev_exts == NULL)
        error_exit(7, "out of memory");
    result = enum_dev_ext(devices[0], NULL, &dev_ext_count, dev_exts);
    if (result != VK_SUCCESS)
        error_exit(7, "vkEnumerateDeviceExtensionProperties %d", result);
    fact("inject.device_extensions", "%u", dev_ext_count);
    fact("inject.ext.VK_KHR_swapchain", "%d",
         has_ext(dev_exts, dev_ext_count, "VK_KHR_swapchain"));
    free(dev_exts);

    fact("inject.error", "none");
    if (destroy_instance != NULL)
        destroy_instance(instance, NULL);
    printf("vk_inject_test: PASS\n");
    return 0;
}
