/**
 * vcam_hook.cpp - 系统级摄像头替换 Hook 库
 * 
 * 注入到 cameraserver 进程中，Hook ANativeWindow buffer 操作，
 * 将摄像头画面替换为视频帧数据。
 *
 * 原理：
 *   cameraserver 将摄像头帧通过 ANativeWindow (Surface) 传递给 App。
 *   我们 Hook dequeueBuffer/queueBuffer，在帧数据到达 App 之前替换内容。
 *
 * 编译：
 *   aarch64-linux-android31-clang++ -std=c++17 -O2 -fPIC -shared \
 *     -o libvcam_hook.so vcam_hook.cpp -llog -ldl
 */

#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include <android/log.h>
#include <android/native_window.h>
#include <sys/mman.h>

#define LOG_TAG "VCAM-HOOK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ===== 配置 ===== */
static const char *VIDEO_PATH = "/data/local/tmp/vcam/virtual.mp4";
static volatile int g_hook_active = 1;

/* ===== 原始函数指针 ===== */
static int (*orig_dequeueBuffer)(ANativeWindow *window,
    ANativeWindowBuffer **buffer, int *fenceFd) = nullptr;
static int (*orig_queueBuffer)(ANativeWindow *window,
    ANativeWindowBuffer *buffer, int fenceFd) = nullptr;
static int (*orig_cancelBuffer)(ANativeWindow *window,
    ANativeWindowBuffer *buffer, int fenceFd) = nullptr;

/* ===== Native Window hook vtable ===== */
typedef struct ANativeWindow_Opaque {
    // vtable pointer
    void *vtable;
} ANativeWindow_Opaque;

typedef int (*DequeueBufferFn)(ANativeWindow *, ANativeWindowBuffer **, int *);
typedef int (*QueueBufferFn)(ANativeWindow *, ANativeWindowBuffer *, int);
typedef int (*CancelBufferFn)(ANativeWindow *, ANativeWindowBuffer *, int);

struct ANativeWindowVTable {
    void *reserved[4];
    DequeueBufferFn dequeueBuffer;
    QueueBufferFn queueBuffer;
    CancelBufferFn cancelBuffer;
    // ... more vtable entries
};

/* ===== 视频解码器状态 ===== */
static pthread_t g_decoder_thread = 0;
static volatile int g_decoder_running = 0;
static uint8_t *g_video_frame = nullptr;
static size_t g_frame_size = 0;
static int g_frame_width = 0;
static int g_frame_height = 0;
static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ===== Hook 实现 ===== */

// 被 hook 的 dequeueBuffer - 不修改，直接透传
static int hook_dequeueBuffer(ANativeWindow *window,
                               ANativeWindowBuffer **buffer, int *fenceFd) {
    return orig_dequeueBuffer(window, buffer, fenceFd);
}

// 被 hook 的 queueBuffer - 在这里替换帧数据
static int hook_queueBuffer(ANativeWindow *window,
                             ANativeWindowBuffer *buffer, int fenceFd) {
    if (!g_hook_active || !g_video_frame || g_frame_size == 0) {
        return orig_queueBuffer(window, buffer, fenceFd);
    }

    // 尝试用视频帧替换 buffer 内容
    pthread_mutex_lock(&g_frame_mutex);
    if (g_video_frame && buffer && buffer->handle) {
        void *dst = nullptr;
        // 尝试 lock buffer 获取可写地址
        // GraphicBuffer 的 lock 需要先获取 native_handle
        // 这里使用一个简化的方法：直接写 buffer 的底层内存
        // 实际生产中需要根据 buffer 格式做正确的 YUV/NV21 转换
        
        // 对于大多数 Android 设备，camera buffer 是 NV21 或 YUV420
        // 我们尝试通过 mmap 或直接写入来替换
        
        // 使用 ANativeWindow_lock 获取 buffer 地址（如果可以的话）
        ANativeWindow_Buffer outBuffer;
        if (ANativeWindow_lock(window, &outBuffer, nullptr) == 0) {
            if (outBuffer.bits && g_video_frame) {
                size_t copySize = (g_frame_size < outBuffer.stride * outBuffer.height * 3 / 2)
                    ? g_frame_size
                    : outBuffer.stride * outBuffer.height * 3 / 2;
                memcpy(outBuffer.bits, g_video_frame, copySize);
            }
            ANativeWindow_unlockAndPost(window);
        }
    }
    pthread_mutex_unlock(&g_frame_mutex);

    return orig_queueBuffer(window, buffer, fenceFd);
}

static int hook_cancelBuffer(ANativeWindow *window,
                              ANativeWindowBuffer *buffer, int fenceFd) {
    return orig_cancelBuffer(window, buffer, fenceFd);
}

/* ===== 视频解码线程 ===== */
static int readVideoResolution(const char *path, int *w, int *h) {
    // 读取配置文件获取分辨率
    FILE *f = fopen("/data/local/tmp/vcam_config.json", "r");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = (char *)malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    
    // 简单解析 JSON
    char *wp = strstr(buf, "\"video_width\"");
    char *hp = strstr(buf, "\"video_height\"");
    if (wp) {
        wp = strchr(wp, ':');
        if (wp) *w = atoi(wp + 1);
    }
    if (hp) {
        hp = strchr(hp, ':');
        if (hp) *h = atoi(hp + 1);
    }
    free(buf);
    return (*w > 0 && *h > 0) ? 0 : -1;
}

static void *decoder_thread(void *arg) {
    LOGI("Video decoder thread started");
    
    int w = 0, h = 0;
    readVideoResolution(VIDEO_PATH, &w, &h);
    
    if (w <= 0) w = 1920;  // 默认值
    if (h <= 0) h = 1080;
    
    g_frame_width = w;
    g_frame_height = h;
    g_frame_size = w * h * 3 / 2;  // NV21 格式大小
    
    // 从视频文件读取第一帧作为静态帧（简单方案）
    // 完整实现需要 MediaCodec 异步解码循环
    FILE *vf = fopen(VIDEO_PATH, "rb");
    if (vf) {
        // MP4 文件头跳过，寻找 mdat box 中的实际帧数据
        // 简化实现：直接分配一帧 NV21 渐变测试数据
        fclose(vf);
    }
    
    // 分配帧缓冲区
    pthread_mutex_lock(&g_frame_mutex);
    g_video_frame = (uint8_t *)malloc(g_frame_size);
    if (g_video_frame) {
        // 填充测试帧：灰色背景
        memset(g_video_frame, 128, w * h);           // Y 平面 (灰色)
        memset(g_video_frame + w * h, 128, w * h / 2); // UV 平面 (无色度)
    }
    pthread_mutex_unlock(&g_frame_mutex);
    
    LOGI("Video frame buffer ready: %dx%d, %zu bytes", w, h, g_frame_size);
    
    // 主循环：持续解码视频帧
    // TODO: 使用 MediaCodec 解码 MP4 → NV21 frame
    // 当前简化版：直接填充灰色画面验证 hook 链通
    while (g_decoder_running) {
        sleep(1);
    }
    
    LOGI("Decoder thread stopped");
    return nullptr;
}

/* ===== 查找函数地址 ===== */
static void *find_func_in_lib(const char *libname, const char *funcname) {
    void *handle = dlopen(libname, RTLD_NOLOAD);
    if (!handle) return nullptr;
    void *addr = dlsym(handle, funcname);
    dlclose(handle);
    return addr;
}

/* ===== Hook 安装 ===== */

// 替换 ANativeWindow vtable 中的函数指针
static int install_window_hooks() {
    // 查找原始函数
    void *dequeue = find_func_in_lib("libnativewindow.so", "_ZN7android12ANativeWindow14dequeueBufferEPP19ANativeWindowBufferPi");
    void *queue = find_func_in_lib("libnativewindow.so", "_ZN7android12ANativeWindow11queueBufferEP19ANativeWindowBufferi");
    void *cancel = find_func_in_lib("libnativewindow.so", "_ZN7android12ANativeWindow12cancelBufferEP19ANativeWindowBufferi");
    
    if (!dequeue && !queue) {
        // 尝试 C 符号名
        dequeue = find_func_in_lib("libnativewindow.so", "ANativeWindow_dequeueBuffer");
        queue = find_func_in_lib("libnativewindow.so", "ANativeWindow_queueBuffer");
        cancel = find_func_in_lib("libnativewindow.so", "ANativeWindow_cancelBuffer");
    }
    
    if (queue) {
        orig_queueBuffer = (QueueBufferFn)queue;
        LOGI("Found queueBuffer @ %p", queue);
    }
    if (dequeue) {
        orig_dequeueBuffer = (DequeueBufferFn)dequeue;
        LOGI("Found dequeueBuffer @ %p", dequeue);
    }
    if (cancel) {
        orig_cancelBuffer = (CancelBufferFn)cancel;
    }
    
    // 使用 LD_PRELOAD 替换全局符号
    // 当 .so 通过 LD_PRELOAD 加载时，同名的弱符号会被覆盖
    // 我们使用 __attribute__((visibility("default"))) 确保符号可见
    
    LOGI("Window hooks installed: dq=%p q=%p cancel=%p",
         (void*)orig_dequeueBuffer, (void*)orig_queueBuffer, (void*)orig_cancelBuffer);
    
    return (orig_queueBuffer != nullptr) ? 0 : -1;
}

/* ===== 通过 LD_PRELOAD 拦截 ANativeWindow 函数 ===== */
// 当此 .so 通过 LD_PRELOAD 加载时，这些符号会自动替换系统实现

extern "C" {

// 拦截 dequeueBuffer
__attribute__((visibility("default")))
int ANativeWindow_dequeueBuffer(ANativeWindow *window,
    ANativeWindowBuffer **buffer, int *fenceFd) {
    if (!orig_dequeueBuffer) {
        orig_dequeueBuffer = (DequeueBufferFn)dlsym(RTLD_NEXT, "ANativeWindow_dequeueBuffer");
    }
    return hook_dequeueBuffer(window, buffer, fenceFd);
}

// 拦截 queueBuffer - 关键 hook 点
__attribute__((visibility("default")))
int ANativeWindow_queueBuffer(ANativeWindow *window,
    ANativeWindowBuffer *buffer, int fenceFd) {
    if (!orig_queueBuffer) {
        orig_queueBuffer = (QueueBufferFn)dlsym(RTLD_NEXT, "ANativeWindow_queueBuffer");
    }
    return hook_queueBuffer(window, buffer, fenceFd);
}

// 拦截 cancelBuffer
__attribute__((visibility("default")))
int ANativeWindow_cancelBuffer(ANativeWindow *window,
    ANativeWindowBuffer *buffer, int fenceFd) {
    if (!orig_cancelBuffer) {
        orig_cancelBuffer = (CancelBufferFn)dlsym(RTLD_NEXT, "ANativeWindow_cancelBuffer");
    }
    return hook_cancelBuffer(window, buffer, fenceFd);
}

} // extern "C"

/* ===== 自动初始化（constructor） ===== */
__attribute__((constructor))
static void vcam_hook_init() {
    LOGI("========================================");
    LOGI("VCAM System Hook loading...");
    LOGI("PID=%d, UID=%d", getpid(), getuid());
    LOGI("========================================");
    
    // 安装 hook
    if (install_window_hooks() == 0) {
        LOGI("Camera hooks installed successfully");
    } else {
        LOGE("Failed to install camera hooks");
        return;
    }
    
    // 启动视频解码线程
    g_decoder_running = 1;
    pthread_create(&g_decoder_thread, nullptr, decoder_thread, nullptr);
    
    LOGI("VCAM System Hook active - all apps will see virtual camera");
}

__attribute__((destructor))
static void vcam_hook_fini() {
    LOGI("VCAM System Hook unloading...");
    g_decoder_running = 0;
    g_hook_active = 0;
    
    if (g_decoder_thread) {
        pthread_join(g_decoder_thread, nullptr);
    }
    
    pthread_mutex_lock(&g_frame_mutex);
    if (g_video_frame) {
        free(g_video_frame);
        g_video_frame = nullptr;
    }
    pthread_mutex_unlock(&g_frame_mutex);
    
    LOGI("VCAM System Hook unloaded");
}
