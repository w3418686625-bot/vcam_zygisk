/**
 * VcamCameraDevice - 虚拟摄像头设备 (模拟 ICameraDevice)
 *
 * 当 App 通过 CameraManager.openCamera() 打开摄像头时，
 * CameraService 会调用此设备的 open() 方法创建 Session。
 * 管理到 VcamCameraSession 的映射。
 */

#ifndef VCAM_CAMERA_DEVICE_H
#define VCAM_CAMERA_DEVICE_H

#include <string>
#include <mutex>
#include <atomic>
#include <android/log.h>

#define LOG_TAG_DEV "VCAM-HAL-Device"
#define LOGI_DEV(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG_DEV, __VA_ARGS__)
#define LOGE_DEV(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG_DEV, __VA_ARGS__)

namespace vcam {

class VcamCameraDevice {
public:
    VcamCameraDevice(const std::string& cameraId, int facing, int width, int height);
    ~VcamCameraDevice();

    const std::string& getCameraId() const { return m_cameraId; }
    int getFacing() const { return m_facing; }
    int getDefaultWidth()  const { return m_defaultWidth; }
    int getDefaultHeight() const { return m_defaultHeight; }

    bool isOpen() const { return m_opened; }
    void setOpened(bool opened) { m_opened = opened; }

private:
    std::string m_cameraId;
    int m_facing;       // 0=front, 1=back
    int m_defaultWidth;
    int m_defaultHeight;
    std::atomic<bool> m_opened{false};
};

} // namespace vcam

#endif // VCAM_CAMERA_DEVICE_H
