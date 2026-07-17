/**
 * VcamConfig - 虚拟摄像头配置管理
 *
 * 读取 /data/local/tmp/vcam_config.json，提供视频路径、分辨率等配置。
 * 控制 APK 通过 root 权限写入配置，虚拟 HAL daemon 读取。
 */

#ifndef VCAM_CONFIG_H
#define VCAM_CONFIG_H

#include <string>
#include <mutex>

namespace vcam {

struct CameraConfig {
    std::string video_path;
    int video_width     = 1920;
    int video_height    = 1080;
    bool disabled       = true;   // true=使用真实摄像头, false=虚拟
    bool image_correction = false;
    bool loop_playback  = true;
    int mode            = 0;      // 0=video, 1=stream
};

class VcamConfig {
public:
    static VcamConfig& instance();

    void reload();
    void save();

    const CameraConfig& getConfig() const { return m_config; }
    bool isDisabled() const { return m_config.disabled; }
    const std::string& getVideoPath() const { return m_config.video_path; }
    int getVideoWidth() const { return m_config.video_width; }
    int getVideoHeight() const { return m_config.video_height; }
    bool isImageCorrection() const { return m_config.image_correction; }
    bool isLoopPlayback() const { return m_config.loop_playback; }

    void setDisabled(bool disabled);
    void setVideoPath(const std::string& path);
    void setVideoResolution(int w, int h);

    static constexpr const char* CONFIG_PATH = "/data/local/tmp/vcam_config.json";

private:
    VcamConfig();
    CameraConfig m_config;
    mutable std::mutex m_mutex;
};

} // namespace vcam

#endif // VCAM_CONFIG_H
