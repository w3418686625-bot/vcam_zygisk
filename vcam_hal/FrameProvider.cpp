#include "FrameProvider.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>
#include <cstring>

#define LOG_TAG "VCAM-HAL-Frame"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vcam {

FrameProvider& FrameProvider::instance() {
    static FrameProvider inst;
    return inst;
}

FrameProvider::~FrameProvider() {
    stop();
}

bool FrameProvider::start(const char* videoPath, int targetWidth, int targetHeight) {
    if (m_running) stop();

    m_videoPath = videoPath;
    m_targetWidth = targetWidth > 0 ? targetWidth : 1920;
    m_targetHeight = targetHeight > 0 ? targetHeight : 1080;

    // 1. 创建 Extractor
    AMediaExtractor* ex = AMediaExtractor_new();
    media_status_t status = AMediaExtractor_setDataSourceFd(ex, -1, 0, 0);
    // fd-based won't work here, use path-based
    AMediaExtractor_delete(ex);
    ex = nullptr;

    FILE* f = fopen(videoPath, "rb");
    if (!f) {
        LOGE("Cannot open video file: %s", videoPath);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    int fd = fileno(f);

    ex = AMediaExtractor_new();
    status = AMediaExtractor_setDataSourceFd(ex, fd, 0, fileSize);
    if (status != AMEDIA_OK) {
        LOGE("AMediaExtractor_setDataSourceFd failed: %d", status);
        fclose(f);
        AMediaExtractor_delete(ex);
        return false;
    }

    // 2. 选择视频轨道
    size_t numTracks = AMediaExtractor_getTrackCount(ex);
    int videoTrack = -1;
    for (size_t i = 0; i < numTracks; i++) {
        AMediaFormat* fmt = AMediaExtractor_getTrackFormat(ex, i);
        const char* mime;
        AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime);
        if (strncmp(mime, "video/", 6) == 0) {
            videoTrack = i;
            int w, h;
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
            LOGI("Video track found: %dx%d, mime=%s", w, h, mime);
            m_currentFrame.width = w;
            m_currentFrame.height = h;
            AMediaFormat_delete(fmt);
            break;
        }
        AMediaFormat_delete(fmt);
    }

    if (videoTrack < 0) {
        LOGE("No video track found");
        fclose(f);
        AMediaExtractor_delete(ex);
        return false;
    }

    AMediaExtractor_selectTrack(ex, videoTrack);

    // 3. 创建解码器
    AMediaFormat* trackFmt = AMediaExtractor_getTrackFormat(ex, videoTrack);
    const char* mime;
    AMediaFormat_getString(trackFmt, AMEDIAFORMAT_KEY_MIME, &mime);

    // 设置输出格式为 YUV420 (NV21)
    AMediaFormat_setInt32(trackFmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); // COLOR_FormatYUV420SemiPlanar

    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) {
        LOGE("Cannot create decoder for %s", mime);
        AMediaFormat_delete(trackFmt);
        AMediaExtractor_delete(ex);
        fclose(f);
        return false;
    }

    status = AMediaCodec_configure(codec, trackFmt, nullptr, nullptr, 0);
    AMediaFormat_delete(trackFmt);

    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", status);
        AMediaCodec_delete(codec);
        AMediaExtractor_delete(ex);
        fclose(f);
        return false;
    }

    status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", status);
        AMediaCodec_delete(codec);
        AMediaExtractor_delete(ex);
        fclose(f);
        return false;
    }

    m_codec = codec;
    m_extractor = ex;
    m_eos = false;
    m_needRestart = false;

    // 4. 启动解码线程
    m_running = true;
    m_decoderThread = std::thread(&FrameProvider::decoderLoop, this);

    LOGI("FrameProvider started: %s, %dx%d", videoPath,
         m_currentFrame.width, m_currentFrame.height);
    return true;
}

void FrameProvider::stop() {
    m_running = false;
    if (m_decoderThread.joinable()) {
        m_decoderThread.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_currentFrame.data) {
        free(m_currentFrame.data);
        m_currentFrame.data = nullptr;
    }

    if (m_codec) {
        AMediaCodec_stop((AMediaCodec*)m_codec);
        AMediaCodec_delete((AMediaCodec*)m_codec);
        m_codec = nullptr;
    }
    if (m_extractor) {
        AMediaExtractor_delete((AMediaExtractor*)m_extractor);
        m_extractor = nullptr;
    }
    LOGI("FrameProvider stopped");
}

void FrameProvider::decoderLoop() {
    LOGI("Decoder loop started");

    while (m_running) {
        if (m_needRestart) {
            // TODO: 循环播放时重新开始
            m_needRestart = false;
        }

        if (!m_eos) {
            // 送数据到解码器
            auto* codec = (AMediaCodec*)m_codec;
            ssize_t inputIdx = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (inputIdx >= 0) {
                size_t bufSize;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec, inputIdx, &bufSize);
                auto* ex = (AMediaExtractor*)m_extractor;
                ssize_t sampleSize = AMediaExtractor_readSampleData(ex, buf, bufSize);
                if (sampleSize >= 0) {
                    int64_t pts = AMediaExtractor_getSampleTime(ex);
                    AMediaCodec_queueInputBuffer(codec, inputIdx, 0, sampleSize,
                                                  (uint64_t)pts, 0);
                    AMediaExtractor_advance(ex);
                } else {
                    // EOS
                    AMediaCodec_queueInputBuffer(codec, inputIdx, 0, 0, 0,
                                                  AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    m_eos = true;
                }
            }
        }

        // 取解码输出
        if (!decodeFrame(15000)) {
            if (m_eos) {
                // 循环：重新 seek 到开头
                auto* ex = (AMediaExtractor*)m_extractor;
                AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
                m_eos = false;

                // 重新配置 codec
                auto* codec = (AMediaCodec*)m_codec;
                AMediaCodec_flush(codec);
                LOGI("Loop: restarted from beginning");
            }
        }
    }

    LOGI("Decoder loop stopped");
}

bool FrameProvider::decodeFrame(int64_t timeoutUs) {
    auto* codec = (AMediaCodec*)m_codec;
    if (!codec) return false;

    AMediaCodecBufferInfo info;
    ssize_t outputIdx = AMediaCodec_dequeueOutputBuffer(codec, &info, timeoutUs);

    if (outputIdx >= 0) {
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
            AMediaCodec_releaseOutputBuffer(codec, outputIdx, false);
            return false;
        }

        size_t bufSize;
        uint8_t* buf = AMediaCodec_getOutputBuffer(codec, outputIdx, &bufSize);
        if (buf && info.size > 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_currentFrame.data) free(m_currentFrame.data);

            m_currentFrame.data = (uint8_t*)malloc(info.size);
            if (m_currentFrame.data) {
                memcpy(m_currentFrame.data, buf + info.offset, info.size);
                m_currentFrame.size = info.size;
                m_currentFrame.pts = info.presentationTimeUs;
                m_currentFrame.stride = m_currentFrame.width; // YUV420: stride = width
            }
        }

        AMediaCodec_releaseOutputBuffer(codec, outputIdx, false);
        return true;
    } else if (outputIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec);
        int w, h, stride;
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
        if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_STRIDE, &stride)) {
            m_currentFrame.stride = stride;
        } else {
            m_currentFrame.stride = w;
        }
        m_currentFrame.width = w;
        m_currentFrame.height = h;
        LOGI("Output format: %dx%d, stride=%d", w, h, m_currentFrame.stride);
        AMediaFormat_delete(fmt);
        return false;
    } else if (outputIdx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        return false;
    }

    return false;
}

bool FrameProvider::getLatestFrame(VideoFrame& outFrame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_currentFrame.data || m_currentFrame.size == 0) return false;

    outFrame.width  = m_currentFrame.width;
    outFrame.height = m_currentFrame.height;
    outFrame.stride = m_currentFrame.stride;
    outFrame.pts    = m_currentFrame.pts;
    outFrame.size   = m_currentFrame.size;

    outFrame.data = (uint8_t*)malloc(m_currentFrame.size);
    if (!outFrame.data) return false;
    memcpy(outFrame.data, m_currentFrame.data, m_currentFrame.size);
    return true;
}

} // namespace vcam
