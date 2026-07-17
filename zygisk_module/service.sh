#!/system/bin/sh
# VCAM Zygisk Module - service.sh
# 在系统启动完成后运行，确保文件权限和配置

MODDIR=${0%/*}
VIDEO_DIR="/data/local/tmp/vcam"
CONFIG_FILE="/data/local/tmp/vcam_config.json"

# 等待系统启动完成
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 2
done

sleep 5  # 额外等待确保所有服务就绪

# 确保目录和文件权限正确
mkdir -p "$VIDEO_DIR" 2>/dev/null
chmod 755 "$VIDEO_DIR" 2>/dev/null
chmod 755 /data/local/tmp 2>/dev/null

if [ -f "$CONFIG_FILE" ]; then
    chmod 644 "$CONFIG_FILE" 2>/dev/null
fi

if [ -f "$VIDEO_DIR/virtual.mp4" ]; then
    chmod 644 "$VIDEO_DIR/virtual.mp4" 2>/dev/null
fi

# 验证 Zygisk 模块是否加载
if [ -d "$MODDIR/zygisk" ]; then
    SO_COUNT=$(ls "$MODDIR/zygisk/"*.so 2>/dev/null | wc -l)
    echo "VCAM: Zygisk module loaded, $SO_COUNT native libs found" >> /dev/kmsg 2>/dev/null
else
    echo "VCAM: WARNING - zygisk directory not found" >> /dev/kmsg 2>/dev/null
fi

echo "VCAM: service.sh complete, boot finished" >> /dev/kmsg 2>/dev/null
