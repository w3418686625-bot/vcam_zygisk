/**
 * FrameProvider - 视频解码 + 帧缓存服务
 *
 * 使用 Android MediaCodec (NDK) 解码 MP4 → YUV420/NV21 帧。
 * 维护一个帧缓冲区，当 Camera HAL 需要帧时取出最新的解码帧。
 *
 * 解码流程：
 *   1. 打开 MP4 文件 → MediaExtractor
 *   2. 选择视频轨道 → 获取 MediaFormat
 *   3. 创建 MediaCodec 解码器
 *   4. 循环：送数据 → 取解码帧 → 缓存
 */

#ifndef VCAM_FRAME_PROVIDER_H
#define VCAM_FRAME_PROVIDER_H

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>

namespace vcam {

struct VideoFrame {
    uint8_t* data     = nullptr;
    size_t   size     = 0;
    int      width    = 0;
    int      height   = 0;
    int      stride   = 0;
    int64_t  pts      = 0;
    uint32_t format   = 0;  // HAL_PIXEL_FORMAT_YCrCb_420_SP (NV21) = 0x11
};

class FrameProvider {
public:
    static FrameProvider& instance();

    bool start(const char* videoPath, int targetWidth = 0, int targetHeight = 0);
    void stop();
    bool isRunning() const { return m_running; }

    // 获取最新解码帧（线程安全），返回拷贝
    bool getLatestFrame(VideoFrame& outFrame);

    // 获取帧信息（不解码）
    int getFrameWidth() const  { std::lock_guard<std::mutex> lk(m_mutex); return m_currentFrame.width; }
    int getFrameHeight() const { std::lock_guard<std::mutex> lk(m_mutex); return m_currentFrame.height; }
    int getFrameStride() const { std::lock_guard<std::mutex> lk(m_mutex); return m_currentFrame.stride; }

private:
    FrameProvider() = default;
    ~FrameProvider();

    void decoderLoop();
    bool decodeFrame(int64_t timeoutUs = 10000);

    std::atomic<bool> m_running{false};
    std::thread m_decoderThread;

    mutable std::mutex m_mutex;
    VideoFrame m_currentFrame;

    // MediaCodec/Extractor 相关在 .cpp 中管理
    void* m_codec       = nullptr;  // AMediaCodec*
    void* m_extractor   = nullptr;  // AMediaExtractor*
    bool   m_eos        = false;
    bool   m_needRestart = false;

    std::string m_videoPath;
    int m_targetWidth  = 0;
    int m_targetHeight = 0;
};

} // namespace vcam

#endif // VCAM_FRAME_PROVIDER_H
