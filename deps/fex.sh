#!/usr/bin/env bash
# Builds FEX's two Wine PE modules plus their bionic unix libraries. The PE
# side is a mingw cross build (aarch64 and arm64ec triples), not an Android
# target: Wine supplies the Windows APIs those modules call, and only
# Source/Windows/UnixLib links against bionic.
set -euo pipefail

: "${ANDROID_NDK_HOME:?}" "${LLVM_MINGW_ROOT:?}" "${FEX_SRC:?}" "${FEX_OUT:?}"
TARGET_TRIPLE=${TARGET_TRIPLE:-aarch64-linux-android30}
JOBS=${JOBS:-$(nproc)}
NDK_BIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
PREFIX=$FEX_OUT/install

export PATH="$LLVM_MINGW_ROOT/bin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
# FEX's mingw toolchain file picks its own compilers; inherited NDK settings
# make CMake prefer them and break the PE build.
unset CC CXX AR RANLIB NM STRIP

common=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DENABLE_FEXCORE_PROFILER=True -DENABLE_LTO=False
  -DBUILD_TESTING=False -DTUNE_CPU=none -DRANGES_NATIVE=OFF
  -DBUILD_STEAM_SUPPORT=True
  -DENABLE_CCACHE=False
  -DOVERRIDE_VERSION="$FEX_DESCRIBE"
  -DOVERRIDE_HASH="${FEX_COMMIT:0:7}"
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
)

for triple in aarch64-w64-mingw32 arm64ec-w64-mingw32; do
  cmake -S "$FEX_SRC" -B "$FEX_OUT/$triple" "${common[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
    -DMINGW_TRIPLE="$triple" \
    -DCMAKE_INSTALL_LIBDIR=lib/wine/aarch64-windows
  cmake --build "$FEX_OUT/$triple" -j"$JOBS"
done

cmake -S "$FEX_SRC/Source/Windows/UnixLib" -B "$FEX_OUT/unix" "${common[@]}" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM="android-${TARGET_TRIPLE##*android}" \
  -DANDROID_STL=c++_static \
  -DCMAKE_INSTALL_LIBDIR=lib/wine/aarch64-unix
cmake --build "$FEX_OUT/unix" -j"$JOBS"

for tree in aarch64-w64-mingw32 arm64ec-w64-mingw32 unix; do
  cmake --install "$FEX_OUT/$tree"
done

for f in lib/wine/aarch64-windows/libarm64ecfex.dll \
         lib/wine/aarch64-windows/libwow64fex.dll \
         lib/wine/aarch64-unix/libarm64ecfex.so \
         lib/wine/aarch64-unix/libwow64fex.so; do
  test -f "$PREFIX/$f" || { echo "missing: $f" >&2; exit 1; }
done

# These .so files are loaded into Wine's own process next to bionic libc;
# any other NEEDED entry is a C++ runtime clash waiting to happen.
for f in "$PREFIX"/lib/wine/aarch64-unix/*.so; do
  if "$NDK_BIN/llvm-readelf" -dW "$f" | grep NEEDED |
       grep -qv -e '\[libc\.so\]' -e '\[libdl\.so\]' -e '\[libm\.so\]'; then
    echo "${f#"$PREFIX"/}: unexpected NEEDED entries:" >&2
    "$NDK_BIN/llvm-readelf" -dW "$f" | grep NEEDED >&2
    exit 1
  fi
done
