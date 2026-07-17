#!/system/bin/sh
# build.sh - 编译 Zygisk 模块原生库
# 用法: bash build.sh [ndk_path]
# 
# 默认 NDK 路径，修改为你的实际路径

NDK="${1:-$ANDROID_NDK_HOME}"
if [ -z "$NDK" ]; then
    # 尝试常见路径
    for p in \
        "$HOME/Android/Sdk/ndk/25.2.9519653" \
        "$HOME/Android/Sdk/ndk/26.1.10909125" \
        "/opt/android-ndk" \
        "$ANDROID_NDK"; do
        if [ -d "$p" ]; then
            NDK="$p"
            break
        fi
    done
fi

if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "ERROR: NDK not found. Set ANDROID_NDK_HOME or pass path as argument."
    echo "Usage: $0 /path/to/android-ndk"
    exit 1
fi

echo "Using NDK: $NDK"

MODULE_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$MODULE_DIR/../out"

mkdir -p "$OUT_DIR/arm64-v8a"
mkdir -p "$OUT_DIR/armeabi-v7a"

# Build for arm64-v8a (most modern phones)
echo "=== Building arm64-v8a ==="
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang++ \
    -std=c++17 -O2 -fPIC -shared \
    -I$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include \
    -I$MODULE_DIR \
    $MODULE_DIR/main.cpp \
    -o $OUT_DIR/arm64-v8a/libvcam_zygisk.so \
    -llog -Wl,-soname,libvcam_zygisk.so

if [ $? -eq 0 ]; then
    echo "arm64-v8a: OK"
else
    echo "arm64-v8a: FAILED"
fi

# Build for armeabi-v7a (older 32-bit phones)
echo "=== Building armeabi-v7a ==="
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi31-clang++ \
    -std=c++17 -O2 -fPIC -shared \
    -I$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include \
    -I$MODULE_DIR \
    $MODULE_DIR/main.cpp \
    -o $OUT_DIR/armeabi-v7a/libvcam_zygisk.so \
    -llog -Wl,-soname,libvcam_zygisk.so

if [ $? -eq 0 ]; then
    echo "armeabi-v7a: OK"
else
    echo "armeabi-v7a: FAILED"
fi

# Copy to zygisk module directory
mkdir -p "$MODULE_DIR/../zygisk"
cp "$OUT_DIR/arm64-v8a/libvcam_zygisk.so" "$MODULE_DIR/arm64-v8a.so" 2>/dev/null
cp "$OUT_DIR/armeabi-v7a/libvcam_zygisk.so" "$MODULE_DIR/armeabi-v7a.so" 2>/dev/null

echo ""
echo "=== Build Complete ==="
echo "Output: $OUT_DIR/"
ls -la "$OUT_DIR/arm64-v8a/" 2>/dev/null
ls -la "$OUT_DIR/armeabi-v7a/" 2>/dev/null
echo ""
echo "To create Magisk module ZIP:"
echo "  cd $MODULE_DIR/.. && zip -r vcam_zygisk.zip module.prop post-fs-data.sh service.sh zygisk/"
