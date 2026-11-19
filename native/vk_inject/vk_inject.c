/* Loader-shaped shim: wraps /system/lib64/libvulkan.so and redirects its
 * vendor ICD open to TITAN_ICD, so the stock Android WSI drives radv. */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define TAG "titan_vk"
#define PLATFORM_LOADER "/system/lib64/libvulkan.so"
#define VENDOR_ICD_SONAME "vulkan."
#define VENDOR_ICD_SUFFIX ".so"
#define MAX_ICD_HANDLES 8
#define MAX_MATCHED_PATH 256

enum {
    STATE_PLATFORM_LOADER = 1u << 0,
    STATE_HOOK_INSTALLED = 1u << 1,
    STATE_ICD_SUBSTITUTED = 1u << 2,
    STATE_VENDOR_FALLBACK = 1u << 3,
};

enum hook_slot {
    HOOK_LOAD_SPHAL,
    HOOK_UNLOAD_SPHAL,
    HOOK_ANDROID_DLOPEN_EXT,
    HOOK_DLOPEN,
    HOOK_COUNT,
};

typedef void *(*android_dlopen_ext_fn)(const char *, int, const android_dlextinfo *);
typedef void *(*android_load_sphal_library_fn)(const char *, int);
typedef int (*android_unload_sphal_library_fn)(void *);
typedef void *(*dlopen_fn)(const char *, int);

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

static const char *const g_hook_names[HOOK_COUNT] = {
    "android_load_sphal_library",
    "android_unload_sphal_library",
    "android_dlopen_ext",
    "dlopen",
};

static struct loader_api g_api;
static void *g_platform;
static uintptr_t g_platform_base;
static int g_ui_process;
static android_dlopen_ext_fn g_android_dlopen_ext;
static android_load_sphal_library_fn g_load_sphal;
static android_unload_sphal_library_fn g_unload_sphal;
static dlopen_fn g_dlopen;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static _Atomic unsigned g_state;
static _Atomic int g_symbol_recorded;
static _Atomic int g_path_recorded;
static _Atomic(void *) g_icd_handles[MAX_ICD_HANDLES];
static char g_hook_symbol[64];
static char g_hook_symbols[128];
static char g_matched_path[MAX_MATCHED_PATH];

static void *replace_load_sphal(const char *name, int flag);
static int replace_unload_sphal(void *handle);
static void *replace_android_dlopen_ext(const char *path, int flags,
                                        const android_dlextinfo *info);
static void *replace_dlopen(const char *path, int flags);
static int hosts_hwui(void);
static void install_crash_trace(void);

/* Match by address: the shim is also installed as libvulkan.so. */
static int object_phdr(struct dl_phdr_info *info, size_t size, void *data)
{
    struct loaded_object *obj = data;

    (void)size;
    if (g_platform_base == 0 || (uintptr_t)info->dlpi_addr != g_platform_base)
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

/* Bionic keeps DT_* pointers link-time relative; accept either form. */
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

    /* RELRO pages stay read-only even though the PT_LOAD has PF_W. */
    segment = object_segment(obj, slot);
    if (segment != NULL && (segment->p_flags & PF_W) != 0 &&
        !object_in_relro(obj, slot))
        protection |= PROT_WRITE;
    if (mprotect((void *)start, (size_t)(end - start), protection) != 0)
        return -1;
    return *(void *volatile *)slot == value ? 0 : -1;
}

static int hook_index(const char *name)
{
    for (int i = 0; i < HOOK_COUNT; i++) {
        if (strcmp(name, g_hook_names[i]) == 0)
            return i;
    }
    return -1;
}

static void *hook_target(int index)
{
    switch (index) {
    case HOOK_LOAD_SPHAL:
        return (void *)(uintptr_t)replace_load_sphal;
    case HOOK_UNLOAD_SPHAL:
        return (void *)(uintptr_t)replace_unload_sphal;
    case HOOK_ANDROID_DLOPEN_EXT:
        return (void *)(uintptr_t)replace_android_dlopen_ext;
    case HOOK_DLOPEN:
        return (void *)(uintptr_t)replace_dlopen;
    default:
        return NULL;
    }
}

static int hook_resolved(int index)
{
    switch (index) {
    case HOOK_LOAD_SPHAL:
        return g_load_sphal != NULL;
    case HOOK_UNLOAD_SPHAL:
        return g_unload_sphal != NULL;
    case HOOK_ANDROID_DLOPEN_EXT:
        return g_android_dlopen_ext != NULL;
    case HOOK_DLOPEN:
        return g_dlopen != NULL;
    default:
        return 0;
    }
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
        int index;

        if (ELF64_R_TYPE(rel->r_info) != type)
            continue;
        sym = (const Elf64_Sym *)(symtab + ELF64_R_SYM(rel->r_info) * sizeof(Elf64_Sym));
        name = (const char *)(strtab + sym->st_name);
        index = hook_index(name);
        if (index < 0)
            continue;
        if (!hook_resolved(index)) {
            __android_log_print(ANDROID_LOG_WARN, TAG,
                                "cannot resolve the real %s, slot left alone", name);
            continue;
        }
        if (patch_slot(obj, obj->base + rel->r_offset, hook_target(index)) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot rewrite the %s slot", name);
            return -1;
        }
        *patched |= 1u << index;
    }
    return 0;
}

static void format_hook_symbols(unsigned patched)
{
    size_t used = 0;

    g_hook_symbols[0] = '\0';
    for (int i = 0; i < HOOK_COUNT; i++) {
        int written;

        if ((patched & (1u << i)) == 0)
            continue;
        written = snprintf(g_hook_symbols + used, sizeof(g_hook_symbols) - used, "%s%s",
                           used != 0 ? "," : "", g_hook_names[i]);
        if (written < 0)
            break;
        used += (size_t)written;
        if (used >= sizeof(g_hook_symbols))
            break;
    }
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
                              R_AARCH64_JUMP_SLOT, &patched) != 0)
            return -1;
    }
    if (rela != 0 && relasz != 0) {
        if (patch_relocations(&obj, rela, relasz, symtab, strtab,
                              R_AARCH64_GLOB_DAT, &patched) != 0)
            return -1;
    }
    if (patched == 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "%s imports none of android_load_sphal_library, "
                            "android_unload_sphal_library, android_dlopen_ext, dlopen",
                            PLATFORM_LOADER);
        return -1;
    }
    format_hook_symbols(patched);
    __android_log_print(ANDROID_LOG_INFO, TAG, "hooked %s in %s", g_hook_symbols,
                        PLATFORM_LOADER);
    return 0;
}

static int is_vendor_icd(const char *path)
{
    static const char *const dirs[] = {
        "/vendor/lib64/hw/",
        "/vendor/lib/hw/",
    };
    const char *name = NULL;
    size_t length;
    const size_t suffix = sizeof(VENDOR_ICD_SUFFIX) - 1;

    if (path == NULL)
        return 0;
    if (path[0] == '/') {
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            const size_t dir = strlen(dirs[i]);

            if (strncmp(path, dirs[i], dir) == 0) {
                name = path + dir;
                break;
            }
        }
        if (name == NULL)
            return 0;
    } else {
        if (strchr(path, '/') != NULL)
            return 0;
        name = path;
    }
    if (strncmp(name, VENDOR_ICD_SONAME, sizeof(VENDOR_ICD_SONAME) - 1) != 0)
        return 0;
    length = strlen(name);
    return length > suffix + sizeof(VENDOR_ICD_SONAME) - 1 &&
           strcmp(name + length - suffix, VENDOR_ICD_SUFFIX) == 0;
}

static void track_handle(void *handle)
{
    for (size_t i = 0; i < MAX_ICD_HANDLES; i++) {
        void *expected = NULL;

        if (atomic_compare_exchange_strong_explicit(&g_icd_handles[i], &expected, handle,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed))
            return;
    }
    __android_log_print(ANDROID_LOG_WARN, TAG, "ICD handle table full, %p will not unload",
                        handle);
}

static int untrack_handle(void *handle)
{
    if (handle == NULL)
        return 0;
    for (size_t i = 0; i < MAX_ICD_HANDLES; i++) {
        void *expected = handle;

        if (atomic_compare_exchange_strong_explicit(&g_icd_handles[i], &expected, NULL,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed))
            return 1;
    }
    return 0;
}

static void record_symbol(int index)
{
    if (atomic_exchange_explicit(&g_symbol_recorded, 1, memory_order_acq_rel) == 0)
        snprintf(g_hook_symbol, sizeof(g_hook_symbol), "%s", g_hook_names[index]);
}

static void record_path(const char *path)
{
    if (atomic_exchange_explicit(&g_path_recorded, 1, memory_order_acq_rel) == 0)
        snprintf(g_matched_path, sizeof(g_matched_path), "%s", path);
}

static void *open_icd(int hook, const char *path)
{
    const char *icd;

    if (!is_vendor_icd(path))
        return NULL;
    icd = getenv("TITAN_ICD");
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

        __android_log_print(ANDROID_LOG_INFO, TAG, "%s: TITAN_ICD %s replaces %s",
                            g_hook_names[hook], icd, path);
        /* the vendor namespace cannot see /data/app */
        handle = dlopen(icd, RTLD_NOW);
        if (handle != NULL) {
            track_handle(handle);
            atomic_fetch_or_explicit(&g_state, STATE_ICD_SUBSTITUTED,
                                     memory_order_relaxed);
            record_symbol(hook);
            record_path(path);
            return handle;
        }
        __android_log_print(ANDROID_LOG_WARN, TAG, "dlopen %s failed: %s", icd, dlerror());
    }
    atomic_fetch_or_explicit(&g_state, STATE_VENDOR_FALLBACK, memory_order_relaxed);
    return NULL;
}

static void *replace_load_sphal(const char *name, int flag)
{
    void *handle = open_icd(HOOK_LOAD_SPHAL, name);

    if (handle != NULL)
        return handle;
    return g_load_sphal != NULL ? g_load_sphal(name, flag) : NULL;
}

static int replace_unload_sphal(void *handle)
{
    if (untrack_handle(handle))
        return dlclose(handle);
    return g_unload_sphal != NULL ? g_unload_sphal(handle) : -1;
}

static void *replace_android_dlopen_ext(const char *path, int flags,
                                        const android_dlextinfo *info)
{
    void *handle = open_icd(HOOK_ANDROID_DLOPEN_EXT, path);

    if (handle != NULL)
        return handle;
    return g_android_dlopen_ext != NULL ? g_android_dlopen_ext(path, flags, info) : NULL;
}

static void *replace_dlopen(const char *path, int flags)
{
    void *handle = open_icd(HOOK_DLOPEN, path);

    if (handle != NULL)
        return handle;
    return g_dlopen != NULL ? g_dlopen(path, flags) : NULL;
}

static int resolve_reals(void)
{
    void *libdl = dlopen("libdl.so", RTLD_NOW | RTLD_LOCAL);

    if (libdl == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "dlopen libdl.so failed: %s", dlerror());
        return -1;
    }
    /* libvndksupport is DT_NEEDED, so the handle search reaches the real pair */
    g_load_sphal =
        (android_load_sphal_library_fn)dlsym(g_platform, "android_load_sphal_library");
    g_unload_sphal =
        (android_unload_sphal_library_fn)dlsym(g_platform, "android_unload_sphal_library");
    g_android_dlopen_ext =
        (android_dlopen_ext_fn)dlsym(libdl, "android_dlopen_ext");
    g_dlopen = (dlopen_fn)dlsym(libdl, "dlopen");
    return 0;
}

/* dladdr reports the same load address dl_iterate_phdr calls dlpi_addr. */
static int resolve_platform_base(void)
{
    Dl_info info;

    if (dladdr((void *)(uintptr_t)g_api.get_instance_proc_addr, &info) == 0 ||
        info.dli_fbase == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot locate the load address of %s",
                            PLATFORM_LOADER);
        return -1;
    }
    g_platform_base = (uintptr_t)info.dli_fbase;
    return 0;
}

static void init_once(void)
{
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

    if (resolve_reals() != 0)
        return;

    if (resolve_platform_base() != 0)
        return;

    g_ui_process = hosts_hwui();
    if (g_ui_process)
        install_crash_trace();

    /* the loader binds its ICD on the first API call, so patch first */
    if (install_hook() != 0)
        return;
    atomic_fetch_or_explicit(&g_state, STATE_HOOK_INSTALLED, memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s ready, ui_process=%d", PLATFORM_LOADER,
                        g_ui_process);
}

static void ensure_ready(void)
{
    (void)pthread_once(&g_once, init_once);
}

static int maps_contains(const char *needle)
{
    FILE *maps = fopen("/proc/self/maps", "re");
    char line[512];
    int found = 0;

    if (maps == NULL)
        return 0;
    while (fgets(line, sizeof(line), maps) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = 1;
            break;
        }
    }
    fclose(maps);
    return found;
}

static void write_frame(int fd, uintptr_t frame)
{
    Dl_info info;

    if (dladdr((void *)frame, &info) != 0 && info.dli_fname != NULL)
        dprintf(fd, "%#" PRIxPTR " %s+%#" PRIxPTR "\n", frame, info.dli_fname,
                frame - (uintptr_t)info.dli_fbase);
    else
        dprintf(fd, "%#" PRIxPTR "\n", frame);
}

static void crash_handler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = ctx;
    char comm[32] = {0};
    uintptr_t sp = uc->uc_mcontext.sp;
    uintptr_t fp = uc->uc_mcontext.regs[29];
    int fd, i;

    (void)info;
    fd = open("/data/user/0/dev.titan.nt/files/crash.txt",
              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        int comm_fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
        if (comm_fd >= 0) {
            (void)!read(comm_fd, comm, sizeof(comm) - 1);
            close(comm_fd);
        }
        dprintf(fd, "signal %d tid %d comm %s\n", sig, gettid(), comm);
        write_frame(fd, uc->uc_mcontext.pc);
        write_frame(fd, uc->uc_mcontext.regs[30]);
        for (i = 0; i < 32 && fp > sp && fp < sp + (8u << 20); i++) {
            uintptr_t next = *(uintptr_t *)fp;
            uintptr_t ret = *(uintptr_t *)(fp + 8);
            if (ret != 0)
                write_frame(fd, ret - 4);
            if (next <= fp)
                break;
            fp = next;
        }
        close(fd);
    }
    _exit(70);
}

static void install_crash_trace(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGILL, &action, NULL);
}

/* HWUI in an Android UI process holds the vendor driver; a second client there
 * destroys its mutexes. ART is mapped only in that process. */
static int hosts_hwui(void)
{
    return maps_contains("/libhwui.so") || maps_contains("/libart.so");
}

unsigned titan_vk_inject_state(void)
{
    ensure_ready();
    return atomic_load_explicit(&g_state, memory_order_relaxed);
}

const char *titan_vk_inject_symbol(void)
{
    ensure_ready();
    return g_hook_symbol;
}

const char *titan_vk_inject_symbols(void)
{
    ensure_ready();
    return g_hook_symbols;
}

const char *titan_vk_inject_matched_path(void)
{
    ensure_ready();
    return g_matched_path;
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
    if (g_ui_process) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "no Vulkan here: the Android UI owns the vendor driver");
        if (pInstance != NULL)
            *pInstance = VK_NULL_HANDLE;
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
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
    /* winevulkan asks for vkCreateInstance through this entry, so the gate
     * above is bypassed unless the lookup returns the gated function */
    if (g_ui_process && instance == VK_NULL_HANDLE &&
        strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkCreateInstance;
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
