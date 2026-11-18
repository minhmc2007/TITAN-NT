#!/usr/bin/env bash
# The AOSP loader cannot see an app-private ICD: it only enumerates
# /vendor/lib64/hw from ro.hardware.vulkan. Build the Khronos loader with its
# Android WSI path so VK_ICD_FILENAMES/VK_DRIVER_FILES work unrooted.
set -euo pipefail

SRC=${VULKAN_LOADER_SRC:?}
HEADERS=${VULKAN_HEADERS_PREFIX:?}
OUT=${VULKAN_LOADER_OUT:?}
NDK=${ANDROID_NDK_HOME:?}
API=34

BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
BUILD="$OUT/build"
DIST="$OUT/dist"
STAGE="$OUT/stage"

mkdir -p "$BUILD" "$DIST"

if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=none \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$HEADERS" \
    -DBUILD_TESTS=OFF -DBUILD_WSI_ANDROID_SUPPORT=ON \
    -DCMAKE_C_FLAGS="-O2 -ffixed-x18 -fno-sanitize=shadow-call-stack -march=armv8.2-a+lse" \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384"
fi

cmake --build "$BUILD" --target vulkan -j"$(nproc)"

LIB=$(find "$BUILD" -maxdepth 3 -name 'libvulkan.so' -print -quit)
[ -n "$LIB" ] || { echo "libvulkan.so not found under $BUILD"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -L "$LIB" "$STAGE/libvulkan.so"
"$BIN/llvm-strip" --strip-unneeded -o "$DIST/libvulkan.so" "$STAGE/libvulkan.so"

# Inline asm and assembly bypass -ffixed-x18; x18 carries the Windows TEB
# when the loader is dlopened from Wine's ARM64EC unix side.
"$BIN/llvm-objdump" -d --no-show-raw-insn "$DIST/libvulkan.so" > "$DIST/disasm.txt"
awk '
  /^[0-9a-f]+ <.*>:$/ { sym=$0 }
  /^[[:space:]]*[0-9a-f]+:/ {
    if ($0 ~ /(^|[^[:alnum:]_])[xw]18([^[:alnum:]_]|$)/) {
      print sym
      print $0
    }
  }
' "$DIST/disasm.txt" > "$DIST/x18.txt"
if [ -s "$DIST/x18.txt" ]; then
  echo "x18 used in libvulkan.so:"
  head -n 40 "$DIST/x18.txt"
  exit 1
fi

"$BIN/llvm-readelf" -h "$DIST/libvulkan.so" > "$DIST/elf.txt"
grep -q 'AArch64' "$DIST/elf.txt" || { cat "$DIST/elf.txt"; exit 1; }
grep -q 'Type:.*DYN' "$DIST/elf.txt" || { cat "$DIST/elf.txt"; exit 1; }
if "$BIN/llvm-readelf" -S -W "$DIST/libvulkan.so" | grep -E '\.symtab|\.debug_' > /dev/null; then
  echo "unstripped sections remain"
  exit 1
fi

# The AOSP loader has no env override; prove the strings made it into the
# build and that the Android surface entry point is exported.
for s in VK_ICD_FILENAMES VK_DRIVER_FILES VK_KHR_android_surface VK_KHR_surface; do
  grep -aqF "$s" "$DIST/libvulkan.so" || { echo "missing string $s"; exit 1; }
done
"$BIN/llvm-readelf" --dyn-syms -W "$DIST/libvulkan.so" > "$DIST/syms.txt"
for s in vkCreateInstance vkGetInstanceProcAddr vkEnumerateInstanceExtensionProperties \
         vkCreateAndroidSurfaceKHR; do
  grep -q " $s\$" "$DIST/syms.txt" || { echo "missing export $s"; exit 1; }
done

allowed="libdl.so libm.so libc.so"
needed=$("$BIN/llvm-readelf" -dW "$DIST/libvulkan.so" |
  sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
extra=""
for n in $needed; do
  case " $allowed " in
    *" $n "*) ;;
    *) extra="$extra $n" ;;
  esac
done
if [ -n "$extra" ]; then
  echo "unexpected NEEDED:$extra"
  exit 1
fi

echo "== NEEDED =="
"$BIN/llvm-readelf" -dW "$DIST/libvulkan.so" | grep NEEDED
echo "== size =="
stat -c '%n %s bytes' "$DIST/libvulkan.so"
