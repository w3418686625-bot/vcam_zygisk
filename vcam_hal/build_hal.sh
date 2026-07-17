#!/bin/bash
# build_hal.sh - 交叉编译虚拟 Camera HAL
# 用法: bash build_hal.sh [NDK路径]
#
# 前置条件:
#   1. 安装 Android NDK r25+
#   2. 从 AOSP 源码提取 HIDL camera 库或使用 NDK camera2 头文件
#
# 产物:
#   vcam_hal/build/vcam_daemon       (daemon 可执行文件)
#   vcam_hal/build/libvcam_hal.so    (虚拟 HAL 实现库)
#
# 部署:
#   adb push build/vcam_daemon /data/local/tmp/vcam/
#   adb push build/libvcam_hal.so /data/local/tmp/vcam/

set -e

# ---- NDK 查找 ----
NDK="${1:-$ANDROID_NDK_HOME}"
if [ -z "$NDK" ]; then
    for p in \
        "$HOME/Android/Sdk/ndk/25.2.9519653" \
        "$HOME/Android/Sdk/ndk/26.1.10909125" \
        "$HOME/Android/Sdk/ndk/27.0.12077973"; do
        [ -d "$p" ] && NDK="$p" && break
    done
fi

if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "ERROR: NDK not found."
    echo "Please set ANDROID_NDK_HOME or pass NDK path as argument."
    echo "Usage: bash build_hal.sh /path/to/ndk"
    exit 1
fi

echo "NDK: $NDK"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

# 如果是在 macOS 上
if [ ! -d "$TOOLCHAIN" ]; then
    TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
fi
# Windows
if [ ! -d "$TOOLCHAIN" ]; then
    TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
fi

if [ ! -d "$TOOLCHAIN" ]; then
    echo "ERROR: Cannot find toolchain at $TOOLCHAIN"
    exit 1
fi

SYSROOT="$TOOLCHAIN/sysroot"

# ---- 编译器 ----
CC="$TOOLCHAIN/bin/aarch64-linux-android31-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android31-clang++"

echo "Toolchain: $TOOLCHAIN"
echo "CC: $CC"
echo "CXX: $CXX"

# ---- 构建目录 ----
mkdir -p "$BUILD_DIR"

# ---- 编译源文件 ----
echo ""
echo "=== Compiling VcamConfig ==="
$CXX -std=c++17 -O2 -fPIC -c \
    -I"$SYSROOT/usr/include" \
    -I"$SCRIPT_DIR" \
    -o "$BUILD_DIR/VcamConfig.o" \
    "$SCRIPT_DIR/VcamConfig.cpp"

echo "=== Compiling FrameProvider ==="
$CXX -std=c++17 -O2 -fPIC -c \
    -I"$SYSROOT/usr/include" \
    -I"$NDK/sources/android/native_app_glue" \
    -I"$SCRIPT_DIR" \
    -o "$BUILD_DIR/FrameProvider.o" \
    "$SCRIPT_DIR/FrameProvider.cpp"

echo "=== Compiling VcamCameraProvider ==="
$CXX -std=c++17 -O2 -fPIC -c \
    -I"$SYSROOT/usr/include" \
    -I"$SCRIPT_DIR" \
    -o "$BUILD_DIR/VcamCameraProvider.o" \
    "$SCRIPT_DIR/VcamCameraProvider.cpp"

echo "=== Compiling VcamCameraDevice ==="
$CXX -std=c++17 -O2 -fPIC -c \
    -I"$SYSROOT/usr/include" \
    -I"$SCRIPT_DIR" \
    -o "$BUILD_DIR/VcamCameraDevice.o" \
    "$SCRIPT_DIR/VcamCameraDevice.cpp"

echo "=== Compiling VcamCameraSession ==="
$CXX -std=c++17 -O2 -fPIC -c \
    -I"$SYSROOT/usr/include" \
    -I"$SCRIPT_DIR" \
    -o "$BUILD_DIR/VcamCameraSession.o" \
    "$SCRIPT_DIR/VcamCameraSession.cpp"

# ---- 链接为 .so ----
echo ""
echo "=== Linking libvcam_hal.so ==="
$CXX -std=c++17 -O2 -fPIC -shared \
    -Wl,-soname,libvcam_hal.so \
    -o "$BUILD_DIR/libvcam_hal.so" \
    "$BUILD_DIR/VcamConfig.o" \
    "$BUILD_DIR/FrameProvider.o" \
    "$BUILD_DIR/VcamCameraProvider.o" \
    "$BUILD_DIR/VcamCameraDevice.o" \
    "$BUILD_DIR/VcamCameraSession.o" \
    -L"$SYSROOT/usr/lib/aarch64-linux-android/31" \
    -landroid -llog -lmediandk -lcamera2ndk -lnativewindow \
    -lutils -lcutils -lbinder -lhidlbase -lhidltransport \
    -Wl,-rpath,/system/lib64

# ---- 编译 daemon 可执行文件 ----
echo ""
echo "=== Compiling vcam_daemon ==="
$CXX -std=c++17 -O2 -fPIE \
    -I"$SYSROOT/usr/include" \
    -I"$SCRIPT_DIR" \
    -o "$BUILD_DIR/vcam_daemon.o" \
    -c "$SCRIPT_DIR/vcam_daemon_main.cpp"

echo "=== Linking vcam_daemon ==="
$CXX -std=c++17 -O2 -fPIE -pie \
    -o "$BUILD_DIR/vcam_daemon" \
    "$BUILD_DIR/vcam_daemon.o" \
    -L"$BUILD_DIR" \
    -L"$SYSROOT/usr/lib/aarch64-linux-android/31" \
    -lvcam_hal \
    -landroid -llog -lmediandk -lcamera2ndk -lnativewindow \
    -lutils -lcutils -lbinder -lhidlbase -lhidltransport \
    -Wl,-rpath,/data/local/tmp/vcam

echo ""
echo "========================================"
echo "Build complete!"
ls -la "$BUILD_DIR/vcam_daemon" "$BUILD_DIR/libvcam_hal.so" 2>/dev/null
echo ""
echo "Deploy to device:"
echo "  adb push build/vcam_daemon /data/local/tmp/vcam/"
echo "  adb push build/libvcam_hal.so /data/local/tmp/vcam/"
echo "  adb shell chmod 755 /data/local/tmp/vcam/vcam_daemon"
echo ""
echo "Start daemon:"
echo "  adb shell su -c '/data/local/tmp/vcam/vcam_daemon &'"
echo ""
echo "Stop daemon:"
echo "  adb shell su -c 'kill \$(pidof vcam_daemon)'"
echo "========================================"
