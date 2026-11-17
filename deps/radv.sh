#!/usr/bin/env bash
# Build the Xclipse RADV driver for Android. The driver's build.sh targets a
# plain NDK build; TITAN reserves x18 because Wine's ARM64EC TEB lives there.
set -euo pipefail

SRC=${RADV_SRC:?}
OUT=${RADV_OUT:?}
NDK=${ANDROID_NDK_HOME:?}
API=34

BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
BUILD="$OUT/build"
DIST="$OUT/dist"
STAGE="$OUT/stage"

mkdir -p "$BUILD" "$DIST"

# libunwind saves and restores x18; ARM64EC keeps the TEB there, so zero the
# saved slot and skip the restore.
UNWIND=$("$BIN/aarch64-linux-android$API-clang++" -print-file-name=libunwind.a 2>/dev/null || true)
[ -f "$UNWIND" ] || UNWIND=
if [ -z "$UNWIND" ]; then
  for f in "$BIN/../sysroot/usr/lib/aarch64-linux-android/libunwind.a" \
           "$BIN/../sysroot/usr/lib/aarch64-linux-android/libc++_static.a" \
           "$BIN/../sysroot/usr/lib/aarch64-linux-android/libc++abi.a" \
           "$BIN/../lib/clang"/*/lib/linux*/libunwind.a; do
    [ -f "$f" ] || continue
    if "$BIN/llvm-nm" "$f" 2>/dev/null | grep -q 'unw_getcontext'; then
      UNWIND=$f
      break
    fi
  done
fi
if [ -z "$UNWIND" ]; then
  while IFS= read -r f; do
    if "$BIN/llvm-nm" "$f" 2>/dev/null | grep -q 'unw_getcontext'; then
      UNWIND=$f
      break
    fi
  done < <(find "$NDK/toolchains/llvm/prebuilt/linux-x86_64" -name '*.a' -type f -path '*aarch64*')
fi
if [ -z "$UNWIND" ]; then
  echo "no aarch64 unwinder archive with unw_getcontext under $NDK" >&2
  find "$NDK/toolchains/llvm/prebuilt/linux-x86_64" \
    \( -name 'libunwind*.a' -o -name 'libc++_static.a' \) -print >&2
  exit 1
fi
echo "unwinder: $UNWIND"
PATCHED="$BUILD/libunwind-x18.a"
if [ ! -f "$PATCHED" ]; then
  rm -rf "$BUILD/unwind"
  mkdir -p "$BUILD/unwind"
  (cd "$BUILD/unwind" && "$BIN/llvm-ar" x "$UNWIND")
  python3 - "$BUILD/unwind" <<'PY'
import glob
import os
import sys

root = sys.argv[1]
pairs = [
    (bytes.fromhex('124c09a9'), bytes.fromhex('1f4c09a9')),
    (bytes.fromhex('124c49a9'), bytes.fromhex('1f4c49a9')),
]
hits = 0
for path in glob.glob(os.path.join(root, '*.o')):
    data = bytearray(open(path, 'rb').read())
    for old, new in pairs:
        hits += data.count(old)
        data = data.replace(old, new)
    open(path, 'wb').write(data)
if hits < 2:
    sys.exit('expected at least 2 x18 context instructions, patched %d' % hits)
print('patched %d x18 instructions' % hits)
PY
  "$BIN/llvm-ar" rcs "$PATCHED" "$BUILD"/unwind/*.o
fi
cp "$PATCHED" "$UNWIND" 2>/dev/null || sudo cp "$PATCHED" "$UNWIND"

cat > "$BUILD/cross.ini" <<EOF
[binaries]
c = '$BIN/aarch64-linux-android$API-clang'
cpp = '$BIN/aarch64-linux-android$API-clang++'
ar = '$BIN/llvm-ar'
strip = '$BIN/llvm-strip'

[properties]
cpp_link_args = ['-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

if [ ! -f "$BUILD/build.ninja" ]; then
  meson setup "$BUILD" "$SRC" --cross-file "$BUILD/cross.ini" \
    -Dbuildtype=release -Db_ndebug=true \
    -Dplatforms=android -Dplatform-sdk-version="$API" \
    -Dandroid-stub=true -Dandroid-strict=false \
    -Dandroid-libbacktrace=disabled \
    -Dvulkan-drivers=amd -Dgallium-drivers= -Dvulkan-layers= -Dtools= \
    -Dllvm=disabled -Dzstd=disabled -Dlmsensors=disabled -Dperfetto=false \
    -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dallow-fallback-for=libdrm,perfetto --force-fallback-for=expat,libdrm,zlib \
    -Dlibdrm:default_library=static -Dexpat:default_library=static -Dzlib:default_library=static \
    -Dc_args='-march=armv8.2-a -ffixed-x18 -fno-sanitize=shadow-call-stack' \
    -Dcpp_args='-march=armv8.2-a -ffixed-x18 -fno-sanitize=shadow-call-stack'
fi

ninja -C "$BUILD" src/amd/vulkan/libvulkan_radeon.so

# Inline asm and assembly bypass -ffixed-x18, so check the machine code before
# and after stripping. The unstripped copy keeps the symbol names.
scan_x18() {
  local lib=$1 report=$2
  "$BIN/llvm-objdump" -d --no-show-raw-insn "$lib" > "$OUT/disasm.txt"
  awk '
    /^[0-9a-f]+ <.*>:$/ { sym=$0 }
    /^[[:space:]]*[0-9a-f]+:/ {
      if ($0 ~ /(^|[^[:alnum:]_])[xw]18([^[:alnum:]_]|$)/) {
        print sym
        print $0
      }
    }
  ' "$OUT/disasm.txt" > "$report"
  if [ -s "$report" ]; then
    echo "x18 used in $lib:"
    cat "$report"
    return 1
  fi
}

scan_x18 "$BUILD/src/amd/vulkan/libvulkan_radeon.so" "$DIST/x18-unstripped.txt"
"$BIN/llvm-strip" --strip-unneeded \
  -o "$BUILD/vulkan.radeon.so" "$BUILD/src/amd/vulkan/libvulkan_radeon.so"
scan_x18 "$BUILD/vulkan.radeon.so" "$DIST/x18-stripped.txt"

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BUILD/vulkan.radeon.so" "$STAGE/vulkan.radeon.so"

python3 - "$SRC" "$STAGE/meta.json" <<'PY'
import json
import os
import re
import subprocess
import sys

src, out = sys.argv[1], sys.argv[2]
version = open(os.path.join(src, 'VERSION')).read().strip()
header = open(os.path.join(src, 'include', 'vulkan', 'vulkan_core.h')).read()
api = re.search(r'VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION\(0, (\d+), (\d+), VK_HEADER_VERSION\)', header)
patch = re.search(r'#define VK_HEADER_VERSION (\d+)', header)
vulkan = 'Vulkan %s.%s.%s' % (api.group(1), api.group(2), patch.group(1))
count = subprocess.check_output(['git', '-C', src, 'rev-list', '--count', 'HEAD'], text=True).strip()
meta = {
    'schemaVersion': 1,
    'name': 'RADV Xclipse (Mesa %s)' % version,
    'description': 'RADV for Samsung Xclipse GPUs, based on Mesa %s.' % version,
    'author': 'TITAN',
    'packageVersion': int(count),
    'vendor': 'Mesa',
    'driverVersion': vulkan,
    'minApi': 34,
    'libraryName': 'vulkan.radeon.so',
}
with open(out, 'w') as f:
    json.dump(meta, f, indent=2)
    f.write('\n')
PY

tar -C "$STAGE" -I 'zstd -T0 -6' -cf "$DIST/radv-xclipse.tar.zst" .

LIB="$STAGE/vulkan.radeon.so"
"$BIN/llvm-readelf" -h "$LIB" > "$DIST/elf.txt" 2>&1
grep -q 'AArch64' "$DIST/elf.txt" || {
  cat "$DIST/elf.txt"; exit 1;
}
if "$BIN/llvm-readelf" -S -W "$LIB" | grep -Eq '\.symtab|\.debug_'; then
  echo "unstripped sections remain"; exit 1
fi

allowed="libhardware.so liblog.so libnativewindow.so libsync.so libm.so libdl.so libc.so"
needed=$("$BIN/llvm-readelf" -dW "$LIB" |
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
"$BIN/llvm-readelf" -dW "$LIB" | grep NEEDED
echo "== size =="
stat -c '%n %s bytes' "$LIB" "$DIST/radv-xclipse.tar.zst"
