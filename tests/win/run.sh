#!/system/bin/sh
# Device entry point for the M2 Wine bundle. App-private executables cannot be
# execve()ed on API 29+, so every ELF here starts through linker64.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
export WINEPREFIX="${WINEPREFIX:-$HERE/prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINELOADER="$HERE/lib/wine/aarch64-unix/wine"
export PATH="$HERE/bin:$PATH"
if [ "${TITAN_SIGSYS:-0}" = 1 ]; then export LD_PRELOAD="$HERE/libsigprobe.so"; fi

wine() { /system/bin/linker64 "$WINELOADER" "$@"; }
wineserver() { /system/bin/linker64 "$HERE/bin/wineserver" "$@"; }

if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "creating prefix at $WINEPREFIX"
    wine wineboot -i
    wineserver -w || true
fi

rc=0
for exe in m2_win_test64 m2_win_test32; do
    echo "== $exe"
    wine "$HERE/tests/$exe.exe" || rc=1
done
exit $rc
