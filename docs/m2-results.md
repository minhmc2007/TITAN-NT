# M2 results

Built by `ci-deps-cpu.yml` run 43846262676 (Wine + FEX) and `ci-m2-win.yml` run
43850212825 (device bundle `titan-m2-wine`, `titan-m2-wine.tar.gz`, 181 MB).
The same tree was republished by run 43887571813 as the `m2-bundle` prerelease;
the device results below are from that asset (181,484,985 bytes).

## Static invariants

| Check | Result |
| --- | --- |
| Wine install tree | 1020 `aarch64-windows` modules, 1007 `i386-windows` |
| `cmd.exe`, `wow64.dll`, `wow64win.dll` in `aarch64-windows` | present |
| Loader ELF type / preloader | `DYN` (PIE) / none |
| `kernelbase.dll` | `coff-arm64x` (ARM64X, so ARM64EC and aarch64 share it) |
| `libarm64ecfex.dll`, `libwow64fex.dll` | machine `0xaa64`, built in ~3 min |
| fsync symbols in `bin/wineserver` / `ntdll.so` | `fsync: up and running.` / `fsync.shm` |

## Gate

| Gate item | Result |
| --- | --- |
| Win64 executable, 100% | 10/10 checks, 0 failures |
| Win32 WoW64 executable, 100% | 10/10 checks, 0 failures |
| `.foz` disk cache on F2FS | 6 files, 7.6 MB, written to the prefix on `f2fs` |
| 0 `wineserver` socket hops in `NtWait*` | 144 ns (Win64) / 319 ns (Win32) per signalled wait |

The gate's "under `forkd`" is not applicable to this path: `forkd` is the ART
thread shim for the APK host, and Wine's loader never enters it. Both
executables ran in a Termux app process (`untrusted_app`, app seccomp active),
which is the domain the gate is about. All four clauses are met.

## Wait latency

`WaitForSingleObject` on an already-signalled manual event, 200,000 rounds, no
CPU pinning:

| Bits | Server-side sync (run 43839750678) | fsync (run 43850212825) | fsync, second run |
| --- | --- | --- | --- |
| 64 | 51,062 ns | 146 ns | 144 ns |
| 32 | 53,204 ns | 326 ns | 319 ns |

51 us is a wineserver socket round trip; 144 ns is the in-process object scan.
The 32-bit path is 2.2x the 64-bit one because the i686 binary reaches the same
code through WoW64 and FEX. The two fsync columns are separate device runs, so
the 1-2% spread is run-to-run noise rather than a code change.

## `.foz` disk cache

FEX only writes the cache when `FEX_DISKCACHE=1` is exported; without it the
translated blocks live and die in memory, which is why the first hello-world run
left no files. The benchmark suite (`ci-bench.yml` run 43856591013) was run with
the switch on, and the cache appeared inside the prefix:

| Path (relative to `$WINEPREFIX`) | Files | Size |
| --- | --- | --- |
| `drive_c/windows/DiskCache/85172ccfbd1234d5/` | 2 | 4.8 MB |
| `drive_c/users/steamuser/AppData/Local/fex-emu/DiskCache/85172ccfbd1234d5/` | 4 | 2.8 MB |

Each entry is a `RWCacheDB_<hash>.foz` plus a `RWCacheDB_<hash>_idx.foz` index,
written after the run finished. `stat -f` on the prefix reports `f2fs`, so the
gate's "on F2FS" holds; the cache is not on a tmpfs or an emulated filesystem.
The two roots are FEX's Windows data directory (`%LOCALAPPDATA%\fex-emu`) and
the Wine-side system cache, both inside the same prefix.

## Real x86 applications

Beyond the sync harness, stock 32-bit and 64-bit x86 PE hello-world binaries run
under WoW64 plus FEX from the same prefix. That exercises the PE loader,
`kernel32`/`ntdll` thunks and translation for both bitnesses, not just the wait
primitives the harness measures. Measured throughput against the native AArch64
baseline is in `docs/bench.md`; the median `x86_64` result is 40-100% of native
depending on whether the workload is bound on memory or on translated control
flow.

## fsync on Android

`deps/patches/wine/0005-fsync-android-epoch.patch` replaces the Amphora commit
that only turned fsync off. Two bionic/Android limits had to go:

- `shm_open` does not exist, so the shared page is a file (`fsync.shm`) in the
  config directory, opened by the server with `openat(config_dir_fd, ...)` and
  by each client from `config_dir`. It is still one `MAP_SHARED` mapping, which
  is what keeps futexes inode-keyed and therefore cross-process.
- `futex_waitv` (449) is `SIGSYS` in app processes, so shm index 0 (never
  allocated to an object) carries a wake epoch. Signallers store the object,
  bump the epoch, wake it; waiters read the epoch before scanning the objects
  and sleep on it with `FUTEX_WAIT_BITSET`. A signal between the scan and the
  wait makes the wait fail with `EAGAIN` instead of being lost. This wakes all
  waiters on every signal, which costs more than `futex_waitv` did, but keeps
  `NtWait*` out of wineserver.

Non-Android keeps `shm_open` and `futex_waitv` and still requires `WINEFSYNC=1`.
Android enables fsync by default; `WINEFSYNC=0` disables it.

## `titan_ntsync`

The four M2 acceptance items (generation-tagged arena, atomic `WaitAll`,
cross-process `FUTEX_WAIT_BITSET` plus `WAIT_ABANDONED`, wall-clock spin budget)
are implemented in `native/ntsync` and pass 78/78 on device. Wine's shipped wait
path is fsync rather than `titan_ntsync`; the semantics match where they overlap,
the implementations are separate. `titan_ntsync` remains the reference and the
fallback if fsync has to be turned off with `WINEFSYNC=0`.

## Known defects

- `wineboot`'s ARMNT pass fails: `C:\windows\sysarm32\rundll32.exe: c0000135`.
  `--enable-archs` builds `i386`, not `arm`, so `wineboot` skips that machine and
  the i386/arm64ec prefix install still completes. Win32 and Win64 are unaffected.
- `Failed to open /etc/machine-id` is printed by Wine on Android; there is no
  machine-id file and nothing depends on it here.
- The M1 blocking-path stall is still unexplained (1/20k `futex_block`, 5/20k
  `futex_poll`); see `docs/m1-results.md`.
