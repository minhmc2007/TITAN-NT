#!/system/bin/sh
# App-private ELFs cannot be execve()ed on API 29+, so start through linker64.
set -eu

DIR=${1:-$(cd "$(dirname "$0")" && pwd)}
DIR=$(cd "$DIR" && pwd)
LOG="$DIR/vk-loader.log"
ICD="$DIR/vulkan.radeon.json"

for f in libvulkan.so vk_loader_test vulkan.radeon.so; do
    [ -f "$DIR/$f" ] || { echo "missing $DIR/$f"; exit 1; }
done

# The loader consumes an ICD manifest, not the custom-driver meta.json. The
# absolute library_path is what lets an unrooted app-private ICD load.
cat > "$ICD" <<EOF
{"file_format_version":"1.0.0","ICD":{"library_path":"$DIR/vulkan.radeon.so","api_version":"1.4.368"}}
EOF

export VK_ICD_FILENAMES="$ICD"
export LD_LIBRARY_PATH="$DIR"
export VK_LOADER_DEBUG=error,warn,driver

# linker64 rejects relative program paths, and the loader is dlopened by
# absolute path so the Android namespace never resolves the AOSP libvulkan.
set +e
/system/bin/linker64 "$DIR/vk_loader_test" "$DIR/libvulkan.so" > "$LOG" 2>&1
rc=$?
set -e
cat "$LOG"

fail=0
[ "$rc" -eq 0 ] || fail=1
grep -q '^RESULT loader.exit=0$' "$LOG" || fail=1
grep -q '^RESULT loader.dlopen=ok$' "$LOG" || fail=1
grep -q '^RESULT loader.instance=ok$' "$LOG" || fail=1
grep -q '^RESULT loader.physical_devices=[1-9][0-9]*$' "$LOG" || fail=1
grep -q '^RESULT device.0.name=.' "$LOG" || fail=1
for ext in VK_KHR_surface VK_KHR_android_surface; do
    if ! grep -q "^RESULT loader.ext.$ext=[01]$" "$LOG"; then
        echo "vk_loader_test: missing loader.ext.$ext fact"
        fail=1
    elif grep -q "^RESULT loader.ext.$ext=0$" "$LOG"; then
        echo "vk_loader_test: WARN loader.ext.$ext=0, the ICD did not advertise it"
    fi
done
if grep -q '^RESULT loader.error=' "$LOG"; then
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "vk_loader_test: FAIL (exit $rc)"
    exit 1
fi
echo "vk_loader_test: PASS"
