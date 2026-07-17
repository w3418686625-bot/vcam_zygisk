#!/system/bin/sh
# VCAM Zygisk Module - post-fs-data
# 在系统启动早期运行，创建必要的目录和权限

MODDIR=${0%/*}

# 创建视频存储目录
VIDEO_DIR="/data/local/tmp/vcam"
mkdir -p "$VIDEO_DIR" 2>/dev/null
chmod 755 "$VIDEO_DIR" 2>/dev/null

# 确保 /data/local/tmp 可被所有进程读取
chmod 755 /data/local/tmp 2>/dev/null

# 确保配置文件全局可读
CONFIG_FILE="/data/local/tmp/vcam_config.json"
if [ -f "$CONFIG_FILE" ]; then
    chmod 644 "$CONFIG_FILE" 2>/dev/null
fi

# 确保视频文件全局可读
if [ -f "$VIDEO_DIR/virtual.mp4" ]; then
    chmod 644 "$VIDEO_DIR/virtual.mp4" 2>/dev/null
fi

# 记录启动日志
echo "VCAM: post-fs-data complete, video_dir=$VIDEO_DIR" >> /dev/kmsg 2>/dev/null
