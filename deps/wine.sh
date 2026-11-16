#!/usr/bin/env bash
# Cross build of Wine for bionic: unix side aarch64-linux-android, PE side
# aarch64+arm64ec (ARM64X) and i386. Wine is configured twice, natively for
# its build tools and cross for the target; the tools tree is what
# --with-wine-tools points at.
set -euo pipefail

: "${ANDROID_NDK_HOME:?}" "${LLVM_MINGW_ROOT:?}" "${WINE_SRC:?}" "${WINE_OUT:?}"
TARGET_TRIPLE=${TARGET_TRIPLE:-aarch64-linux-android30}
JOBS=${JOBS:-$(nproc)}
NDK_BIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
HOST_PATH=/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
TOOLS=$WINE_OUT/tools
BUILD=$WINE_OUT/wine
PREFIX=$WINE_OUT/install

configure_flags=(
  --host="$TARGET_TRIPLE"
  --prefix="$PREFIX"
  # Android's configure default installs under $prefix/arm64-v8a; ntdll finds
  # its libraries relative to the loader, so keep bin/ lib/ share/.
  --exec-prefix="$PREFIX"
  --enable-archs=arm64ec,aarch64,i386
  --with-mingw=clang
  --with-wine-tools=../tools
  --disable-tests
  --disable-win16
  # amd_ags_x64 needs libdrm, which is not in the NDK sysroot.
  --disable-amd_ags_x64
  --without-alsa --without-capi --without-coreaudio --without-cups --without-dbus
  --without-ffmpeg --without-fontconfig --without-freetype
  --without-gettext --with-gettextpo=no
  --without-gnutls --without-gphoto --without-gssapi --without-gstreamer
  --without-inotify --without-krb5 --without-netapi --without-opencl --without-oss
  --without-pcap --without-pcsclite --without-piper --without-pulse --without-sane
  --without-sdl --without-udev --without-unwind --without-usb --without-v4l2
  --without-vosk --without-vulkan --without-wayland --without-x
)

target_env() {
  export PATH="$LLVM_MINGW_ROOT/bin:$NDK_BIN:$HOST_PATH"
  # --target belongs in CC itself: Wine links with $(CC) $(LDFLAGS) and does
  # not pass $(CFLAGS), so a target flag in CFLAGS alone loses the target.
  export CC="$NDK_BIN/clang --target=$TARGET_TRIPLE"
  export CXX="$NDK_BIN/clang++ --target=$TARGET_TRIPLE"
  export AR="$NDK_BIN/llvm-ar" RANLIB="$NDK_BIN/llvm-ranlib" STRIP="$NDK_BIN/llvm-strip"
  export DLLTOOL="$LLVM_MINGW_ROOT/bin/llvm-dlltool"
  export CFLAGS="-O2 -g0 -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES"
  export CROSSCFLAGS="-O2 -g0"
  export LDFLAGS="-Wl,-z,max-page-size=16384"
}

host_env() {
  export PATH="$HOST_PATH"
  export CC=/usr/bin/gcc CXX=/usr/bin/g++
  unset AR RANLIB STRIP DLLTOOL CFLAGS CROSSCFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_PATH
}

cd "$WINE_SRC"
autoreconf -fi
rm -rf autom4te.cache
# Proton's tree ships stale request headers and no syscall tables; both are
# generated from the sources and must not be trusted from the tarball.
tools/make_requests
tools/make_specfiles

rm -rf "$TOOLS" "$BUILD"
mkdir -p "$TOOLS" "$BUILD"

(
  host_env
  cd "$TOOLS"
  "$(realpath --relative-to="$TOOLS" "$WINE_SRC")/configure" --enable-win64 \
    --without-x --without-gstreamer --without-vulkan --without-wayland \
    --without-pulse --without-alsa --without-sdl --disable-tests
  make -j"$JOBS" __tooldeps__ nls/all
)

(
  target_env
  cd "$BUILD"
  "$(realpath --relative-to="$BUILD" "$WINE_SRC")/configure" "${configure_flags[@]}"
  make -j"$JOBS"
  make -j"$JOBS" install
)

for f in bin/wineserver \
         lib/wine/aarch64-unix/wine \
         lib/wine/aarch64-unix/ntdll.so \
         lib/wine/aarch64-unix/win32u.so \
         lib/wine/aarch64-windows/ntdll.dll \
         lib/wine/aarch64-windows/kernelbase.dll \
         lib/wine/i386-windows/ntdll.dll \
         lib/wine/i386-windows/kernelbase.dll \
         share/wine/wine.inf \
         share/wine/nls/l_intl.nls; do
  test -f "$PREFIX/$f" || { echo "missing: $f" >&2; exit 1; }
done

# The preloader is a static ET_EXEC; Android only starts app-private binaries
# through /system/bin/linker64, so its presence means patch 0001 was lost.
test ! -e "$PREFIX/lib/wine/aarch64-unix/wine-preloader" ||
  { echo "wine-preloader was built" >&2; exit 1; }

"$LLVM_MINGW_ROOT/bin/llvm-objdump" -h "$PREFIX/lib/wine/aarch64-windows/kernelbase.dll" |
  grep -q 'file format coff-arm64x' ||
  { echo "kernelbase.dll is not ARM64X" >&2; exit 1; }

find "$PREFIX/lib/wine/aarch64-unix" "$PREFIX/bin" -type f -print0 |
  while IFS= read -r -d '' f; do
    "$NDK_BIN/llvm-readelf" -h "$f" >/dev/null 2>&1 || continue
    if "$NDK_BIN/llvm-readelf" -lW "$f" | awk '$1 == "LOAD" { print $NF }' | grep -qv '^0x4000$'; then
      echo "$f: LOAD segments are not 16 KB aligned" >&2
      exit 1
    fi
  done
