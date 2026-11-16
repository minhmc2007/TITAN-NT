#!/usr/bin/env bash
# llvm-mingw is not on the runner image and is ~400 MB unpacked, so it lives
# in a cached directory. The NDK is preinstalled; ANDROID_NDK_HOME must point
# at it.
set -euo pipefail

DEST=${TOOLCHAIN_DIR:?}
LLVM_MINGW=${LLVM_MINGW:?}
MINGW_ASSET="llvm-mingw-$LLVM_MINGW-ucrt-ubuntu-22.04-x86_64"

mkdir -p "$DEST"

if [ ! -d "$DEST/llvm-mingw" ]; then
  curl -sSL -o "$DEST/mingw.tar.xz" \
    "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/$MINGW_ASSET.tar.xz"
  tar -C "$DEST" -xf "$DEST/mingw.tar.xz"
  mv "$DEST/$MINGW_ASSET" "$DEST/llvm-mingw"
  rm -f "$DEST/mingw.tar.xz"
fi

: "${ANDROID_NDK_HOME:?}"
test -x "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
test -x "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
"$DEST/llvm-mingw/bin/clang" --version | head -1
"$DEST/llvm-mingw/bin/aarch64-w64-mingw32-clang" --version | head -1
"$DEST/llvm-mingw/bin/arm64ec-w64-mingw32-clang" --version | head -1
