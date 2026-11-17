#!/usr/bin/env bash
# Build DXVK as ARM64EC with llvm-mingw. Upstream v3.1.1 has no ARM64EC
# machine file; the patch gates the x86 SIMD flags and marks ARM64EC as ARM64.
set -euo pipefail

SRC=${DXVK_SRC:?}
OUT=${DXVK_OUT:?}
MINGW=${LLVM_MINGW_ROOT:?}

BIN="$MINGW/bin"
BUILD="$OUT/build"
DIST="$OUT/dist"
STAGE="$OUT/stage"
TOOLS="$OUT/bin"

mkdir -p "$BUILD" "$DIST" "$TOOLS"
ln -sf "$BIN/arm64ec-w64-mingw32-windres" "$TOOLS/windres"
export PATH="$TOOLS:$BIN:$PATH"

cat > "$BUILD/cross.ini" <<EOF
[binaries]
c = '$BIN/arm64ec-w64-mingw32-clang'
cpp = '$BIN/arm64ec-w64-mingw32-clang++'
ar = '$BIN/arm64ec-w64-mingw32-ar'
strip = '$BIN/arm64ec-w64-mingw32-strip'
windres = '$BIN/arm64ec-w64-mingw32-windres'

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

meson setup "$BUILD" "$SRC" --cross-file "$BUILD/cross.ini" \
  --buildtype=release -Db_ndebug=true --strip -Dbuild_id=false

ninja -C "$BUILD"

rm -rf "$STAGE"
mkdir -p "$STAGE"
find "$BUILD" -type f -name '*.dll' -exec cp {} "$STAGE/" \;
cp "$SRC/LICENSE" "$STAGE/LICENSE"
test -f "$STAGE/d3d11.dll"
test -f "$STAGE/dxgi.dll"
test -f "$STAGE/d3d10core.dll"
test -f "$STAGE/d3d9.dll"

# meson strips only on install; these DLLs are copied from the build tree.
for dll in "$STAGE"/*.dll; do
  "$BIN/arm64ec-w64-mingw32-strip" --strip-all "$dll"
done

for dll in "$STAGE"/*.dll; do
  "$BIN/llvm-readobj" --file-headers "$dll" |
    grep -Eq 'Machine: (IMAGE_FILE_MACHINE_ARM64EC|IMAGE_FILE_MACHINE_ARM64X)' || {
      echo "$dll is not ARM64EC/ARM64X"; exit 1;
    }
  if "$BIN/llvm-readobj" --sections "$dll" | grep -q 'Name: \.debug_'; then
    echo "$dll still has debug sections"; exit 1
  fi
done

tar -C "$STAGE" -I 'zstd -T0 -6' -cf "$DIST/dxvk-arm64ec.tar.zst" .

echo "== machine =="
for dll in "$STAGE"/*.dll; do
  printf '%s ' "$(basename "$dll")"
  "$BIN/llvm-readobj" --file-headers "$dll" |
    sed -n 's/.*Machine: \(.*\)/\1/p'
done
echo "== sections =="
for dll in "$STAGE/d3d11.dll" "$STAGE/dxgi.dll" "$STAGE/d3d10core.dll" "$STAGE/d3d9.dll"; do
  echo "$(basename "$dll"):"
  "$BIN/llvm-readobj" --sections "$dll" |
    sed -n 's/.*Name: \(.*\)/\1/p' | tr '\n' ' '
  echo
done
echo "== size =="
stat -c '%n %s bytes' "$STAGE/d3d11.dll" "$STAGE/dxgi.dll" \
  "$STAGE/d3d10core.dll" "$STAGE/d3d9.dll" "$DIST/dxvk-arm64ec.tar.zst"
