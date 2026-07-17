#!/system/bin/sh
# VCAM Zygisk Module - customize.sh
# KernelSU/Magisk 安装时执行

SKIPUNZIP=0

ui_print "==================================="
ui_print " VCAM Virtual Camera (Zygisk)"
ui_print " System-level camera replacement"
ui_print "==================================="
ui_print ""

# 检查 Zygisk 支持
if [ -z "$ZYGISKSRC" ] && [ -z "$MAGISKTMP" ]; then
    ui_print "! Warning: Zygisk support not detected"
    ui_print "! This module requires Magisk Zygisk or KernelSU Zygisk Next"
    ui_print ""
fi

# 检查架构
ARCH=$(getprop ro.product.cpu.abi)
ui_print "- Device architecture: $ARCH"

case "$ARCH" in
    arm64-v8a)
        SO_FILE="arm64-v8a.so"
        ;;
    armeabi-v7a)
        SO_FILE="armeabi-v7a.so"
        ;;
    *)
        ui_print "! Unsupported architecture: $ARCH"
        ui_print "! Only arm64-v8a and armeabi-v7a are supported"
        abort "Aborting..."
        ;;
esac

# 设置权限
ui_print "- Setting permissions..."
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/zygisk" 0 0 0755 0644

# 确保配置目录存在
ui_print "- Creating config directories..."
mkdir -p /data/local/tmp/vcam
chmod 755 /data/local/tmp/vcam
chmod 755 /data/local/tmp

ui_print ""
ui_print "==================================="
ui_print " Installation Complete!"
ui_print "==================================="
ui_print ""
ui_print " Next steps:"
ui_print " 1. Reboot your device"
ui_print " 2. Open VCAM app"
ui_print " 3. Select a video file"
ui_print " 4. Tap 'Activate' then 'Replace'"
ui_print " 5. Open any camera app to verify"
ui_print ""
ui_print " Note: No LSPosed required!"
ui_print " Zygisk handles injection automatically."
ui_print ""
