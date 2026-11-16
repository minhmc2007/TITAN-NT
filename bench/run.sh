#!/system/bin/sh
# Device entry point for the CPU benchmark artifact. App-private ELFs cannot be
# execve()ed on API 29+, so the Wine loader starts through linker64.
set -eu

BUNDLE=${1:-$HOME/titan-m2/wine2}
export WINEPREFIX=${WINEPREFIX:-$HOME/titan-m2/prefix}
export WINELOADER="$BUNDLE/lib/wine/aarch64-unix/wine"
export PATH="$BUNDLE/bin:$PATH"
export WINEDEBUG=${WINEDEBUG:--all}

HERE=$(cd "$(dirname "$0")" && pwd)
RESULTS=${RESULTS:-$PWD/bench-results.jsonl}

[ -x /system/bin/linker64 ] || { echo "no /system/bin/linker64" >&2; exit 1; }
[ -f "$WINELOADER" ] || { echo "no wine loader at $WINELOADER" >&2; exit 1; }

jesc() {
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/[[:cntrl:]]/ /g'
}

prop() {
    if command -v getprop >/dev/null 2>&1; then
        getprop "$1" 2>/dev/null | head -1 | jesc
    fi
}

{
    printf '{"bench":"device","uname":"%s"' "$(uname -a | jesc)"
    printf ',"model":"%s"' "$(prop ro.product.model)"
    printf ',"platform":"%s"' "$(prop ro.board.platform)"
    printf ',"cpu_max_freq_khz":['
    first=1
    for f in /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq; do
        [ -r "$f" ] || continue
        v=$(cat "$f")
        [ -n "$v" ] || continue
        if [ "$first" = 1 ]; then first=0; else printf ','; fi
        printf '%s' "$v"
    done
    printf ']}\n'
} | tee "$RESULTS"

if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "creating prefix at $WINEPREFIX" >&2
    /system/bin/linker64 "$WINELOADER" wineboot -i
    /system/bin/linker64 "$BUNDLE/bin/wineserver" -w
fi

TO=
if [ -x /system/bin/timeout ]; then TO=/system/bin/timeout; fi

run_bench() {
    if [ -n "$TO" ]; then
        "$TO" 900 /system/bin/linker64 "$WINELOADER" "$1" --all
    else
        /system/bin/linker64 "$WINELOADER" "$1" --all
    fi
}

rc=0
for exe in titan_bench_x86_64.exe titan_bench_i686.exe titan_bench_aarch64.exe; do
    if [ ! -f "$HERE/$exe" ]; then
        echo "missing $exe" >&2
        rc=1
        continue
    fi
    echo "== $exe" >&2
    tmp="$HERE/.$exe.out"
    st=0
    if run_bench "$HERE/$exe" > "$tmp"; then
        st=0
    else
        st=$?
    fi
    cat "$tmp" | tee -a "$RESULTS"
    rm -f "$tmp"
    if [ "$st" -ne 0 ]; then
        echo "$exe exited $st" >&2
        rc=1
    fi
done

echo "results: $RESULTS" >&2
exit "$rc"
