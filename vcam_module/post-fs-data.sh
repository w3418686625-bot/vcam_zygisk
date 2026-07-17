#!/system/bin/sh
# VCAM 模块 - post-fs-data.sh
# 开机早期执行，在大部分系统服务启动之前
# 负责创建目录、设置权限

MODDIR=${0%/*}

# 创建视频和配置存储目录
VIDEO_DIR="/data/local/tmp/vcam"
mkdir -p "$VIDEO_DIR"
chmod 755 "$VIDEO_DIR"

# 确保配置文件全局可读
CONFIG_FILE="/data/local/tmp/vcam_config.json"
if [ -f "$CONFIG_FILE" ]; then
    chmod 644 "$CONFIG_FILE"
fi

# 确保视频文件全局可读
VIDEO_FILE="$VIDEO_DIR/virtual.mp4"
if [ -f "$VIDEO_FILE" ]; then
    chmod 644 "$VIDEO_FILE"
fi

# 确保 /data/local/tmp 可访问
chmod 755 /data/local/tmp 2>/dev/null

echo "VCAM: post-fs-data done" > /dev/kmsg
