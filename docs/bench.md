# CPU benchmark suite

`ci-bench.yml` cross-compiles one C source (`bench/titan_bench.c`) into three
Windows PE executables and packages them with `bench/run.sh`. Nothing is
compiled locally.

| Executable | PE machine | Path on the Galaxy A55 |
| --- | --- | --- |
| `titan_bench_x86_64.exe` | `0x8664` | x86-64 PE translated by FEX (primary target) |
| `titan_bench_i686.exe` | `0x14c` | 32-bit x86 PE through WoW64, also translated by FEX |
| `titan_bench_aarch64.exe` | `0xaa64` | native AArch64 PE, no translation |

The aarch64 build is the baseline that separates FEX translation cost from
Wine's loader/syscall cost. All three link the same LZ4 and Zstd objects and
run the same loops, so `x86_64` vs `aarch64` is a direct translation-overhead
comparison. `i686` is the WoW64 path, not a second native baseline.

## Workloads

`ops_per_s` means different things per workload; the JSONL also carries an
explicit throughput field where it helps.

| `name` | What it measures | `ops_per_s` unit |
| --- | --- | --- |
| `int_alu_dependent` | one dependent chain of add/xor/rotate/multiply | logical integer ops/s |
| `int_alu_independent` | four independent chains of the same ops | logical integer ops/s |
| `fp_double_fma` | dependent `a = a*m + c` in double | multiply+add pairs/s |
| `fp_double_sqrt` | dependent `a = sqrt(a+1)` in double | add+sqrt pairs/s |
| `fp_float_fma` | dependent `a = a*m + c` in float | multiply+add pairs/s |
| `fp_float_sqrt` | dependent `a = sqrtf(a+1)` in float | add+sqrt pairs/s |
| `branch_dependent` | sum of a 1 MiB random byte array with a data-dependent if/else | elements/s |
| `branch_branchless` | the same sum with a sign mask instead of a branch | elements/s |
| `mem_chase` | pointer chase over a 64 MiB random permutation | accesses/s |
| `mem_stream_*` | `memcpy` of 4 KiB, 64 KiB, 1 MiB, 8 MiB, 32 MiB | bytes/s |
| `int_mt_1t`..`int_mt_8t` | `int_alu_independent` on 1/2/4/8 threads | aggregate ops/s |
| `lz4_compress` | LZ4 default compression of a 4 MiB mixed corpus | bytes/s |
| `lz4_decompress` | LZ4 decompression of that frame | bytes/s |
| `zstd_compress` | Zstd level 3 compression of the same corpus | bytes/s |
| `zstd_decompress` | Zstd decompression of that frame | bytes/s |

The branch pair is the point of the branch workload: the dependent variant
keeps a real conditional branch and pays mispredictions; the branchless
variant computes the identical sum with a sign mask
(`mask = (b >> 7) - 1; sum += (b ^ mask) - mask`) and may vectorise. Before
timing, the binary runs one fixed pass of each variant and exits non-zero if
the two checksums differ, so the branchless loop cannot silently measure
different work. The timed checksums are not compared: the two variants
calibrate to different pass counts.

The branch loop carries an empty `asm volatile` in both arms. That is not
decoration: it makes the arms non-speculatable, so LLVM cannot if-convert the
conditional into a `cmov`/`csel` and erase the branch-predictor cost. The
branchless variant has no such barrier.

`mem_chase` builds a random permutation of 1 MiB cache-line nodes (64 MiB
buffer, well past the 4 MiB shared L3) so the hardware stride prefetcher
cannot hide the miss. `mem_stream` reports both `mb_per_s` (decimal MB/s) and
`ns_per_copy`; the size sweep shows L1/L2/L3/DRAM behaviour.

The compression corpus is generated from a fixed seed: alternating 64 KiB
blocks of dictionary text and xorshift32 random bytes. Compression ratio is
the real `compressed_bytes / original_bytes` for each codec, so the LZ4 and
Zstd ratios differ. Both decoders are verified byte-for-byte against the
source before any timing starts.

## Methodology

- **Calibration.** Every workload starts at one iteration and grows the count
  geometrically until one call takes at least the target time (default
  0.35 s), then adds 25% headroom. Nothing is hardcoded, so a fast A78 and a
  FEX-translated x86 binary both land in the 0.2-1 s range.
- **Warm-up.** The calibration calls are themselves warm-up; one more
  untimed call runs at the final iteration count before sampling.
- **Repeats and median.** Five timed repeats by default. The reported `ns` is
  the median, `min_ns`/`max_ns` bracket it, and `repeats` records how many
  samples actually completed. A single scheduler hiccup therefore cannot
  dominate the number.
- **Dead-code elimination.** Every loop returns a checksum that is XORed into
  a `volatile` sink and printed in the JSONL line. The compiler cannot delete
  the work.
- **Fixed data.** Each generated data set has its own fixed seed, so
  `--only mem_chase` and a full `--all` run use the same permutation.
- **CPU utilization.** `cpu` is process user+kernel time from
  `GetProcessTimes` divided by wall time, sampled around each repeat; 1.0 is
  one core busy, 8.0 is all eight cores. For the single-threaded workloads it
  should be close to 1.0, which is also a sanity check that the thread was
  not migrated off or throttled mid-run.
- **Memory.** `peak_rss_kb` is `PeakWorkingSetSize` from
  `GetProcessMemoryInfo`, a process-wide high-water mark, so it only grows
  across the suite. `private_kb` is the current `PrivateUsage` at the moment
  the line is printed. Neither is a per-workload allocation counter. Wine's
  `K32GetProcessMemoryInfo` on the Android bundle leaves `PrivateUsage` at 0;
  the field is kept because a native Windows host fills it.
- **Multithreading.** Thread creation and the start event are outside the
  timed window; the join is inside it, so a worker that starts late counts
  against wall time. Every thread runs the same calibrated iteration count at
  every thread count. `scaling` is aggregate throughput at N threads divided
  by the 1-thread measurement in the same run. On this SoC 1-4 threads are
  Cortex-A78 and 5-8 spill onto Cortex-A55, so scaling above 4 is expected to
  flatten.

## Running on the device

Download the artifact, extract it, and point the runner at the Wine bundle:

```sh
gh run download <run-id> -R minhmc2007/TITAN-NT -n titan-bench -D titan-bench
tar -xzf titan-bench/titan-bench.tar.gz -C titan-bench
cd titan-bench
sh run.sh "$HOME/titan-m2/wine2"
```

`run.sh` takes the bundle directory as `$1` (default `$HOME/titan-m2/wine2`),
uses `$WINEPREFIX` if set (default `$HOME/titan-m2/prefix`), and sets
`WINELOADER`, `PATH` and `WINEDEBUG` the same way the M2 bundle does. It
creates the prefix with `wineboot -i` on first use. Every exe is started as
`/system/bin/linker64 "$WINELOADER" <exe> --all`; PE images are loaded by
Wine, but the Wine loader itself is an app-private ELF and must go through
linker64 on API 29+.

Output is written to `bench-results.jsonl` in the current directory and also
tee'd to the terminal. `RESULTS=/path/file.jsonl` overrides the path.

Useful single-workload runs (no rebuild needed):

```sh
sh run.sh "$HOME/titan-m2/wine2"          # all three exes
# or call one exe directly through the same loader:
/system/bin/linker64 "$HOME/titan-m2/wine2/lib/wine/aarch64-unix/wine" \
  ./titan_bench_x86_64.exe --only int_alu,mem_chase --repeats 7
```

`--list` prints every measurement name, `--only name[,name]` accepts a group
(`int_alu`, `fp`, `branch`, `mem_stream`, `int_mt`, `lz4`, `zstd`) or exact
names, and `--seconds`/`--repeats` adjust the timing budget.

## Reading the JSONL

One object per line. The first line is `{"bench":"start",...}`, the last is
`{"bench":"summary",...}`; everything in between has `"bench":"cpu"`.

```json
{"bench":"cpu","name":"int_alu_dependent","ns":344821000,"ops_per_s":1.1601e+09,"cpu":0.998,"peak_rss_kb":6144,"private_kb":0,"min_ns":341002000,"max_ns":352119000,"iters":100000000,"repeats":5,"checksum":123456789}
```

Common fields:

| Field | Meaning |
| --- | --- |
| `ns` | median wall time of one calibrated run, nanoseconds (lower is better) |
| `ops_per_s` | throughput; unit depends on the workload (higher is better) |
| `cpu` | process user+kernel / wall time for the median run |
| `peak_rss_kb` | process-wide peak working set, KiB |
| `private_kb` | current private commit, KiB |
| `min_ns`, `max_ns` | fastest and slowest repeat |
| `iters` | calibrated iteration count for the median run |
| `repeats` | number of timed samples that completed |
| `checksum` | result folded into the volatile sink |

Workload-specific fields: `mb_per_s`, `size_bytes`, `ns_per_copy` on
`mem_stream_*`; `ns_per_access`, `buffer_mb` on `mem_chase`; `threads`,
`scaling`, `per_thread_ops_per_s` on `int_mt_*`; `mb_per_s`, `ratio`,
`compressed_bytes`, `original_bytes` and `level` on compression. A failed
workload emits `{"bench":"cpu","name":...,"error":"..."}`, increments
`failures` in the summary, and makes the process exit non-zero.

The device header is a plain JSON object (no `#` comments) so the whole file
stays parseable:

```json
{"bench":"device","uname":"Linux localhost 6.1.162-android14-11 ... aarch64 Android","model":"SM-A556E","platform":"erd8845","cpu_max_freq_khz":[2050000,2050000,2050000,2050000,2750000,2750000,2750000,2750000]}
```

Example queries with `jq` (optional; the format is plain JSONL):

```sh
jq -c 'select(.bench=="cpu") | {name, ops_per_s, cpu}' bench-results.jsonl
jq -r 'select(.name=="int_mt_8t") | .scaling' bench-results.jsonl
```

## Baseline, Galaxy A55

One run on the A55 (`SM-A556E`, platform string `erd8845`), bundle from the
`m2-bundle` prerelease, fresh prefix, `FEX_DISKCACHE=1`, no CPU pinning, device
not thermally controlled. Medians of the default repeats. Percentages are
relative to the native `aarch64` column. Raw output:
`bench/results/a55-2026-11-16/bench-results.jsonl`.

| Workload | `aarch64` | `x86_64` | % | `i686` | % |
| --- | --- | --- | --- | --- | --- |
| `branch_branchless` | 2,418 MB/s | 2,600 MB/s | 107.6 | 2,237 MB/s | 92.5 |
| `mem_stream_32m` | 8,297 MB/s | 8,858 MB/s | 106.8 | 6,712 MB/s | 80.9 |
| `mem_stream_4k` | 61,666 MB/s | 53,735 MB/s | 87.1 | 49,380 MB/s | 80.1 |
| `int_alu_dependent` | 2.03 G/s | 1.59 G/s | 78.4 | 1.69 G/s | 83.5 |
| `branch_dependent` | 254.7 MB/s | 150.9 MB/s | 59.3 | 159.4 MB/s | 62.6 |
| `int_mt_1t` | 7.08 G/s | 3.32 G/s | 47.0 | 3.61 G/s | 50.9 |
| `int_mt_8t` | 30.01 G/s (4.24x) | 13.71 G/s (4.12x) | 45.7 | 11.11 G/s (3.08x) | 37.0 |
| `int_alu_independent` | 6.79 G/s | 2.98 G/s | 43.8 | 3.64 G/s | 53.5 |
| `fp_double_fma` | 1.02 G/s | 0.42 G/s | 40.6 | 0.46 G/s | 44.8 |
| `zstd_decompress` | 1,823 MB/s | 777 MB/s | 42.6 | 470 MB/s | 25.8 |
| `zstd_compress` | 186.5 MB/s | 84.1 MB/s | 45.1 | 53.6 MB/s | 28.7 |
| `lz4_decompress` | 3,539 MB/s | 689 MB/s | 19.5 | 853 MB/s | 24.1 |

Reading it:

- The translation tax on scalar ALU code is 2.1-2.3x (`int_mt_1t`,
  `int_alu_independent`). That is one x86 instruction expanding to roughly two
  AArch64 instructions plus register moves, and it is the number to quote.
- Bandwidth-bound work is unaffected (`mem_stream_*` 80-107%): DRAM and L1 do
  not care which ISA issued the load. Values above 100% are llvm-mingw's
  AArch64 codegen against FEX's register allocation on a loop that is waiting on
  memory anyway, not a translation win.
- Unpredictable control flow is where translation costs most
  (`branch_dependent` 59-63%, `lz4_decompress` 19.5%). Each translated branch is
  a guest-PC lookup, so a mispredict pays the guest's mispredict plus the
  dispatch.
- Thread scaling survives translation: 4.12x on `x86_64` against 4.24x native
  on 4x A78 + 4x A55. `i686` at 3.08x is the only real anomaly.
- Two rows are not trustworthy as multipliers. `lz4_decompress` and
  `int_alu_independent` are both *faster* on `i686` than on `x86_64`, which
  cannot happen if the gap were a translation-layer cost, since both go through
  one FEX layer. Treat those `x86_64` figures as a codegen pathology and re-run
  with `--only` before quoting them.
- `zstd_compress` on `x86_64` reported a 3,060 ms `max_ns` against a median in
  the tens of ms, so one repeat was contaminated (throttle or contention). The
  median stands; the spread does not.

Peak RSS on the same run was 6.9 MB native, 14.3 MB `x86_64`, 17.5 MB `i686`.
That is the JIT code cache, and `peak_rss_kb` is a process-wide high-water mark
that only rises across a run, so it is not a per-workload figure.

## Build pins

`ci-bench.yml` fetches release source tarballs inside CI; nothing is vendored.

| Dependency | Version | Source | SHA-256 |
| --- | --- | --- | --- |
| LZ4 | `v1.10.0` | `lz4/lz4` source archive, `lib/lz4.c` + `lib/lz4.h` | `537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b` |
| Zstd | `v1.5.7` | `facebook/zstd` source archive, `lib/common` + `lib/compress` + `lib/decompress` | `37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3` |

Zstd is compiled with `-DZSTD_DISABLE_ASM -DXXH_NAMESPACE=ZSTD_
-DZSTD_LEGACY_SUPPORT=0`; the dictBuilder and CLI sources are not built. LZ4
uses the upstream two-file single-file build. llvm-mingw `20260721` (UCRT)
provides the three cross compilers; the same source is compiled at `-O2` with
`-ffp-contract=off` so multiply-add chains stay two operations on every
target.
