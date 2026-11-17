#!/system/bin/sh
# App-private ELFs cannot be execve()ed on API 29+, so start through linker64.
set -eu

DIR=${1:-$(cd "$(dirname "$0")" && pwd)}
DIR=$(cd "$DIR" && pwd)
LOG="$DIR/vk-probe.log"

for f in vk_probe libvulkan_radeon.so; do
    [ -f "$DIR/$f" ] || { echo "missing $DIR/$f"; exit 1; }
done

cd "$DIR"
set +e
/system/bin/linker64 ./vk_probe ./libvulkan_radeon.so > "$LOG" 2>&1
rc=$?
set -e
cat "$LOG"

fail=0
[ "$rc" -eq 0 ] || fail=1
grep -q '^RESULT probe.exit=0$' "$LOG" || fail=1
grep -q '^RESULT probe.dlopen=ok$' "$LOG" || fail=1
grep -q '^RESULT probe.instance=ok$' "$LOG" || fail=1
grep -q '^RESULT x18_clobbers=0$' "$LOG" || fail=1
grep -q '^RESULT device.0.name=.' "$LOG" || fail=1
grep -q '^RESULT drm.readable=1$' "$LOG" || fail=1

if [ "$fail" -ne 0 ]; then
    echo "vk_probe: FAIL (exit $rc)"
    exit 1
fi
echo "vk_probe: PASS"
