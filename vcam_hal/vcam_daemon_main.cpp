/**
 * vcam_daemon_main.cpp - 虚拟摄像头 HAL Daemon 入口
 *
 * 此 daemon 以 root 权限运行，替换系统原厂的 Camera HAL 服务。
 *
 * 工作原理：
 *   1. 查找正在运行的 camera provider 服务
 *   2. 杀掉原厂 camera provider 进程
 *   3. 以相同服务名注册自己的虚拟 camera provider
 *   4. hwservicemanager 会将后续的 camera 请求路由到我们
 *   5. 所有 App 通过标准 Camera2/Camera1 API 获取虚拟视频画面
 *
 * Android 12+ 使用 AIDL，Android 10-12 使用 HIDL。
 * 本 daemon 实现 HIDL android.hardware.camera.provider@2.4。
 *
 * 编译: NDK 交叉编译为 aarch64 可执行文件
 * 部署: /data/local/tmp/vcam/vcam_daemon
 * 启动: /data/local/tmp/vcam/vcam_daemon &
 * 停止: kill $(pidof vcam_daemon)
 */

#include "VcamCameraProvider.h"
#include "VcamConfig.h"
#include "FrameProvider.h"
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <thread>
#include <chrono>

#define LOG_TAG "VCAM-Daemon"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// 原厂 camera provider 常见进程名
const char* REAL_PROVIDER_NAMES[] = {
    "android.hardware.camera.provider@2.4-service",
    "android.hardware.camera.provider@2.5-service",
    "android.hardware.camera.provider@2.6-service",
    "android.hardware.camera.provider@2.7-service",
    "camera.provider",
    "vendor.qti.camera.provider",
    "vendor.oplus.camera.provider",
    "vendor.oplus.hardware.camera.provider",
    nullptr
};

// 获取进程 PID
int findProcessPid(const char* name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", name);
    FILE* fp = popen(cmd, "r");
    if (!fp) return -1;
    char buf[64];
    int pid = -1;
    if (fgets(buf, sizeof(buf), fp)) {
        pid = atoi(buf);
    }
    pclose(fp);
    return pid;
}

// 杀掉原厂 camera provider
bool killRealCameraProvider() {
    LOGI("Searching for real camera provider to replace...");
    bool killed = false;

    for (int i = 0; REAL_PROVIDER_NAMES[i]; i++) {
        int pid = findProcessPid(REAL_PROVIDER_NAMES[i]);
        if (pid > 1) {
            LOGI("Found real provider '%s' (PID=%d), killing...",
                 REAL_PROVIDER_NAMES[i], pid);
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "kill -9 %d 2>/dev/null", pid);
            system(cmd);
            killed = true;
        }
    }

    // 同时 kill cameraserver 使其重新发现 provider
    int csPid = findProcessPid("cameraserver");
    if (csPid > 1) {
        LOGI("Killing cameraserver (PID=%d) to force re-discovery", csPid);
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "kill -9 %d 2>/dev/null", csPid);
        system(cmd);
    }

    // 等待旧服务完全退出
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return killed;
}

// 创建 /dev/vcam 等配置文件
bool setupEnvironment() {
    system("mkdir -p /data/local/tmp/vcam 2>/dev/null");
    system("chmod 755 /data/local/tmp/vcam 2>/dev/null");

    // 确保配置目录可访问
    system("chmod 755 /data/local/tmp 2>/dev/null");
    return true;
}

volatile sig_atomic_t g_running = 1;

void signalHandler(int sig) {
    LOGI("Received signal %d, shutting down...", sig);
    g_running = 0;
}

// 配置检查线程：定期检查控制 APK 写入的配置变化
void configWatcher() {
    using namespace std::chrono;
    auto lastCheck = steady_clock::now();

    while (g_running) {
        std::this_thread::sleep_for(seconds(2));
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - lastCheck).count() >= 2) {
            vcam::VcamConfig::instance().reload();

            auto& cfg = vcam::VcamConfig::instance();
            if (!cfg.isDisabled()) {
                // 确保解码器在运行
                if (!vcam::FrameProvider::instance().isRunning()) {
                    const auto& vc = cfg.getConfig();
                    LOGI("Starting FrameProvider: %s", vc.video_path.c_str());
                    vcam::FrameProvider::instance().start(
                        vc.video_path.c_str(),
                        vc.video_width, vc.video_height);
                }
            } else {
                // 禁用时停止解码
                if (vcam::FrameProvider::instance().isRunning()) {
                    vcam::FrameProvider::instance().stop();
                }
            }

            lastCheck = now;
        }
    }
}

} // anonymous namespace

int main(int argc, char** argv) {
    LOGI("========================================");
    LOGI("VCAM Virtual Camera HAL Daemon v3.0");
    LOGI("PID=%d, UID=%d", getpid(), getuid());
    LOGI("========================================");

    // 信号处理
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);

    // 环境准备
    if (!setupEnvironment()) {
        LOGE("Failed to setup environment");
        return 1;
    }

    // 读取配置
    vcam::VcamConfig::instance().reload();
    LOGI("Config loaded: disabled=%d", vcam::VcamConfig::instance().isDisabled());

    // 初始化 Provider
    if (!vcam::VcamCameraProvider::instance().initialize()) {
        LOGE("Failed to initialize camera provider");
        return 1;
    }

    // 杀掉原厂 HAL 并注册自己
    LOGI("Attempting to replace real camera HAL...");
    if (killRealCameraProvider()) {
        LOGI("Real camera provider killed. Virtual HAL will take over.");
    } else {
        LOGI("No real camera provider found running. May already be replaced.");
    }

    // 启动配置监听线程
    std::thread watcher(configWatcher);
    watcher.detach();

    // 启动视频解码（如果未 disabled）
    if (!vcam::VcamConfig::instance().isDisabled()) {
        const auto& cfg = vcam::VcamConfig::instance().getConfig();
        vcam::FrameProvider::instance().start(
            cfg.video_path.c_str(),
            cfg.video_width, cfg.video_height);
    }

    LOGI("========================================");
    LOGI("Virtual Camera HAL is NOW ACTIVE");
    LOGI("All apps will see virtual camera when 'disabled'=false");
    LOGI("Kill this daemon to restore real camera.");
    LOGI("========================================");

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // 定期检查 cameraserver 状态
        static int csCheckCount = 0;
        if (++csCheckCount % 12 == 0) { // 每分钟检查一次
            int pid = findProcessPid("cameraserver");
            if (pid <= 0) {
                LOGI("cameraserver not running (will be auto-restarted by init)");
            }
        }
    }

    // 清理
    LOGI("Shutting down Virtual Camera HAL...");
    vcam::FrameProvider::instance().stop();
    vcam::VcamCameraProvider::instance().shutdown();

    LOGI("Virtual Camera HAL stopped.");
    return 0;
}
