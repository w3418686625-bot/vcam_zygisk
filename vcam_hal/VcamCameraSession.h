/**
 * VcamCameraSession - 虚拟摄像头会话
 *
 * 当 App 的 CameraCaptureSession 配置完成并开始请求时，
 * 此会话负责将解码后的视频帧填充到输出 buffer 中。
 *
 * 支持的输出格式: YUV_420_888 (NV21), IMPLEMENTATION_DEFINED, NV21
 *
 * 帧交付流程:
 *   1. App 发送 CaptureRequest
 *   2. Session 从 FrameProvider 获取最新解码帧
 *   3. 将帧数据复制到目标 buffer (GraphicBuffer)
 *   4. 通知 App 帧已就绪 (通过回调)
 */

#ifndef VCAM_CAMERA_SESSION_H
#define VCAM_CAMERA_SESSION_H

#include <cstdint>
#include <mutex>
#include <atomic>
#include <vector>
#include <condition_variable>
#include <thread>

namespace vcam {

struct OutputStream {
    int      id;
    int32_t  width;
    int32_t  height;
    int32_t  format;    // HAL pixel format
    int32_t  usage;
    int32_t  stride;
    void*    nativeWindow;   // ANativeWindow* 或 buffer queue
    bool     configured;
};

class VcamCameraDevice;

class VcamCameraSession {
public:
    VcamCameraSession(VcamCameraDevice* device);
    ~VcamCameraSession();

    // 流配置
    bool configureStreams(const std::vector<OutputStream>& streams);
    void close();

    // 处理捕获请求: 将视频帧复制到输出 buffer
    bool processCaptureRequest(int frameNumber,
                               const std::vector<int>& outputStreamIds);

    // 获取默认请求模板
    const uint8_t* getDefaultRequestSettings(int type, size_t* outSize);

    bool isClosed() const { return m_closed; }

private:
    void frameLoop();
    bool fillOutputBuffer(int streamId, const OutputStream& stream);

    VcamCameraDevice* m_device;
    std::vector<OutputStream> m_streams;
    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_streaming{false};

    std::thread m_frameThread;
    mutable std::mutex m_mutex;

    int m_frameNumber = 0;
};

} // namespace vcam

#endif // VCAM_CAMERA_SESSION_H
