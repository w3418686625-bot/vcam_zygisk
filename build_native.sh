#!/system/bin/sh
# build_native.sh - 交叉编译 native hook 库
# 用法: bash build_native.sh /path/to/android-ndk
#
# 产物:
#   app/src/main/assets/native/libvcam_hook.so  (hook library)
#   app/src/main/assets/native/vcam_inject       (injector)

NDK="${1:-$ANDROID_NDK_HOME}"
if [ -z "$NDK" ]; then
    for p in "$HOME/Android/Sdk/ndk/25.2.9519653" "$HOME/Android/Sdk/ndk/26.1.10909125"; do
        [ -d "$p" ] && NDK="$p" && break
    done
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "ERROR: NDK not found. Usage: $0 /path/to/ndk"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASSETS_DIR="$SCRIPT_DIR/app/src/main/assets/native"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$TOOLCHAIN/sysroot"
CC="$TOOLCHAIN/bin/aarch64-linux-android31-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android31-clang++"

echo "NDK: $NDK"
echo "Assets: $ASSETS_DIR"

mkdir -p "$ASSETS_DIR"

echo "=== Building injector ==="
$CC -std=c11 -O2 -static -fPIE \
    -I"$SYSROOT/usr/include" \
    "$ASSETS_DIR/vcam_inject.c" \
    -o "$ASSETS_DIR/vcam_inject" \
    -ldl
echo "Injector: $?"

echo "=== Building hook library ==="
$CXX -std=c++17 -O2 -fPIC -shared \
    -I"$SYSROOT/usr/include" \
    -I"$NDK/sources/android/native_app_glue" \
    "$ASSETS_DIR/vcam_hook.cpp" \
    -o "$ASSETS_DIR/libvcam_hook.so" \
    -llog -ldl
echo "Hook lib: $?"

echo ""
ls -la "$ASSETS_DIR/vcam_inject" "$ASSETS_DIR/libvcam_hook.so" 2>/dev/null
echo "Done!"
