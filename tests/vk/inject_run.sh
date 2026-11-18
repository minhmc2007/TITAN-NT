#!/system/bin/sh
# App-private ELFs cannot be execve()ed on API 29+, so start through linker64.
set -eu

DIR=${1:-$(cd "$(dirname "$0")" && pwd)}
DIR=$(cd "$DIR" && pwd)
LOG="$DIR/vk-inject.log"

for f in libtitan_vk.so vk_inject_test vulkan.radeon.so; do
    [ -f "$DIR/$f" ] || { echo "missing $DIR/$f"; exit 1; }
done

# The shim reads TITAN_ICD at the loader's first ICD open. The platform loader
# ignores the Khronos variables, so keep them out of the run.
export TITAN_ICD="$DIR/vulkan.radeon.so"
unset VK_ICD_FILENAMES VK_DRIVER_FILES

cd "$DIR"
# linker64 rejects relative program paths, and dlopen does not search the cwd.
set +e
{ /system/bin/linker64 "$DIR/vk_inject_test" "$DIR/libtitan_vk.so" 2>&1; \
  echo "RESULT runner.exit=$?"; } | tee "$LOG"
set -e

fail=0
grep -q '^RESULT runner.exit=0$' "$LOG" || fail=1
grep -q '^RESULT inject.dlopen=ok$' "$LOG" || fail=1
grep -q '^RESULT inject.instance=ok$' "$LOG" || fail=1
grep -q '^RESULT inject.error=none$' "$LOG" || fail=1
grep -q '^RESULT inject.substituted=1$' "$LOG" || fail=1
grep -qE '^RESULT inject.hook_symbol=(android_load_sphal_library|android_dlopen_ext|dlopen)$' \
    "$LOG" || fail=1
hook_symbols='android_load_sphal_library|android_unload_sphal_library|android_dlopen_ext|dlopen'
grep -qE "^RESULT inject.hook_symbols=($hook_symbols)(,($hook_symbols))*$" \
    "$LOG" || fail=1
grep -qE '^RESULT inject.matched_path=((/vendor/lib64/hw/|/vendor/lib/hw/)?vulkan\..+\.so)$' \
    "$LOG" || fail=1
grep -qE '^RESULT inject.physical_devices=[1-9][0-9]*$' "$LOG" || fail=1
grep -q '^RESULT device.0.name=.*RADV' "$LOG" || fail=1
grep -q '^RESULT inject.ext.VK_KHR_swapchain=1$' "$LOG" || fail=1
for ext in VK_KHR_surface VK_KHR_android_surface; do
    if ! grep -q "^RESULT inject.ext.$ext=1$" "$LOG"; then
        echo "vk_inject_test: $ext missing"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "vk_inject_test: FAIL"
    exit 1
fi
echo "vk_inject_test: PASS"
