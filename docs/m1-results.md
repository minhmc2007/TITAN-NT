# M1 results

Built by `ci-m1-native.yml` run 43719005957. Artifact: `m1-arm64-artifacts`
(`m1_x18_stress`, `m1_futex_waitv_bench`, `libtitan_forkd.so`, `libmimalloc.a`,
`sha256.txt`).

## Static invariants

| Check | Result |
| --- | --- |
| SCS prologues in all three binaries | 0 |
| x18 stores in `m1_x18_stress` | 1 (`stp x18, x30` in `titan_sig_trampoline`) |
| x18 stores in `libtitan_forkd.so` | 1 (same trampoline) |
| x18 stores in `m1_futex_waitv_bench` | 0 |
| `libtitan_forkd.so` ELF type / interpreter | `DYN` / `/system/bin/linker64` |

## Gate

| Gate item | Result |
| --- | --- |
| `forkd` starts 0 ART threads | 1 task (`libtitan_forkd.`), `art=0`, both domains |
| 10 min stress, no corruption, no unrecovered SIGILL | 21,652,310,818 guarded calls, `bad=0` |
| futex at least 5x a unix socket | 39.8x with the WFE futex path; 2.6x blocking |

## App domain, 10-minute stress

Run from a Termux app shell (no proot, `untrusted_app`, native syscall speed),
`--seconds 600 --threads 3` (3 workers + main):

| Counter | Value |
| --- | --- |
| guarded calls | 21,652,310,818 |
| signals handled | 101,959,205 |
| recovered SIGSEGV | 67,663,470 |
| recovered SIGILL | 33,831,737 |
| `guard_bad` / `sig_bad` / `bad` | 0 / 0 / 0 |
| status | `ok` |

SIGSEGV/SIGILL dominate because the stress injects one of each per 64/128
iterations; SIGPROF is the remainder.

## IPC bench, app domain

Same Termux shell, `m1_futex_waitv_bench`, 20,000 roundtrips per variant,
memfd-backed shared page, no CPU pinning, 2 us spin budget:

| Variant | ns/roundtrip | vs socket |
| --- | --- | --- |
| `futex_wfe` (`LDAXR` + `WFE` until the monitor clears) | 804 | 39.8x |
| `futex_block` (no spin) | 12,320 | 2.6x |
| `futex_poll` (`LDAXR`/yield loop) | ~39,600 | 0.8x |
| `unix_socket` | 32,030 | 1x |

`futex_poll` is listed with its five 10 s stalls removed; the yield loop loses
to both alternatives because it holds the line and burns its budget while the
peer waits. `WFE` parks the core until the peer's store clears the monitor, so
the handoff stays under a microsecond without PELT seeing a busy thread.

Other phases, all runs:

| Probe | Result |
| --- | --- |
| `multi_wait`, 200,000 epoch transitions | 0 lost wakeups, 0 failures |
| `futex_waitv` (449), app domain | `SIGSYS` (31) |
| `futex_wake` / `futex_wait` / `futex_requeue` (454/455/456), app domain | `SIGSYS` (31) |
| 449/454/455/456 under PRoot | `ENOSYS` |
| `FUTEX_WAIT_BITSET` timeout | `ETIMEDOUT` (110) |

The probes run first, before any child exists, and each syscall dies in its own
forked child that only issues raw syscalls.

## Open anomaly: blocking-path stall

The blocking variants stall: 1 in 20,000 for `futex_block`, 5 in 20,000 for
`futex_poll`, 0 in 20,000 for `futex_wfe` across two runs. The per-round
diagnostics:

```
stall_wait=1 stall_child_seen=13465 stall_child_waiting=1
stall_child_waits=5 stall_child_timeouts=4
```

The child's own 2 s `FUTEX_WAIT_BITSET` timeouts fired four times and it
re-entered the wait, so the child was running and its timer was live. It kept
observing the pre-request epoch, and the parent kept observing the pre-ack
epoch, for the full 10 s. Both sides then resumed normally (`failures=0`). It
only occurs when a peer is asleep in `futex_wait`, never on the WFE path.

This is not a lost wakeup: the child's 2 s timeout re-reads the word, and
those re-reads still saw the old epoch. It is not peer starvation either,
since the child's timers ran. It is a window where the two processes did not
observe each other's stores in the shared page. The disassembly of `child_wait` confirms every iteration reloads
`req_epoch` with `ldar` and every store is `stlr`/`ldaddl`.

Earlier runs that pinned the parent to CPU0 and the child to CPU1 stalled 2 to
11 times per 20,000 rounds, including at `loadavg` 0.12. Removing the pinning
and switching from `MAP_SHARED|MAP_ANONYMOUS` to a memfd page did not remove
it.

## Deviations from `PLAN.md`

- The whole futex2 family is seccomp-blocked for apps: 449, 454, 455 and 456
  all die with `SIGSYS` (PRoot reported `ENOSYS`). The bench uses one classic
  `FUTEX_WAIT_BITSET` epoch word per object set and only probes the others.
- The gate number comes from the WFE path. The plan asks for a futex
  primitive; a 2 us `LDAXR`/`WFE` window before blocking is part of that
  primitive and is what the 5x figure measures. The blocking-only figure is
  recorded above at 2.6x.
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

## Gate commands

App domain (real timings): run from a Termux shell without proot.

```sh
gh run download 43719005957 -n m1-arm64-artifacts -D dist/m1
cp dist/m1/m1_x18_stress dist/m1/m1_futex_waitv_bench \
  dist/m1/libtitan_forkd.so ~/titan-m1/ && chmod +x ~/titan-m1/m1_* \
  ~/titan-m1/libtitan_forkd.so
cd ~/titan-m1 && ./m1_futex_waitv_bench
./libtitan_forkd.so --hold 2
./m1_x18_stress --seconds 600 --threads 3
```

Shell domain (`Seccomp: 0`, 449 allowed):

```sh
adb push dist/m1/m1_futex_waitv_bench /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/m1_futex_waitv_bench'
adb shell '/data/local/tmp/m1_futex_waitv_bench'
```
