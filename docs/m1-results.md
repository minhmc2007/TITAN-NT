# M1 results

Built by `ci-m1-native.yml` run 43707685081 from `56dc5eb8`. Artifact:
`m1-arm64-artifacts` (`m1_x18_stress`, `m1_futex_waitv_bench`,
`libtitan_forkd.so`, `libmimalloc.a`, `sha256.txt`).

## Static invariants

| Check | Result |
| --- | --- |
| SCS prologues in all three binaries | 0 |
| x18 stores in `m1_x18_stress` | 1 (`stp x18, x30` in `titan_sig_trampoline`) |
| x18 stores in `libtitan_forkd.so` | 1 (same trampoline) |
| x18 stores in `m1_futex_waitv_bench` | 0 |
| `libtitan_forkd.so` ELF type / interpreter | `DYN` / `/system/bin/linker64` |

## Device runs

All runs below executed in `untrusted_app_27` with `Seccomp: 2`, inside the
Termux PRoot rootfs. Syscall policy matches a real app process; **timings do
not** because PRoot traps every syscall.

| Run | Result |
| --- | --- |
| `m1_x18_stress --seconds 15 --threads 3` | 81,928,688 guarded calls, 389,728 signals, 256,027 recovered SIGSEGV, 128,016 recovered SIGILL, `guard_bad=0`, `sig_bad=0` |
| `m1_futex_waitv_bench` multi-object phase | 200,000 epoch transitions, 0 lost wakeups, 2/2 runs |
| `futex_waitv` (449) probe | `-ENOSYS` |
| `FUTEX_WAIT_BITSET` timeout | `ETIMEDOUT` (110) |
| `libtitan_forkd.so --hold 1` | low-VA maps at `0x10000`, `0x10000000`, `0x100000000`; memfd arena visible in exec'd child; x18 signal roundtrip ok; `/proc/self/task` names = `["libtitan_forkd."]`, `art=0` |

PRoot results for the IPC comparison are transport-bound: futex 150–368 µs vs
socket 117–340 µs per roundtrip, both dominated by the ptrace stop. The `≥5x`
gate number must come from a real process (`adb shell`), see below.

## Shell domain (Shizuku, run 43709583627)

`rish` children land in the shell/system cgroup, `Seccomp: 0`, `u:r:shell:s0`.

| Run | Result |
| --- | --- |
| `libtitan_forkd.so --hold 2` | identical to the app-domain run: low-VA ok, exec ack ok, x18 signal ok, 1 thread, `art=0` |
| `futex_waitv` (449) probe | `-ETIMEDOUT`, so 449 is allowed here |
| `m1_futex_waitv_bench` | 200,000 epoch transitions, 0 lost wakeups; ping 0 ack mismatches, 10 waits hit the 10 s budget with `loadavg` 18.7 |
| ping, excluding the 10 s stalls | futex 145 µs vs socket 247 µs per roundtrip |

Every 10 s stall ended with the ack arriving, so they are peer-scheduling
delays, not lost wakeups. Shizuku children run in the system cgroup and lose
the CPU to top-app work while the device is loaded; the ≥5x number has to come
from a foreground app process, where both peers run in `top-app`.

## Deviations from `PLAN.md`

- Syscall 449 is seccomp-blocked for apps (`ENOSYS`), as measured in M0. The
  bench uses one `FUTEX_WAIT_BITSET` epoch word per object set; 449 is only
  probed and reported.
- `libtitan_forkd.so` is an executable PIE carrying a `.so` name; it needs
  `-Wl,--dynamic-linker=/system/bin/linker64`.
- `libmimalloc.a` is built with the x18 flags and shipped, but `forkd` still
  uses bionic malloc. Linking it requires the M2 allocator swap.
- `mimalloc` pin corrected to the peeled commit
  `56dc5eb80145f9a3ba7ca23d47c6a248b9d8586c`; `FEX` and `dxvk` pins corrected
  the same way. `docs/upstream_manifest.md` now records tag commits, not tag
  objects.
- PRoot rejects `O_DIRECTORY` with `EINVAL`; the task-directory scan opens
  without it, which Linux accepts.
- PRoot also inflated one ping round past the 2 s wait window; waits now retry
  against a 10 s per-round budget so a stall is only a failure when no progress
  happens at all.

## Gate commands

Shell domain (no PRoot, real timings; 449 is allowed here):

```sh
gh run download 43707685081 -n m1-arm64-artifacts -D dist/m1
adb push dist/m1/m1_x18_stress dist/m1/m1_futex_waitv_bench \
  dist/m1/libtitan_forkd.so /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/m1_x18_stress \
  /data/local/tmp/m1_futex_waitv_bench /data/local/tmp/libtitan_forkd.so'
adb shell '/data/local/tmp/m1_futex_waitv_bench'
adb shell '/data/local/tmp/libtitan_forkd.so --hold 2'
adb shell '/data/local/tmp/m1_x18_stress --seconds 600 --threads 3'
```

App domain: run the same three binaries from a Termux shell (no proot) so the
process carries `untrusted_app` seccomp; only correctness counters are
meaningful there, not latency.
