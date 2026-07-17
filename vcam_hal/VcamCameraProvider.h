/**
 * VcamCameraProvider - 虚拟摄像头 Provider (HIDL ICameraProvider)
 *
 * 向 cameraserver / CameraService 注册虚拟摄像头，
 * 替代原厂 Camera HAL，所有 App 通过标准 Camera2/Camera1 API 获取虚拟画面。
 *
 * 实现 ICameraProvider 接口：
 *   - getCameraIdList: 返回虚拟摄像头列表 (vcam0, vcam1)
 *   - getCameraCharacteristics: 返回摄像头属性
 *   - getCameraDeviceInterface_V3_x: 返回 Camera2 设备
 *   - getCameraDeviceInterface_V1_x: 返回 Camera1 设备（兼容旧 App）
 */

#ifndef VCAM_CAMERA_PROVIDER_H
#define VCAM_CAMERA_PROVIDER_H

#include <string>
#include <vector>
#include <mutex>

namespace vcam {

// 虚拟摄像头 ID
constexpr const char* VCAM_BACK_ID  = "vcam0";
constexpr const char* VCAM_FRONT_ID = "vcam1";

class VcamCameraProvider {
public:
    static VcamCameraProvider& instance();

    // 生命周期
    bool initialize();
    void shutdown();

    // 获取摄像头列表
    std::vector<std::string> getCameraIdList() const;

    // 获取摄像头特性 (模拟 1920x1080, 30fps)
    const uint8_t* getCameraCharacteristics(const std::string& cameraId, size_t* outSize) const;

    // 是否已启用
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

    // 打开/关闭摄像头设备
    void* openDevice(const std::string& cameraId);  // 返回 VcamCameraDevice*
    void  closeDevice(void* device);

    // 获取实际注册状态
    bool isRegistered() const { return m_registered; }
    void setRegistered(bool reg) { m_registered = reg; }

private:
    VcamCameraProvider();

    // 为每个摄像头生成 camera_characteristics metadata
    uint8_t* generateCharacteristics(const std::string& cameraId, int facing,
                                       int width, int height, size_t* outSize) const;

    mutable std::mutex m_mutex;
    bool m_enabled   = true;
    bool m_registered = false;
};

} // namespace vcam

#endif // VCAM_CAMERA_PROVIDER_H
