#!/usr/bin/env bash
# Build vkd3d-proton as ARM64X with llvm-mingw. v3.1 has an ARM64X machine
# file, but it names gcc wrappers and passes -msse unconditionally.
set -euo pipefail

SRC=${VKD3D_SRC:?}
OUT=${VKD3D_OUT:?}
MINGW=${LLVM_MINGW_ROOT:?}

BIN="$MINGW/bin"
BUILD="$OUT/build"
DIST="$OUT/dist"
STAGE="$OUT/stage"
TOOLS="$OUT/bin"

mkdir -p "$BUILD" "$DIST" "$TOOLS"

# 20260908 ships hybrid ARM64X libraries under the aarch64 prefix only; shadow
# the arm64ec compiler names so the cross file keeps its upstream spelling.
for tool in clang clang++; do
  cat > "$TOOLS/arm64ec-w64-mingw32-$tool" <<EOF
#!/usr/bin/env bash
set -euo pipefail
real='$BIN/aarch64-w64-mingw32-$tool'
preprocess=0
for arg in "\$@"; do
  case "\$arg" in
    -E|-M|-MM|-S|-fsyntax-only) preprocess=1 ;;
  esac
done
args=()
for arg in "\$@"; do
  if [ "\$preprocess" = 1 ] && [ "\$arg" = -marm64x ]; then
    continue
  fi
  args+=("\$arg")
done
exec "\$real" "\${args[@]}"
EOF
  chmod +x "$TOOLS/arm64ec-w64-mingw32-$tool"
done
export PATH="$TOOLS:$BIN:$PATH"

meson setup "$BUILD" "$SRC" --cross-file "$SRC/build-arm64x.txt" \
  --buildtype=release --strip

ninja -C "$BUILD"

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BUILD/libs/d3d12/d3d12.dll" "$STAGE/d3d12.dll"
cp "$BUILD/libs/d3d12core/d3d12core.dll" "$STAGE/d3d12core.dll"
cp "$SRC/COPYING" "$STAGE/COPYING"

# meson strips only on install; these DLLs are copied from the build tree.
for dll in "$STAGE/d3d12.dll" "$STAGE/d3d12core.dll"; do
  "$BIN/arm64ec-w64-mingw32-strip" --strip-all "$dll"
done

for dll in "$STAGE/d3d12.dll" "$STAGE/d3d12core.dll"; do
  "$BIN/llvm-readobj" --file-headers "$dll" |
    grep -Eq 'Machine: (IMAGE_FILE_MACHINE_ARM64EC|IMAGE_FILE_MACHINE_ARM64X)' || {
      echo "$dll is not ARM64EC/ARM64X"; exit 1;
    }
  if "$BIN/llvm-readobj" --sections "$dll" | grep -q 'Name: \.debug_'; then
    echo "$dll still has debug sections"; exit 1
  fi
done

tar -C "$STAGE" -I 'zstd -T0 -6' -cf "$DIST/vkd3d-arm64x.tar.zst" .

echo "== machine =="
for dll in "$STAGE/d3d12.dll" "$STAGE/d3d12core.dll"; do
  printf '%s ' "$(basename "$dll")"
  "$BIN/llvm-readobj" --file-headers "$dll" |
    sed -n 's/.*Machine: \(.*\)/\1/p'
done
echo "== sections =="
for dll in "$STAGE/d3d12.dll" "$STAGE/d3d12core.dll"; do
  echo "$(basename "$dll"):"
  "$BIN/llvm-readobj" --sections "$dll" |
    sed -n 's/.*Name: \(.*\)/\1/p' | tr '\n' ' '
  echo
done
echo "== size =="
stat -c '%n %s bytes' "$STAGE/d3d12.dll" "$STAGE/d3d12core.dll" \
  "$DIST/vkd3d-arm64x.tar.zst"
