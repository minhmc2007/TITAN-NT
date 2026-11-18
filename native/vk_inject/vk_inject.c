/*
 * Loader-shaped shim for Wine: loads the platform libvulkan.so and redirects
 * the single android_dlopen_ext call it makes for its vendor ICD to TITAN_ICD,
 * so the stock Android WSI (android surface + swapchain) drives radv.
 */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define TAG "titan_vk"
#define PLATFORM_LOADER "/system/lib64/libvulkan.so"
#define VENDOR_ICD_PREFIX "/vendor/lib64/hw/vulkan."
#define VENDOR_ICD_SUFFIX ".so"

enum {
    STATE_PLATFORM_LOADER = 1u << 0,
    STATE_HOOK_INSTALLED = 1u << 1,
    STATE_ICD_SUBSTITUTED = 1u << 2,
    STATE_VENDOR_FALLBACK = 1u << 3,
};

typedef void *(*android_dlopen_ext_fn)(const char *, int, const android_dlextinfo *);

struct loader_api {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions;
    PFN_vkEnumerateInstanceLayerProperties enumerate_instance_layers;
    PFN_vkCreateInstance create_instance;
    PFN_vkCreateAndroidSurfaceKHR create_android_surface;
};

struct loaded_object {
    const Elf64_Phdr *phdr;
    size_t phnum;
    Elf64_Addr base;
};

static struct loader_api g_api;
static void *g_platform;
static android_dlopen_ext_fn g_android_dlopen_ext;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static _Atomic unsigned g_state;

static void *substitute_dlopen(const char *path, int flags, const android_dlextinfo *info);

static int object_phdr(struct dl_phdr_info *info, size_t size, void *data)
{
    struct loaded_object *obj = data;
    const char *name = info->dlpi_name;
    const char *base = NULL;

    (void)size;
    if (name != NULL && name[0] != '\0') {
        base = strrchr(name, '/');
        base = base != NULL ? base + 1 : name;
    }
    if (base == NULL || strcmp(base, "libvulkan.so") != 0)
        return 0;

    obj->phdr = info->dlpi_phdr;
    obj->phnum = info->dlpi_phnum;
    obj->base = info->dlpi_addr;
    return 1;
}

static const Elf64_Phdr *object_segment(const struct loaded_object *obj, uintptr_t address)
{
    for (size_t i = 0; i < obj->phnum; i++) {
        const Elf64_Phdr *ph = &obj->phdr[i];
        uintptr_t start;

        if (ph->p_type != PT_LOAD)
            continue;
        start = obj->base + ph->p_vaddr;
        if (address >= start && address < start + ph->p_memsz)
            return ph;
    }
    return NULL;
}

static int object_in_relro(const struct loaded_object *obj, uintptr_t address)
{
    for (size_t i = 0; i < obj->phnum; i++) {
        const Elf64_Phdr *ph = &obj->phdr[i];
        uintptr_t start;

        if (ph->p_type != PT_GNU_RELRO)
            continue;
        start = obj->base + ph->p_vaddr;
        if (address >= start && address < start + ph->p_memsz)
            return 1;
    }
    return 0;
}

/* Bionic keeps DT_* d_ptr values link-time relative, glibc relocates them in
 * place. Accept whichever form lands inside a PT_LOAD of this object. */
static uintptr_t object_pointer(const struct loaded_object *obj, Elf64_Addr value)
{
    uintptr_t relocated;

    if (value == 0)
        return 0;
    if (object_segment(obj, (uintptr_t)value) != NULL)
        return (uintptr_t)value;
    relocated = (uintptr_t)value + obj->base;
    return object_segment(obj, relocated) != NULL ? relocated : 0;
}

static int patch_slot(const struct loaded_object *obj, uintptr_t slot, void *value)
{
    const Elf64_Phdr *segment;
    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t start;
    uintptr_t end;
    int protection = PROT_READ;

    if (page_size <= 0)
        return -1;
    start = slot & ~(uintptr_t)(page_size - 1);
    end = (slot + sizeof(void *) + (uintptr_t)page_size - 1) &
          ~(uintptr_t)(page_size - 1);
    if (mprotect((void *)start, (size_t)(end - start), PROT_READ | PROT_WRITE) != 0)
        return -1;

    *(void *volatile *)slot = value;

    /* RELRO pages were mapped read-only after relocation even though the
     * containing PT_LOAD still carries PF_W. */
    segment = object_segment(obj, slot);
    if (segment != NULL && (segment->p_flags & PF_W) != 0 &&
        !object_in_relro(obj, slot))
        protection |= PROT_WRITE;
    if (mprotect((void *)start, (size_t)(end - start), protection) != 0)
        return -1;
    return *(void *volatile *)slot == value ? 0 : -1;
}

static int patch_relocations(const struct loaded_object *obj, uintptr_t table, size_t size,
                             uintptr_t symtab, uintptr_t strtab, unsigned type,
                             unsigned *patched)
{
    for (size_t offset = 0; offset + sizeof(Elf64_Rela) <= size;
         offset += sizeof(Elf64_Rela)) {
        const Elf64_Rela *rel = (const Elf64_Rela *)(table + offset);
        const Elf64_Sym *sym;
        const char *name;

        if (ELF64_R_TYPE(rel->r_info) != type)
            continue;
        sym = (const Elf64_Sym *)(symtab + ELF64_R_SYM(rel->r_info) * sizeof(Elf64_Sym));
        name = (const char *)(strtab + sym->st_name);
        if (strcmp(name, "android_dlopen_ext") != 0)
            continue;
        if (patch_slot(obj, obj->base + rel->r_offset,
                       (void *)(uintptr_t)substitute_dlopen) != 0)
            return -1;
        (*patched)++;
    }
    return 0;
}

static int install_hook(void)
{
    struct loaded_object obj = {0};
    const Elf64_Dyn *dynamic = NULL;
    Elf64_Addr strtab_value = 0;
    Elf64_Addr symtab_value = 0;
    Elf64_Addr jmprel_value = 0;
    Elf64_Xword pltrelsz = 0;
    Elf64_Xword pltrel = 0;
    Elf64_Addr rela_value = 0;
    Elf64_Xword relasz = 0;
    uintptr_t strtab;
    uintptr_t symtab;
    uintptr_t jmprel;
    uintptr_t rela;
    unsigned patched = 0;

    (void)dl_iterate_phdr(object_phdr, &obj);
    if (obj.phdr == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s is not mapped", PLATFORM_LOADER);
        return -1;
    }
    for (size_t i = 0; i < obj.phnum; i++) {
        if (obj.phdr[i].p_type == PT_DYNAMIC) {
            dynamic = (const Elf64_Dyn *)(obj.base + obj.phdr[i].p_vaddr);
            break;
        }
    }
    if (dynamic == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s has no PT_DYNAMIC", PLATFORM_LOADER);
        return -1;
    }

    for (const Elf64_Dyn *d = dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB:
            strtab_value = d->d_un.d_ptr;
            break;
        case DT_SYMTAB:
            symtab_value = d->d_un.d_ptr;
            break;
        case DT_JMPREL:
            jmprel_value = d->d_un.d_ptr;
            break;
        case DT_PLTRELSZ:
            pltrelsz = d->d_un.d_val;
            break;
        case DT_PLTREL:
            pltrel = d->d_un.d_val;
            break;
        case DT_RELA:
            rela_value = d->d_un.d_ptr;
            break;
        case DT_RELASZ:
            relasz = d->d_un.d_val;
            break;
        default:
            break;
        }
    }

    strtab = object_pointer(&obj, strtab_value);
    symtab = object_pointer(&obj, symtab_value);
    jmprel = object_pointer(&obj, jmprel_value);
    rela = object_pointer(&obj, rela_value);
    if (strtab == 0 || symtab == 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s has no dynamic string table",
                            PLATFORM_LOADER);
        return -1;
    }

    if (jmprel != 0 && pltrelsz != 0 && (pltrel == DT_RELA || pltrel == 0)) {
        if (patch_relocations(&obj, jmprel, pltrelsz, symtab, strtab,
                              R_AARCH64_JUMP_SLOT, &patched) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG,
                                "cannot rewrite the android_dlopen_ext PLT slot");
            return -1;
        }
    }
    if (rela != 0 && relasz != 0) {
        if (patch_relocations(&obj, rela, relasz, symtab, strtab,
                              R_AARCH64_GLOB_DAT, &patched) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG,
                                "cannot rewrite the android_dlopen_ext GOT slot");
            return -1;
        }
    }
    if (patched == 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "%s has no android_dlopen_ext relocation", PLATFORM_LOADER);
        return -1;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "hooked android_dlopen_ext in %s (%u slots)",
                        PLATFORM_LOADER, patched);
    return 0;
}

static int is_vendor_icd(const char *path)
{
    const size_t prefix = sizeof(VENDOR_ICD_PREFIX) - 1;
    const size_t suffix = sizeof(VENDOR_ICD_SUFFIX) - 1;
    const char *name;
    size_t length;

    if (strncmp(path, VENDOR_ICD_PREFIX, prefix) != 0)
        return 0;
    name = path + prefix;
    length = strlen(name);
    return length > suffix && strcmp(name + length - suffix, VENDOR_ICD_SUFFIX) == 0;
}

static void *substitute_dlopen(const char *path, int flags, const android_dlextinfo *info)
{
    if (path != NULL && is_vendor_icd(path)) {
        const char *icd = getenv("TITAN_ICD");

        if (icd == NULL || icd[0] == '\0') {
            __android_log_print(ANDROID_LOG_WARN, TAG, "TITAN_ICD unset, opening %s", path);
        } else if (icd[0] != '/') {
            __android_log_print(ANDROID_LOG_WARN, TAG, "TITAN_ICD %s is not absolute, opening %s",
                                icd, path);
        } else if (access(icd, R_OK) != 0) {
            __android_log_print(ANDROID_LOG_WARN, TAG, "TITAN_ICD %s unreadable (%s), opening %s",
                                icd, strerror(errno), path);
        } else {
            void *handle;

            __android_log_print(ANDROID_LOG_INFO, TAG, "TITAN_ICD %s replaces %s", icd, path);
            /* The vendor namespace cannot see /data/app, so the ICD opens in
             * the caller's namespace. */
            handle = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
            if (handle != NULL) {
                atomic_fetch_or_explicit(&g_state, STATE_ICD_SUBSTITUTED,
                                         memory_order_relaxed);
                return handle;
            }
            __android_log_print(ANDROID_LOG_WARN, TAG, "dlopen %s failed: %s", icd,
                                dlerror());
        }
        atomic_fetch_or_explicit(&g_state, STATE_VENDOR_FALLBACK, memory_order_relaxed);
    }
    return g_android_dlopen_ext(path, flags, info);
}

static void init_once(void)
{
    void *libdl;

    g_platform = dlopen(PLATFORM_LOADER, RTLD_NOW | RTLD_LOCAL);
    if (g_platform == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "dlopen %s failed: %s", PLATFORM_LOADER,
                            dlerror());
        return;
    }
    atomic_fetch_or_explicit(&g_state, STATE_PLATFORM_LOADER, memory_order_relaxed);

    g_api.get_instance_proc_addr =
        (PFN_vkGetInstanceProcAddr)dlsym(g_platform, "vkGetInstanceProcAddr");
    g_api.get_device_proc_addr =
        (PFN_vkGetDeviceProcAddr)dlsym(g_platform, "vkGetDeviceProcAddr");
    g_api.enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)dlsym(g_platform, "vkEnumerateInstanceVersion");
    g_api.enumerate_instance_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)dlsym(g_platform,
                                                         "vkEnumerateInstanceExtensionProperties");
    g_api.enumerate_instance_layers =
        (PFN_vkEnumerateInstanceLayerProperties)dlsym(g_platform,
                                                      "vkEnumerateInstanceLayerProperties");
    g_api.create_instance =
        (PFN_vkCreateInstance)dlsym(g_platform, "vkCreateInstance");
    g_api.create_android_surface =
        (PFN_vkCreateAndroidSurfaceKHR)dlsym(g_platform, "vkCreateAndroidSurfaceKHR");

    if (g_api.get_instance_proc_addr == NULL || g_api.create_instance == NULL ||
        g_api.enumerate_instance_extensions == NULL ||
        g_api.enumerate_instance_layers == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "%s does not export the loader entrypoints", PLATFORM_LOADER);
        return;
    }

    libdl = dlopen("libdl.so", RTLD_NOW | RTLD_LOCAL);
    if (libdl != NULL)
        g_android_dlopen_ext =
            (android_dlopen_ext_fn)dlsym(libdl, "android_dlopen_ext");
    if (g_android_dlopen_ext == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "cannot resolve android_dlopen_ext, ICD stays stock");
        return;
    }

    /* The platform loader binds its ICD on the first API call; the slot has to
     * be swapped before that, which is why every entrypoint lands here first. */
    if (install_hook() != 0)
        return;
    atomic_fetch_or_explicit(&g_state, STATE_HOOK_INSTALLED, memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s ready", PLATFORM_LOADER);
}

static void ensure_ready(void)
{
    (void)pthread_once(&g_once, init_once);
}

unsigned titan_vk_inject_state(void)
{
    ensure_ready();
    return atomic_load_explicit(&g_state, memory_order_relaxed);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *pApiVersion)
{
    ensure_ready();
    if (g_api.enumerate_instance_version == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    return g_api.enumerate_instance_version(pApiVersion);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    ensure_ready();
    if (g_api.enumerate_instance_extensions == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    return g_api.enumerate_instance_extensions(pLayerName, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
    ensure_ready();
    if (g_api.enumerate_instance_layers == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    return g_api.enumerate_instance_layers(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkInstance *pInstance)
{
    ensure_ready();
    if (g_api.create_instance == NULL) {
        if (pInstance != NULL)
            *pInstance = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return g_api.create_instance(pCreateInfo, pAllocator, pInstance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *pName)
{
    ensure_ready();
    if (g_api.get_instance_proc_addr == NULL || pName == NULL)
        return NULL;
    return g_api.get_instance_proc_addr(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char *pName)
{
    ensure_ready();
    if (g_api.get_device_proc_addr == NULL || pName == NULL)
        return NULL;
    return g_api.get_device_proc_addr(device, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    ensure_ready();
    if (g_api.create_android_surface == NULL)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return g_api.create_android_surface(instance, pCreateInfo, pAllocator, pSurface);
}
