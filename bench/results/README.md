# Recorded benchmark runs

Raw JSONL from `bench/run.sh`, kept so later builds can be compared against the
same device instead of a remembered table.

## `a55-2026-11-16`

`SM-A556E`, platform string `erd8845`, kernel `6.1.162-android14-11`, 4x 2.05 GHz
plus 4x 2.75 GHz. Bundle from the `m2-bundle` prerelease, fresh prefix,
`FEX_DISKCACHE=1`, no CPU pinning, device not thermally controlled. One run:
73 lines, 1 device header, then `titan_bench_x86_64.exe`, `titan_bench_i686.exe`
and `titan_bench_aarch64.exe` in that order.

The medians and the caveats that apply to them are in `docs/bench.md`
("Baseline, Galaxy A55"). Two rows are soft: `lz4_decompress` and
`int_alu_independent` are faster on `i686` than on `x86_64`, which points at
codegen rather than translation cost, and `zstd_compress` on `x86_64` had one
contaminated repeat (`max_ns` 3,060 ms).
