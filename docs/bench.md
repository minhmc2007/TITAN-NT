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
{"bench":"device","uname":"Linux localhost 6.1.75 ... aarch64 Toybox","model":"SM-A556E","platform":"s5e8845","cpu_max_freq_khz":[2000000,2000000,2000000,2000000,2750000,2750000,2750000,2750000]}
```

Example queries with `jq` (optional; the format is plain JSONL):

```sh
jq -c 'select(.bench=="cpu") | {name, ops_per_s, cpu}' bench-results.jsonl
jq -r 'select(.name=="int_mt_8t") | .scaling' bench-results.jsonl
```

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
