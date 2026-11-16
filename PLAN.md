
# PROJECT TITAN-NT — Engineering Execution Plan (Exynos 1480 / Xclipse 530)

## 0. Mission & Strict Scope
Build **TITAN-NT**, an unrooted Android Windows runtime engineered exclusively for **Samsung Galaxy A55 (`SM-A556E`, SoC `Exynos 1480` / `s5e8845`, GPU `Xclipse 530` / "TITAN")** under active TEC cooling.

Do **NOT** add compatibility layers, fallbacks, codepaths, or abstractions for Qualcomm Adreno/Turnip, ARM Mali/PanVK, Intel/MediaTek SoCs, or virtual machines (AVF/KVM). Every optimization targets this exact silicon.

---

## 1. Host Environment & CI-Only Workflow (CRITICAL)
The local development host is **Arch Linux ARM** with limited resources.

1. **ZERO Local Compilation:**
   - **FORBIDDEN locally:** Running `clang`, `gcc`, `cmake --build`, `ninja`, `meson compile`, `./gradlew assemble`, downloading/installing Android NDK/SDK, or cloning massive full-history upstream trees unnecessarily.
   - Local host is strictly for editing files, `git`, `gh` CLI (`gh repo fork`, `gh run watch`, `gh run view --log-failed`), and optional light `adb` device probingtions CI (`ubuntu-latest`):**
   - All native NDK builds, Mesa/Wine/FEX cross-compiles, and APK builds MUST execute on GitHub Actions (`ubuntu-latest` x86_64 runners).
   - Use `gh` CLI to fork upstream dependencies (`radv-xclipse`, `FEX`, `proton-wine`, `dxvk`, `vkd3d-proton`) into the user's GitHub namespace whenever patches are required, and pin exact commits/tags.
   - Keep CI fast and under GitHub runner disk limits (~14 GB free):
     - Run a disk-cleanup step before heavy builds (`radv-xclipse`, `wine`, `fex`).
     - Split heavy upstream builds into dedicated cached workflows (`ci-deps-*.yml`) that upload prebuilt `.tar.zst` sysroot artifacts to GitHub Releases/Artifacts + `ccache`, so downstream milestone workflows compile in `< 5 minutes`.
2. **Commit, Watch CI, and HARD STOP at Every Milestone:**
   - Workflow per step: edit → `git add` / `git commit` / `git push` → trigger/wait via `gh run watch` → if failed, fetch logs via `gh run view --log-failed`, fix, recommit.
   - **HARD STOP AT GATE:** Once the current milestone's GitHub CI workflow turns **GREEN** and uploads test binaries/APKs as artifacts, **STOP IMMEDIATELY**. Do NOT start the next milestone. Output the exact `gh run download` + `adb` commands for the user to push and test on their Galaxy A55, and wait for the user's device logs.

---

## 2. Agent Coding Discipline (MANDATORY)
1. **Zero AI-Slop Code & Comments:**
   - **FORBIDDEN:** Line-by-line narration (`// Open the file`, `// Loop over threads`, `// Allocate memory`), comments restating symbol names, decorative ASCII banners, emojis inside source/build/workflow files, apology/deferral comments (`// TODO: handle later`, `// Simplified for production`, `// Placeholder`).
   - **ALLOWED COMMENTS ONLY:** Terse explanations of **WHY** a non-obvious hardware invariant, ABI constraint, memory barrier, or kernel workaround exists (e.g., `/* Cortex-A55 lacks FEAT_LRCPC; never emit LDAPR */`, `/* x18 holds Windows TEB inside ARM64EC; swap before calling Bionic SCS libs */`).
   - **Zero Unrequested Docstrings:** Do not write multi-line docstrings for self-explanatory internal functions.
2. **No Fake Mocks or Silent Stubs:**
   - Never fake passing tests, never stub out failing syscalls/Vulkan calls with `return 0`, and never mask CI failures with `|| true`.

---

## 3. Hard Target Hardware & OS Constraints
- **Device:** Samsung Galaxy A55 (`SM-A556E`, unrooted, Android 14–16, `Linux 6.1.x` GKI, `4 KB` host pages; verify `sysconf(_SC_PAGESIZE)` at startup).
- **CPU (`ARMv8.2-A` asymmetric big.LITTLE, 4 MB shared L3):**
  - `CPU 0–3`: 4× Cortex-A55 @ 2.00 GHz (**NO `LDAPR`/`FEAT_LRCPC`, NO `LDAPUR`/`FEAT_LRCPC2`, NO `FlagM2`, NO `SVE2`**).
  - `CPU 4–7`: 4× Cortex-A78 @ 2.75 GHz (`ARMv8.2-A + LSE`; has `LDAPR`, but **cross-cluster `LDAPR` emission is FORBIDDEN** to prevent `SIGILL` on A55 migration).
- **Memory & Bus:** 32-bit (`2×16-bit`) LPDDR5 @ 3200 MHz = **`25.6 GB/s`** peak bandwidth; 8 GB (common) or 12 GB shared RAM with aggressive Samsung LMKD.
- **GPU (`Xclipse 530` / "TITAN"):** 1 WGP / 2 CUs = 128 SP, 8 TMU, 8 ROP @ 1300 MHz (`332.8 GFLOPS FP32`), **NO hardware ray tracing**.
- **Thermal Assumption:** External TEC Peltier cooler active → keep sustained clocks (`A78 @ 2.75 GHz`, `GPU @ 1.30 GHz`) via `ADPF PerformanceHintManager` + `appCategory="game"`.

---

## 4. Hard Ban List (Never Use)
- `PRoot`, `ptrace` syscall interception, `glibc`, Ubuntu/Debian rootfs, X11, XWayland, Wayland compositor.
- `io_uring` (blocked by Android `seccomp-bpf`), `userfaultfd` (blocked for `untrusted_app` by SELinux/`vm.unprivileged_userfaultfd`), `CONFIG_SYSVIPC` (`shmget`/`semget`/`msgsnd` → `-ENOSYS` on Android GKI), `/dev/ntsync`, custom kernels, root/Magisk/su.
- Calling `fork()` inside the ART JVM process (`JNI_OnLoad`) without immediate `execve()`.
- Inheriting a live `VkInstance`, `/dev/kgsl`, or `sync_file` FD across `fork()`.
- Advertising `AVX`/`AVX2` in FEX CPUID (keep 128-bit SSE→NEON to fit 4 MB L3).
- In-place rewriting of `LDR [Xn, #imm]` instructions into `LDAPR`/`STLR`.

---

## 5. Critical Engineering Invariants

### 5.1 Toolchain & `x18` ABI Boundary (TEB vs. Bionic Shadow Call Stack)
- Android system DSOs (`libc.so`, `libaaudio.so`, `libmediandk.so`, `libandroid.so`) use `x18` for Shadow Call Stack (SCS). ARM64EC Wine/FEX/DXVK/VKD3D requires `x18` to hold the Windows TEB.
- **Rule:**
  1. Compile ALL self-built native targets (`libtitan_forkd.so`, `titan_hal_shim`, `wine`, `fex_unixlib`, `dxvk`, `vkd3d-proton`, AND **`radv-xclipse` `vulkan.radeon.so`**) with NDK (`>= r27`) using:
     `-ffixed-x18 -fno-sanitize=shadow-call-stack -march=armv8.2-a+lse`
  2. Do **NOT** call stock Bionic `libc.so` non-leaf functions (`malloc`, `dlopen`, `pthread_*`, etc.) directly from `-ffixed-x18` code without swapping `x18`. Verify stock device `libc.so`/`libaaudio.so` prologue instructions in M0.
  3. Route internal allocations through self-compiled `mimalloc` (`-ffixed-x18 -fno-sanitize=shadow-call-stack`) and raw Linux `syscall` wrappers, and route ALL calls to external Android system DSOs (`libaaudio`, `libmediandk`, `libandroid`, and unavoidable `libc` entrypoints) through `x18_guard.S` swapping Windows TEB (`x18`) ↔ per-thread `__thread void* titan_scs_ptr` (4 KB guard-mapped SCS stack stored via ELF TLS / `TPIDR_EL0`).
  4. **Async Signal Safety (`SA_ONSTACK`):** Every signal handler (`SIGSEGV`, `SIGPROF`, `SIGILL`) MUST register a dedicated `sigaltstack` and enter through a naked/assembly stub (zero compiler SCS prologue) that checks whether `x18` holds TEB vs. SCS pointer, saves `x18` to `ucontext`, loads `titan_scs_ptr` into `x18`, calls the C handler, and restores `x18` on `rt_sigreturn`.

### 5.2 Process Model (`libtitan_forkd.so`)
- UI lives in Android ART process (`TitanActivity`). Runtime never runs inside ART.
- Package daemon as `jniLibs/arm64-v8a/libtitan_forkd.so` and spawn it from Kotlin via `ProcessBuilder`/`execve()`.
- `forkd` has zero ART threads and clean low-4 GB virtual address space (allowing 1:1 32-bit WoW64 reservations without a base-register offset).
- Child Win32 processes (`fork()`ed before any Vulkan init) create their own `VkInstance` post-`fork()`.

### 5.3 NT Sync, Broker & VFS (`GKI 6.1`)
- **Fast Sync:** Map a session-wide `ASharedMemory_create` (`memfd`) arena across child processes. Map `KEVENT`, `KMUTANT`, `KSEMAPHORE` to cache-line-aligned atomic state words and execute `NtWaitForSingleObject` / `NtWaitForMultipleObjects` in userspace via **`futex_waitv`** (syscall 449, verified on GKI 6.1) + `FUTEX_WAIT_BITSET` fallback.
- **Broker (`wineserver-lite`):** A single lightweight thread inside `forkd` pinned to `CPU 0–3` (A55), communicating over a lock-free shared-memory ring strictly for cross-process structural state (`HWND` tree, `NamedPipe`, dead-process handle cleanup, atomic registry writes).
- **VFS & Async I/O:** Containers must reside in app-private F2FS (`/data/user/0/.../files/containers/`), never `/sdcard` FUSE. Maintain a live RW-locked case-insensitive hash index in `ntdll` (updated immediately on `NtCreateFile`/`NtDeleteFile`/`NtSetInformationFile`). Execute overlapped/IOCP I/O on a `preadv`/`pwritev` worker pool pinned to `CPU 0–3` (A55).
- **4KB-on-16KB Subpage Safety:** Use hardware `mprotect` on 4 KB kernels; if ever running on a 16 KB kernel, never `mprotect(PROT_READ)` a mixed RW/RO 16 KB page (prevents `-EFAULT`), enforcing subpage RO/guard checks in JIT stores.

### 5.4 CPU Translator (`TitanFEX`) & Core Pinning
- Base on **FEXCore 2609+** loaded by Proton 11 via `load_unixlib_by_name` (`libarm64ecfex.so` / `libwow64fex.so`).
- Enforce `ARMv8.2-A + LSE` codegen (`SupportsRCPC = false`, `SupportsFlagM2 = false`, `AVX = false`).
- Enable `FEX_DISKCACHE=1` (`.foz`) on F2FS + build an offline `.pdata` + `GFIDS` pre-translator (`titan_aot_warm`).
- Hard thread affinity (`sched_setaffinity`):
  - `CPU 4–7` (4× A78 @ 2.75 GHz): Main game thread, DXVK/VKD3D submit threads, RADV submission, AAudio callback.
  - `CPU 0–3` (4× A55 @ 2.00 GHz): FEX background JIT/AOT optimizer, RADV ACO shader compiler workers, `wineserver-lite`, F2FS `preadv` pool.

### 5.5 Graphics (`TitanVK` — Xclipse 530 Only)
- **Driver:** Self-compiled `radv-xclipse` (`vulkan.radeon.so`, Mesa 27 RADV + Valve ACO) with Xclipse 530 (`TITAN`) stability fixes (`ordered memory access in shaders`, `full depth cache flush`, GS register fix, VKD3D `O_CLOEXEC`/sync-file FD leak fix, `VK_NV_raw_access_chains`, `VK_AMD_anti_lag`), Ray Tracing disabled.
- **Mandatory DXVK / VKD3D-Proton Configuration:**
  - `BCn` wrapper/DXVK emulation **OFF** (`radv-xclipse` handles BC1–BC7 internally; double-BCn is fatal to `25.6 GB/s` bus).
  - `dxvk.enableDescriptorBuffer = False` and `dxvk.enableDescriptorHeap = False`.
  - Default `dxgi.maxDeviceMemory = 2560` (3072 on 12 GB SKU) to prevent Samsung LMKD kills.
- **Zero-Copy Display + RDNA FP16 Spatial Upscaler:**
  - `winandroid.drv` maps `HWND` directly to `ASurfaceControl` / `ANativeWindow` backed by `AHardwareBuffer` swapchain + `AChoreographer`.
  - Insert an RDNA FP16 Compute **FSR 1.0 / CAS** pass right before `vkQueuePresentKHR` (`540p`/`720p` internal render → `1080×2340` display).
- **Media & Audio:** `WASAPI`/`XAudio2` → `AAudio MMAP`; `MediaFoundation` → `MediaCodec` Exynos MFC decoder.

---

## 6. Repository Layout
```text
titan-nt/
├── PLAN.md
├── .github/workflows/          # All compilation runs here (ci-m0-probe, ci-m1-forkd, ci-deps-*, ci-apk)
├── scripts/                    # probe_device.sh + CI helper scripts
├── native/
│   ├── common/                 # Raw syscall helpers, logging, page-size probe, atomic ring
│   ├── hal_shim/               # x18_guard.S, sig_trampoline.S, Bionic DSO wrappers, aaudio_mmap, mediacodec_mf
│   ├── forkd/                  # libtitan_forkd.so, clean VA init, wineserver-lite, futex_waitv, live F2FS VFS
│   ├── winandroid/             # winandroid.drv (ASurfaceControl / AHardwareBuffer / AChoreographer / Input)
│   ├── titan_vk/               # FSR1/CAS FP16 compute pass, DXVK/VKD3D TITAN config enforcer, Fossilize loader
│   └── tools/                  # titan_aot_warm (.pdata/.foz pre-warmer), sigprof_sampler
├── android/                    # Minimal Kotlin host (built in CI only after M1/M2 gates pass)
└── tests/                      # Standalone C++ & Win32 verification harnesses per milestone
```

---

## 7. Phased Milestones & CI-to-Device Exit Gates

### Milestone 0: CI Bootstrap, Upstream Pinning & Device Probe
- **Tasks:**
  1. Verify `gh auth status` and repo remote; verify GitHub Actions works with a minimal smoke workflow.
  2. Discover and pin exact upstream repos/commits (`radv-xclipse`, `FEX`, `proton-wine`, `dxvk`, `vkd3d-proton`, `mimalloc`) via `gh api` / `git ls-remote` (use `gh repo fork` into the user's account for any repo requiring custom patches; do NOT run heavy clones/builds locally).
  3. Generate `scripts/probe_device.sh` (plus an `ubuntu-latest` CI workflow `.github/workflows/ci-m0-probe-helper.yml` if a tiny probe binary is needed to test `futex_waitv` syscall 449 under `untrusted_app` or inspect pulled `libc.so` via CI `llvm-objdump`).
- **Stop & Handover:** Commit & push, wait for CI green, then **STOP** and instruct the user to run `scripts/probe_device.sh` via `adb` on their Galaxy A55 and paste back `device_profile.json` + stock `libc.so`/`libaaudio.so` prologue check.

### Milestone 1: Clean `forkd`, `-ffixed-x18` Build & Async-Signal `x18_guard` Stress Harness
- **Tasks:**
  1. Write `native/common`, `native/hal_shim` (`x18_guard.S`, `sig_trampoline.S`), `native/forkd` (`libtitan_forkd.so`), `tests/m1_x18_stress.cpp`, and `tests/m1_futex_waitv_bench.cpp`.
  2. Create `.github/workflows/ci-m1-native.yml` using Android NDK r27+ (`-ffixed-x18 -fno-sanitize=shadow-call-stack -march=armv8.2-a+lse`) to build `mimalloc`, `libtitan_forkd.so`, and both test binaries, uploading them as workflow artifacts (`m1-arm64-artifacts`).
  3. Commit, push, `gh run watch`, fix any CI compile/link failures until green.
- **Stop & Handover:** **STOP** and provide exact `gh run download` + `adb push` + `adb shell` commands for the user to run the 10-minute `m1_x18_stress` and `m1_futex_waitv_bench` tests on Galaxy A55.
- **Exit Gate (Verified by User on A55):** 0 ART threads inside `forkd` `/proc/self/task`, 10 minutes `m1_x18_stress` with **0 crashes / 0 TEB corruption / 0 `SIGILL`**, and `futex_waitv` beats Unix socket IPC by ≥5×.

### Milestone 2: CI Prebuilts (`Wine ARM64EC` + `FEXCore`) & Headless Win32/Win64 Verification
- **Tasks:**
  1. Fork/configure upstream `proton-wine` (ARM64EC Bionic, `-ffixed-x18 -fno-sanitize=shadow-call-stack`) and `FEX` (`2609+`, `SupportsRCPC=false`, `SupportsFlagM2=false`, `AVX=false`, `FEX_DISKCACHE=1`) via `.github/workflows/ci-deps-cpu.yml` (with disk cleanup + `ccache` + GitHub Release artifact caching).
  2. Implement `titan_ntsync` (`futex_waitv`), `wineserver-lite`, live F2FS case-insensitive VFS, A78/A55 cluster pinning, and build Win32/Win64 multi-threaded CLI test `.exe`s in CI (`mingw-w64`).
  3. Commit, push, watch CI until green.
- **Acceptance items (from the M1 review, binding):**
  1. `titan_ntsync` maps `KEVENT`/`KMUTANT`/`KSEMAPHORE` to generation-tagged slots in one `memfd` arena; handles carry slot+generation so a recycled slot never satisfies a stale handle.
  2. `NtWaitForMultipleObjects` with `bWaitAll` performs one atomic check-and-acquire pass: no semaphore decrement and no auto-reset event consumption unless every handle is acquirable in the same critical section.
  3. Cross-process waits use `FUTEX_WAIT_BITSET` without `FUTEX_PRIVATE_FLAG`; a mutex whose owner process died returns `WAIT_ABANDONED` and is acquired by the waiter.
  4. Per-object spin budget is wall-clock (`CNTVCT_EL0`, 2 us) with `LDAXR`/`WFE`; two consecutive waits that sleep drop that object's budget to 0.
- **Stop & Handover:** **STOP** and give `gh run download` + `adb` run commands for headless `cmd.exe` + Win32/Win64 test harness on Galaxy A55.
- **Exit Gate:** Both Win64 and Win32 WoW64 test executables pass 100% under `forkd` on A55, generating `.foz` disk cache on F2FS with 0 `wineserver` socket hops in `NtWait*` hot paths.

### Milestone 3: CI `radv-xclipse` + `winandroid.drv` + Minimal Test APK (DX11 / DX12)
- **Tasks:**
  1. Fork & build `radv-xclipse` (`vulkan.radeon.so` compiled `-ffixed-x18 -fno-sanitize=shadow-call-stack`, Xclipse 530 TITAN fixes, RT disabled) + ARM64EC `DXVK` / `VKD3D-Proton` (`BCn=OFF`, `DescriptorBuffer=False`, `DescriptorHeap=False`, `maxDeviceMemory=2560`) in `.github/workflows/ci-deps-gpu.yml`.
  2. Implement `winandroid.drv` (`ASurfaceControl`/`ANativeWindow` + `AHardwareBuffer` + `AChoreographer` + `AInputQueue`) and minimal Kotlin `TitanSurfaceActivity` APK assembled in `.github/workflows/ci-m3-apk.yml`.
  3. Commit, push, watch CI until green.
- **Stop & Handover:** **STOP** and give `gh run download` + `adb install` commands for DX11 + DX12 sample validation on Galaxy A55.
- **Exit Gate:** DX11 and DX12 samples render cleanly at 60/120 Hz on Galaxy A55 with zero black screens, zero `sync_file` FD leaks over 15 min, and `RADV_XCLIPSE_PERF=1` active.

### Milestone 4: RDNA FP16 FSR1, `.pdata` Pre-Warmer, ADPF & Telemetry HUD
- **Tasks:**
  1. Implement `titan_vk` FP16 Compute **FSR 1.0 / CAS** (`540p`/`720p` → `1080×2340`), `tools/titan_aot_warm` (`.pdata` + `GFIDS` → `.foz`), `sigprof_sampler` (1 kHz guest-RIP), `ADPF PerformanceHintManager`, `AAudio MMAP`, `MediaCodec`, and HUD overlay (`A78/A55/GPU clocks`, `RADV_XCLIPSE_PERF=1`, `SIGPROF` hotspot).
  2. Build final APK in CI, commit, watch until green.
- **Stop & Handover:** **STOP** and give `gh run download` + `adb install` commands for real game tests (**Resident Evil 2 Remake**, **Red Dead Redemption 1 PC**) under TEC cooling.
- **Exit Gate:** Stable 30+ FPS frame-pacing at 540p/720p+FSR under TEC cooling with sustained clocks, no OOM/LMKD kills, and warm cache stutter reduction verified.
```

---

### 2. Master Kickoff Prompt cho Claude Opus 5.5 (Max effort)

Bạn mở terminal ngay trong thư mục repo (đã login sẵn `gh auth login` và `git remote`), bật Claude Opus 5.5 max rồi dán nguyên văn prompt này:

```text
Read `PLAN.md` completely and treat it as a binding engineering contract.

Strict rules before you touch anything:
1. HOST CONSTRAINT: This local machine is Arch Linux ARM. NEVER install Android NDK/SDK locally, NEVER run any local C/C++/Rust/Gradle/Meson/Ninja compilers or builds, and NEVER clone huge upstream repos locally unless strictly doing lightweight metadata/header checks. ALL compilation, cross-compiling, and APK assembly MUST happen on GitHub Actions CI (`ubuntu-latest`) controlled via `git` and `gh` CLI.
2. ZERO AI-SLOP RULE: Write ZERO narrative comments explaining what code does, ZERO placeholder/TODO stubs, ZERO emojis or ASCII banners in code/workflows, and ONLY terse comments explaining non-obvious hardware/ABI invariants (`x18` TEB vs SCS, Cortex-A55 missing `LDAPR`, etc.).
3. ONE MILESTONE AT A TIME + HARD STOP: You must work ONLY on the current active milestone, push to GitHub, monitor CI with `gh run watch` (and fix failures via `gh run view --log-failed`), and then **STOP** as soon as CI passes so I can download the artifacts via `gh run download` and test them on my Galaxy A55 via `adb`. Do NOT scaffold future milestones or Kotlin UI prematurely.

Begin ONLY with **Milestone 0**:
1. Check `git status`, `git remote -v`, and `gh auth status`.
2. Inspect upstream repositories for `radv-xclipse`, `FEX`, `proton-wine` (ARM64EC), `dxvk`, `vkd3d-proton`, and `mimalloc` via `gh api` / `git ls-remote` and record exact pinned URLs/commits inside `docs/upstream_manifest.md` (use `gh repo fork` if we need forked repos under my GitHub account).
3. Create `scripts/probe_device.sh` (and if needed, a tiny `.github/workflows/ci-m0-probe.yml` building a minimal NDK probe binary to verify `futex_waitv` syscall 449 under `untrusted_app` and inspect `/system/lib64/libc.so` + `libaaudio.so` for `x18` SCS instructions).
4. Commit, push, watch CI until green, then **STOP** and give me the exact commands to pull the probe/run `scripts/probe_device.sh` on my Galaxy A55 and send you the output.
```

---

