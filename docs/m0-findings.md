# M0 findings — SM-A556E

Measured on the device 2026-11-15. Android 17 (SDK 37), kernel `6.1.162-android14-11`.

| Check | Result |
| --- | --- |
| page size | 4096 |
| cores | 8; affinity masks `0-3` and `4-7` both accepted |
| max clock | cpu0-3 `2050000` kHz, cpu4-7 `2750000` kHz |
| `memfd` + `MAP_SHARED` visible after `fork` | yes |
| free VA at `0x10000`, `0x100000`, `0x1000000`, `0x40000000`, `0x80000000`, `0x100000000` | yes, app and shell domain |
| lowest mapping | shell `0x5df5a58000`, app `0x200000` |
| `futex_waitv` syscall 449, shell domain | `ret 0`, 20.6 ms |
| `futex_waitv` syscall 449, untrusted_app | `ENOSYS` |
| `FUTEX_WAIT_BITSET`, untrusted_app | `ret 0`, 20.5 ms |
| `kcmp` | shell `EINVAL`, untrusted_app `ENOSYS` |
| `bpf` / `perf_event_open` | `EPERM` / `EFAULT` in both domains |
| SCS x18 prologues in bionic | 0 in `libc`, `libaaudio`, `libandroid`, `libmediandk` |

## futex_waitv is unavailable to the app domain

Same binary, same arguments: shell (`Seccomp: 0`) returns 0, `untrusted_app`
(`Seccomp: 2`) returns `ENOSYS`. `kcmp` splits the same way while `bpf` and
`perf_event_open` agree in both domains, so this is the seccomp policy, not proot
forwarding and not a missing kernel syscall. Kernel-side argument rules, proven in
the shell domain: every waiter needs `FUTEX_32` and the timeout is an **absolute**
`CLOCK_MONOTONIC` deadline.

Consequence for `PLAN.md` §5.3: `forkd` is spawned from the app, so syscall 449 is
gone. Multi-object waits must use `FUTEX_WAIT_BITSET` (98), which works in both
domains: a waiter waits on one epoch word and signalers bump it.

## Bionic does not use x18 for SCS on this build

The scanner is validated against hand-encoded `str x18, [x18], #8` /
`ldr x18, [x18, #-8]!` words and llvm-objdump decodes them as such, yet all four
stock DSOs have zero hits across 938 KB of executable segments. So x18 is free to
hold the TEB across bionic calls. `PLAN.md` §5.1 still stands for self-built code
and the loader, but the `x18_guard` swap becomes a cheap assertion rather than a
hot-path cost; M1 must prove no bionic call path clobbers x18 (linker, `setjmp`,
signal return).

## Other deltas from the plan

- Device is Android 17, not 14-16. Pages are 4 KB today; the 16 KB subpage rule stays unexercised.
- Kernel is 6.1.162 with `futex_waitv` present; the block is policy, not kernel.
