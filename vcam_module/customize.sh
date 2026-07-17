#!/system/bin/sh
# VCAM 模块 - customize.sh
# 在模块安装时执行（KernelSU/Magisk 的安装/升级界面）
# 负责初始目录和权限设置

# 创建数据目录
mkdir -p /data/local/tmp/vcam
chmod 755 /data/local/tmp/vcam

# 确保配置目录存在
chmod 755 /data/local/tmp 2>/dev/null

echo ""
echo "========================================"
echo "  VCAM 全局虚拟摄像头模块"
echo "========================================"
echo ""
echo "✓ 模块已安装"
echo ""
echo "使用方法："
echo "  1. 确保 LSPosed 已安装并激活 VCAM 模块"
echo "  2. 在 VCAM App 中选择视频并点击替换"
echo "  3. 重启手机使 LSPosed 作用域生效"
echo "  4. 所有 App 摄像头均被替换"
echo ""
echo "========================================"
