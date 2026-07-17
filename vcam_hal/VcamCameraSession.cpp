#include "VcamCameraSession.h"
#include "VcamCameraDevice.h"
#include "FrameProvider.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "VCAM-HAL-Session"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vcam {

VcamCameraSession::VcamCameraSession(VcamCameraDevice* device)
    : m_device(device) {
    LOGI("Session created for camera: %s", device->getCameraId().c_str());
}

VcamCameraSession::~VcamCameraSession() {
    close();
    LOGI("Session destroyed");
}

bool VcamCameraSession::configureStreams(const std::vector<OutputStream>& streams) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_streams = streams;
    LOGI("Streams configured: %zu streams", streams.size());
    for (auto& s : streams) {
        LOGI("  Stream: %dx%d, format=0x%x, usage=0x%x",
             s.width, s.height, s.format, s.usage);
    }
    return true;
}

void VcamCameraSession::close() {
    if (m_closed) return;
    m_closed = true;
    m_streaming = false;

    if (m_frameThread.joinable()) {
        m_frameThread.join();
    }

    LOGI("Session closed");
}

bool VcamCameraSession::processCaptureRequest(
    int frameNumber, const std::vector<int>& outputStreamIds) {

    if (m_closed) return false;

    m_frameNumber = frameNumber;

    // 对每个输出流填充帧数据
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int streamId : outputStreamIds) {
        for (auto& stream : m_streams) {
            if (stream.id == streamId) {
                fillOutputBuffer(streamId, stream);
            }
        }
    }

    return true;
}

bool VcamCameraSession::fillOutputBuffer(int streamId, const OutputStream& stream) {
    // 从 FrameProvider 获取最新解码帧
    VideoFrame vf;
    if (!FrameProvider::instance().getLatestFrame(vf)) {
        // 没有帧可用，填充灰色画面
        static uint8_t grayFrame[1920 * 1080 * 3 / 2];
        static bool grayInit = false;
        if (!grayInit) {
            memset(grayFrame, 128, sizeof(grayFrame));     // Y: 灰色
            memset(grayFrame + 1920*1080, 128, 1920*1080/2); // UV: 灰色
            grayInit = true;
        }
        // 实际上需要写入 GraphicBuffer，这里只做标记
        LOGI("  fillBuffer[%d]: no frame, using gray fill", streamId);
        return true;
    }

    // 复制帧数据到输出 buffer
    // 实际实现中需要:
    // 1. lock GraphicBuffer
    // 2. 根据格式转换 (YUV/NV21/RGB)
    // 3. memcpy 到 buffer->bits
    // 4. unlock
    LOGI("  fillBuffer[%d]: frame pts=%lld, %dx%d, %zu bytes",
         streamId, (long long)vf.pts, vf.width, vf.height, vf.size);

    if (vf.data) free(vf.data);
    return true;
}

const uint8_t* VcamCameraSession::getDefaultRequestSettings(
    int type, size_t* outSize) {
    // 返回最小合法 metadata
    // TEMPLATE_PREVIEW = 1, TEMPLATE_STILL_CAPTURE = 2,
    // TEMPLATE_VIDEO_RECORD = 3, TEMPLATE_VIDEO_SNAPSHOT = 4
    static uint8_t defaultSettings[1024];
    static bool init = false;
    if (!init) {
        memset(defaultSettings, 0, sizeof(defaultSettings));
        init = true;
    }
    *outSize = sizeof(defaultSettings);
    return defaultSettings;
}

void VcamCameraSession::frameLoop() {
    LOGI("Frame loop started");
    while (!m_closed && m_streaming) {
        // 定期推送帧 (30fps = ~33ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(33));

        // 实际实现中需要通知 CameraService 新帧可用
        // processCaptureResult() 回调
    }
    LOGI("Frame loop stopped");
}

} // namespace vcam
