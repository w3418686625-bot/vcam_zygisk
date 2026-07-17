#include "VcamConfig.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "VCAM-HAL-Config"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vcam {

static const char* DEFAULT_VIDEO_PATH = "/data/local/tmp/vcam/virtual.mp4";

VcamConfig::VcamConfig() {
    m_config.video_path = DEFAULT_VIDEO_PATH;
    reload();
}

VcamConfig& VcamConfig::instance() {
    static VcamConfig inst;
    return inst;
}

void VcamConfig::reload() {
    std::lock_guard<std::mutex> lock(m_mutex);
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) {
        LOGI("Config file not found, using defaults");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0 || size > 65536) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(size + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    // 简易 JSON 解析（不引入 json 库）
    auto findValue = [&](const char* key, std::string& out) {
        const char* p = strstr(buf, key);
        if (!p) return;
        p = strchr(p, ':');
        if (!p) return;
        p++;
        while (*p == ' ' || *p == '"') p++;
        const char* end = p;
        while (*end && *end != '"' && *end != ',' && *end != '\n' && *end != '}') end++;
        out.assign(p, end - p);
    };

    auto findInt = [&](const char* key, int& out) {
        std::string s;
        findValue(key, s);
        if (!s.empty()) out = atoi(s.c_str());
    };

    auto findBool = [&](const char* key, bool& out) {
        std::string s;
        findValue(key, s);
        if (s == "true" || s == "1") out = true;
        else if (s == "false" || s == "0") out = false;
    };

    findValue("video_path", m_config.video_path);
    findInt("video_width", m_config.video_width);
    findInt("video_height", m_config.video_height);
    findBool("disabled", m_config.disabled);
    findBool("image_correction", m_config.image_correction);
    findBool("loop_playback", m_config.loop_playback);
    findInt("mode", m_config.mode);

    free(buf);

    LOGI("Config loaded: path=%s, %dx%d, disabled=%d, loop=%d",
         m_config.video_path.c_str(), m_config.video_width, m_config.video_height,
         m_config.disabled, m_config.loop_playback);
}

void VcamConfig::setDisabled(bool disabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.disabled = disabled;
}

void VcamConfig::setVideoPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.video_path = path;
}

void VcamConfig::setVideoResolution(int w, int h) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.video_width = w;
    m_config.video_height = h;
}

} // namespace vcam
