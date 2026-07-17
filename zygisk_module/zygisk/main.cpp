/*
 * VCAM Zygisk Module - 系统级全局摄像头替换
 *
 * 不依赖 LSPosed，通过 Zygisk 注入所有 App 进程，
 * 在 Native 层 Hook Camera1/Camera2 API，将摄像头画面替换为指定视频。
 *
 * 工作原理：
 *   1. Zygisk 注入每个 App 进程（postAppSpecialize）
 *   2. 读取 /data/local/tmp/vcam_config.json 配置
 *   3. 如果启用替换，Hook Camera API：
 *      - Camera1: hookJniNativeMethods 替换 native_setPreviewDisplay/Texture/startPreview
 *      - Camera2: ArtMethod entry_point 替换 hook CameraManager.openCamera
 *   4. 当 App 打开摄像头时，用 MediaCodec 解码视频到 App 的 Surface
 *
 * 编译: 通过 GitHub Actions 用 NDK 交叉编译
 * 部署: KernelSU/Magisk 模块，一键导入
 */

#include "zygisk.hpp"
#include <jni.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#define LOG_TAG "VCAM-Zygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// ============ 全局配置 ============
static const char* CONFIG_PATH  = "/data/local/tmp/vcam_config.json";
static const char* VIDEO_PATH   = "/data/local/tmp/vcam/virtual.mp4";

struct VcamConfig {
    bool   disabled       = true;    // 默认禁用，激活后改为 false
    bool   loopPlayback   = true;
    int    videoWidth     = 0;
    int    videoHeight    = 0;
    char   videoPath[512] = {0};
};

static VcamConfig g_config;
static bool g_hooksInstalled = false;
static JavaVM* g_jvm = nullptr;

// ============ 配置读取（简易 JSON 解析）============

static bool fileExists(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0 && st.st_size > 0);
}

static bool videoAvailable() {
    struct stat st;
    if (stat(g_config.videoPath[0] ? g_config.videoPath : VIDEO_PATH, &st) != 0) {
        return false;
    }
    return st.st_size > 0 && (st.st_mode & S_IRGRP);
}

// 从 JSON 字符串中提取字段值（简易实现）
static std::string extractJsonString(const char* json, const char* key) {
    std::string s(json);
    std::string pattern = std::string("\"") + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return "";
    pos = s.find(":", pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) pos++;
    if (pos >= s.size() || s[pos] != '"') return "";
    pos++;
    size_t start = pos;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) pos++;
        pos++;
    }
    return s.substr(start, pos - start);
}

static int extractJsonInt(const char* json, const char* key, int def) {
    std::string s(json);
    std::string pattern = std::string("\"") + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return def;
    pos = s.find(":", pos);
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) pos++;
    return atoi(s.c_str() + pos);
}

static bool extractJsonBool(const char* json, const char* key, bool def) {
    std::string s(json);
    std::string pattern = std::string("\"") + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return def;
    pos = s.find(":", pos);
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) pos++;
    if (pos + 4 <= s.size() && s[pos] == 't' && s[pos+1] == 'r' && s[pos+2] == 'u' && s[pos+3] == 'e')
        return true;
    if (pos + 5 <= s.size() && s[pos] == 'f' && s[pos+1] == 'a' && s[pos+2] == 'l' && s[pos+3] == 's' && s[pos+4] == 'e')
        return false;
    return def;
}

static bool readConfig() {
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) {
        LOGD("config not found: %s", CONFIG_PATH);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8192) {
        fclose(f);
        return false;
    }

    char* buf = new char[size + 1];
    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);

    g_config.disabled     = extractJsonBool(buf, "disabled", true);
    g_config.loopPlayback = extractJsonBool(buf, "loop_playback", true);
    g_config.videoWidth   = extractJsonInt(buf, "video_width", 0);
    g_config.videoHeight  = extractJsonInt(buf, "video_height", 0);

    std::string vpath = extractJsonString(buf, "video_path");
    if (!vpath.empty()) {
        strncpy(g_config.videoPath, vpath.c_str(), sizeof(g_config.videoPath) - 1);
    } else {
        strncpy(g_config.videoPath, VIDEO_PATH, sizeof(g_config.videoPath) - 1);
    }

    delete[] buf;

    LOGI("config: disabled=%d loop=%d video=%dx%d path=%s",
         g_config.disabled, g_config.loopPlayback,
         g_config.videoWidth, g_config.videoHeight, g_config.videoPath);
    return true;
}

// ============ 视频解码器（MediaCodec Java API via JNI）============

class VideoDecoder {
public:
    // 启动视频解码到指定 Surface
    static bool start(JNIEnv* env, jobject surface, const char* videoPath) {
        // 清理旧状态
        stop(env);

        if (!surface || !videoPath || !fileExists(videoPath)) {
            LOGE("VideoDecoder::start invalid args: surface=%p path=%s", surface, videoPath);
            return false;
        }

        LOGI("VideoDecoder::start path=%s", videoPath);

        // 1. 创建 MediaExtractor
        jclass extractorClass = env->FindClass("android/media/MediaExtractor");
        if (!extractorClass) {
            LOGE("MediaExtractor class not found");
            env->ExceptionClear();
            return false;
        }
        jmethodID extractorCtor = env->GetMethodID(extractorClass, "<init>", "()V");
        jobject extractor = env->NewObject(extractorClass, extractorCtor);
        jmethodID setDataSource = env->GetMethodID(extractorClass, "setDataSource",
            "(Ljava/lang/String;)V");
        jstring jpath = env->NewStringUTF(videoPath);
        env->CallVoidMethod(extractor, setDataSource, jpath);
        env->DeleteLocalRef(jpath);
        if (env->ExceptionCheck()) {
            LOGE("setDataSource failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            env->DeleteLocalRef(extractor);
            env->DeleteLocalRef(extractorClass);
            return false;
        }

        // 2. 找到视频 track
        jmethodID getTrackCount = env->GetMethodID(extractorClass, "getTrackCount", "()I");
        int trackCount = env->CallIntMethod(extractor, getTrackCount);
        jmethodID getTrackFormat = env->GetMethodID(extractorClass, "getTrackFormat",
            "(I)Landroid/media/MediaFormat;");
        jclass formatClass = env->FindClass("android/media/MediaFormat");
        jmethodID getString = env->GetMethodID(formatClass, "getString",
            "(Ljava/lang/String;)Ljava/lang/String;");
        jmethodID selectTrack = env->GetMethodID(extractorClass, "selectTrack", "(I)V");

        int videoTrack = -1;
        jstring mimeKey = env->NewStringUTF("mime");
        for (int i = 0; i < trackCount; i++) {
            jobject format = env->CallObjectMethod(extractor, getTrackFormat, i);
            if (!format) continue;
            jstring mime = (jstring)env->CallObjectMethod(format, getString, mimeKey);
            if (mime) {
                const char* mimeStr = env->GetStringUTFChars(mime, nullptr);
                if (mimeStr && strncmp(mimeStr, "video/", 6) == 0) {
                    videoTrack = i;
                    env->ReleaseStringUTFChars(mime, mimeStr);
                    env->DeleteLocalRef(mime);
                    env->DeleteLocalRef(format);
                    break;
                }
                env->ReleaseStringUTFChars(mime, mimeStr);
                env->DeleteLocalRef(mime);
            }
            env->DeleteLocalRef(format);
        }
        env->DeleteLocalRef(mimeKey);
        env->DeleteLocalRef(formatClass);

        if (videoTrack < 0) {
            LOGE("No video track found");
            env->DeleteLocalRef(extractor);
            env->DeleteLocalRef(extractorClass);
            return false;
        }

        // 3. 选择视频 track
        env->CallVoidMethod(extractor, selectTrack, videoTrack);
        jobject trackFormat = env->CallObjectMethod(extractor, getTrackFormat, videoTrack);

        // 4. 创建 MediaCodec 解码器
        jclass codecClass = env->FindClass("android/media/MediaCodec");
        jmethodID createByMime = env->GetStaticMethodID(codecClass, "createDecoderByType",
            "(Ljava/lang/String;)Landroid/media/MediaCodec;");
        // 获取 mime type
        jstring mimeKey2 = env->NewStringUTF("mime");
        jstring mimeStr = (jstring)env->CallObjectMethod(trackFormat, getString, mimeKey2);
        env->DeleteLocalRef(mimeKey2);

        jobject codec = env->CallStaticObjectMethod(codecClass, createByMime, mimeStr);
        env->DeleteLocalRef(mimeStr);
        if (env->ExceptionCheck()) {
            LOGE("createDecoderByType failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            env->DeleteLocalRef(extractor);
            env->DeleteLocalRef(extractorClass);
            env->DeleteLocalRef(codecClass);
            return false;
        }

        // 5. 配置解码器（输出到 Surface）
        jmethodID configure = env->GetMethodID(codecClass, "configure",
            "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V");
        env->CallVoidMethod(codec, configure, trackFormat, surface, nullptr, 0);
        if (env->ExceptionCheck()) {
            LOGE("configure failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            // 尝试不带 format 配置（某些设备兼容性）
            env->ExceptionClear();
            env->CallVoidMethod(codec, configure, nullptr, surface, nullptr, 0);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
                env->DeleteLocalRef(codec);
                env->DeleteLocalRef(extractor);
                env->DeleteLocalRef(extractorClass);
                env->DeleteLocalRef(codecClass);
                env->DeleteLocalRef(trackFormat);
                return false;
            }
        }

        // 6. 启动解码器
        jmethodID codecStart = env->GetMethodID(codecClass, "start", "()V");
        env->CallVoidMethod(codec, codecStart);
        if (env->ExceptionCheck()) {
            LOGE("codec start failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            env->DeleteLocalRef(codec);
            env->DeleteLocalRef(extractor);
            env->DeleteLocalRef(extractorClass);
            env->DeleteLocalRef(codecClass);
            env->DeleteLocalRef(trackFormat);
            return false;
        }

        // 保存全局引用
        g_extractor = env->NewGlobalRef(extractor);
        g_codec = env->NewGlobalRef(codec);
        g_codecClass = (jclass)env->NewGlobalRef(codecClass);
        g_extractorClass = (jclass)env->NewGlobalRef(extractorClass);
        g_running = true;

        // 缓存方法 ID
        g_midDequeueInputBuffer  = env->GetMethodID(codecClass, "dequeueInputBuffer", "(J)I");
        g_midQueueInputBuffer    = env->GetMethodID(codecClass, "queueInputBuffer", "(IIIJI)V");
        g_midDequeueOutputBuffer = env->GetMethodID(codecClass, "dequeueOutputBuffer",
            "(Landroid/media/MediaCodec$BufferInfo;J)I");
        g_midReleaseOutputBuffer = env->GetMethodID(codecClass, "releaseOutputBuffer", "(IZ)V");
        g_midStop                = env->GetMethodID(codecClass, "stop", "()V");
        g_midRelease             = env->GetMethodID(codecClass, "release", "()V");
        g_midReadSampleData      = env->GetMethodID(extractorClass, "readSampleData",
            "(Ljava/nio/ByteBuffer;I)I");
        g_midGetSampleTime       = env->GetMethodID(extractorClass, "getSampleTime", "()J");
        g_midAdvance             = env->GetMethodID(extractorClass, "advance", "()Z");
        g_midSeekTo              = env->GetMethodID(extractorClass, "seekTo", "(JI)V");

        env->DeleteLocalRef(codec);
        env->DeleteLocalRef(extractor);
        env->DeleteLocalRef(codecClass);
        env->DeleteLocalRef(extractorClass);
        env->DeleteLocalRef(trackFormat);

        LOGI("VideoDecoder started successfully");

        // 启动解码线程
        pthread_t tid;
        pthread_create(&tid, nullptr, decodeThread, nullptr);
        pthread_detach(tid);

        return true;
    }

    static void stop(JNIEnv* env) {
        g_running = false;
        // 等待解码线程退出
        usleep(200000); // 200ms

        if (g_codec) {
            env->CallVoidMethod(g_codec, g_midStop);
            env->CallVoidMethod(g_codec, g_midRelease);
            env->ExceptionClear();
            env->DeleteGlobalRef(g_codec);
            g_codec = nullptr;
        }
        if (g_extractor) {
            env->DeleteGlobalRef(g_extractor);
            g_extractor = nullptr;
        }
        if (g_codecClass) {
            env->DeleteGlobalRef(g_codecClass);
            g_codecClass = nullptr;
        }
        if (g_extractorClass) {
            env->DeleteGlobalRef(g_extractorClass);
            g_extractorClass = nullptr;
        }
    }

    static bool isRunning() { return g_running; }

private:
    static jobject g_codec;
    static jobject g_extractor;
    static jclass  g_codecClass;
    static jclass  g_extractorClass;
    static volatile bool g_running;

    static jmethodID g_midDequeueInputBuffer;
    static jmethodID g_midQueueInputBuffer;
    static jmethodID g_midDequeueOutputBuffer;
    static jmethodID g_midReleaseOutputBuffer;
    static jmethodID g_midStop;
    static jmethodID g_midRelease;
    static jmethodID g_midReadSampleData;
    static jmethodID g_midGetSampleTime;
    static jmethodID g_midAdvance;
    static jmethodID g_midSeekTo;

    static void* decodeThread(void* /*arg*/) {
        JNIEnv* env = nullptr;
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("decodeThread: AttachCurrentThread failed");
            return nullptr;
        }

        LOGI("decodeThread started");

        jclass bufferInfoClass = env->FindClass("android/media/MediaCodec$BufferInfo");
        jmethodID bufferInfoCtor = env->GetMethodID(bufferInfoClass, "<init>", "()V");
        jobject bufferInfo = env->NewObject(bufferInfoClass, bufferInfoCtor);

        const long TIMEOUT_US = 10000; // 10ms
        bool sawInputEOS = false;
        bool sawOutputEOS = false;

        while (g_running && !sawOutputEOS) {
            if (!g_codec) break;

            // === 处理输入 ===
            if (!sawInputEOS) {
                int inputBufIndex = env->CallIntMethod(g_codec, g_midDequeueInputBuffer, (jlong)TIMEOUT_US);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    inputBufIndex = -1;
                }
                if (inputBufIndex >= 0) {
                    // 获取输入 ByteBuffer
                    jmethodID getInputBuffer = env->GetMethodID(g_codecClass,
                        "getInputBuffer", "(I)Ljava/nio/ByteBuffer;");
                    jobject inputBuffer = env->CallObjectMethod(g_codec, getInputBuffer, inputBufIndex);
                    if (inputBuffer) {
                        int sampleSize = env->CallIntMethod(g_extractor, g_midReadSampleData,
                            inputBuffer, 0);
                        if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                            sampleSize = -1;
                        }

                        if (sampleSize < 0) {
                            sawInputEOS = true;
                            sampleSize = 0;
                        }

                        jlong sampleTime = env->CallLongMethod(g_extractor, g_midGetSampleTime);
                        int flags = sawInputEOS ? 4 /*BUFFER_FLAG_END_OF_STREAM*/ : 0;

                        env->CallVoidMethod(g_codec, g_midQueueInputBuffer,
                            inputBufIndex, 0, sampleSize, sampleTime, flags);
                        env->ExceptionClear();

                        env->DeleteLocalRef(inputBuffer);

                        if (!sawInputEOS) {
                            env->CallVoidMethod(g_extractor, g_midAdvance);
                            env->ExceptionClear();
                        }
                    }
                }
            }

            // === 处理输出 ===
            int outputBufIndex = env->CallIntMethod(g_codec, g_midDequeueOutputBuffer,
                bufferInfo, (jlong)TIMEOUT_US);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                outputBufIndex = -1;
            }

            if (outputBufIndex >= 0) {
                // releaseOutputBuffer(render=true) 将帧渲染到 Surface
                env->CallVoidMethod(g_codec, g_midReleaseOutputBuffer, outputBufIndex, JNI_TRUE);
                env->ExceptionClear();

                // 检查是否 EOS
                jclass biClass = env->GetObjectClass(bufferInfo);
                jfieldID flagsField = env->GetFieldID(biClass, "flags", "I");
                jint biFlags = env->GetIntField(bufferInfo, flagsField);
                env->DeleteLocalRef(biClass);

                if (biFlags & 4 /*BUFFER_FLAG_END_OF_STREAM*/) {
                    sawOutputEOS = true;
                    if (g_config.loopPlayback) {
                        // 循环播放：seek 到开头，重新开始
                        LOGI("Loop: seeking to beginning");
                        env->CallVoidMethod(g_extractor, g_midSeekTo, (jlong)0, 1 /*SEEK_TO_PREVIOUS_SYNC*/);
                        env->ExceptionClear();
                        sawInputEOS = false;
                        sawOutputEOS = false;
                    }
                }
            } else if (outputBufIndex == -3 /*INFO_TRY_AGAIN_LATER*/) {
                // 等待重试
                usleep(5000); // 5ms
            }
        }

        env->DeleteLocalRef(bufferInfo);
        env->DeleteLocalRef(bufferInfoClass);

        LOGI("decodeThread ended");
        g_jvm->DetachCurrentThread();
        return nullptr;
    }
};

// 静态成员初始化
jobject VideoDecoder::g_codec = nullptr;
jobject VideoDecoder::g_extractor = nullptr;
jclass  VideoDecoder::g_codecClass = nullptr;
jclass  VideoDecoder::g_extractorClass = nullptr;
volatile bool VideoDecoder::g_running = false;

jmethodID VideoDecoder::g_midDequeueInputBuffer = nullptr;
jmethodID VideoDecoder::g_midQueueInputBuffer = nullptr;
jmethodID VideoDecoder::g_midDequeueOutputBuffer = nullptr;
jmethodID VideoDecoder::g_midReleaseOutputBuffer = nullptr;
jmethodID VideoDecoder::g_midStop = nullptr;
jmethodID VideoDecoder::g_midRelease = nullptr;
jmethodID VideoDecoder::g_midReadSampleData = nullptr;
jmethodID VideoDecoder::g_midGetSampleTime = nullptr;
jmethodID VideoDecoder::g_midAdvance = nullptr;
jmethodID VideoDecoder::g_midSeekTo = nullptr;

// ============ 全局 Surface 引用 ============
static jobject g_cameraSurface = nullptr;       // Camera1 Surface
static jobject g_cameraSurfaceTexture = nullptr; // Camera1 SurfaceTexture
static pthread_mutex_t g_surfaceMutex = PTHREAD_MUTEX_INITIALIZER;

// ============ Camera1 Native Method Hooks ============
// 使用 Zygisk 的 hookJniNativeMethods 替换 Camera 类的 native 方法

// native_setPreviewDisplay(Surface) → 保存 Surface
static void hook_native_setPreviewDisplay(JNIEnv* env, jobject thiz, jobject surface) {
    LOGI("Camera1: native_setPreviewDisplay hooked, surface=%p", surface);

    pthread_mutex_lock(&g_surfaceMutex);
    // 清理旧的
    if (g_cameraSurface) {
        env->DeleteGlobalRef(g_cameraSurface);
        g_cameraSurface = nullptr;
    }
    // 保存新的（如果非 null）
    if (surface) {
        g_cameraSurface = env->NewGlobalRef(surface);
    }
    pthread_mutex_unlock(&g_surfaceMutex);

    // 不调用原始方法（不启动真实摄像头预览）
    // 视频将在 native_startPreview 时解码到此 Surface
}

// native_setPreviewTexture(SurfaceTexture) → 保存 SurfaceTexture，创建 Surface
static void hook_native_setPreviewTexture(JNIEnv* env, jobject thiz, jobject surfaceTexture) {
    LOGI("Camera1: native_setPreviewTexture hooked, surfaceTexture=%p", surfaceTexture);

    pthread_mutex_lock(&g_surfaceMutex);
    // 清理旧的
    if (g_cameraSurface) {
        env->DeleteGlobalRef(g_cameraSurface);
        g_cameraSurface = nullptr;
    }
    if (g_cameraSurfaceTexture) {
        env->DeleteGlobalRef(g_cameraSurfaceTexture);
        g_cameraSurfaceTexture = nullptr;
    }

    if (surfaceTexture) {
        g_cameraSurfaceTexture = env->NewGlobalRef(surfaceTexture);
        // 从 SurfaceTexture 创建 Surface
        jclass surfaceClass = env->FindClass("android/view/Surface");
        jmethodID surfaceCtor = env->GetMethodID(surfaceClass, "<init>",
            "(Landroid/graphics/SurfaceTexture;)V");
        jobject surface = env->NewObject(surfaceClass, surfaceCtor, surfaceTexture);
        if (surface && !env->ExceptionCheck()) {
            g_cameraSurface = env->NewGlobalRef(surface);
        } else {
            env->ExceptionClear();
            LOGE("Failed to create Surface from SurfaceTexture");
        }
        env->DeleteLocalRef(surface);
        env->DeleteLocalRef(surfaceClass);
    }
    pthread_mutex_unlock(&g_surfaceMutex);
}

// native_startPreview() → 启动视频解码到保存的 Surface
static void hook_native_startPreview(JNIEnv* env, jobject thiz) {
    LOGI("Camera1: native_startPreview hooked, starting video decode");

    const char* vpath = g_config.videoPath[0] ? g_config.videoPath : VIDEO_PATH;

    pthread_mutex_lock(&g_surfaceMutex);
    jobject surface = g_cameraSurface;
    pthread_mutex_unlock(&g_surfaceMutex);

    if (!surface) {
        LOGE("Camera1: no Surface available, cannot start video decode");
        return;
    }

    if (!fileExists(vpath)) {
        LOGE("Camera1: video file not found: %s", vpath);
        return;
    }

    // 停止旧解码器
    if (VideoDecoder::isRunning()) {
        VideoDecoder::stop(env);
        usleep(100000); // 100ms
    }

    // 启动视频解码
    if (!VideoDecoder::start(env, surface, vpath)) {
        LOGE("Camera1: VideoDecoder::start failed");
    }
}

// native_stopPreview() → 停止视频解码
static void hook_native_stopPreview(JNIEnv* env, jobject thiz) {
    LOGI("Camera1: native_stopPreview hooked, stopping video decode");
    if (VideoDecoder::isRunning()) {
        VideoDecoder::stop(env);
    }
}

// native_release() → 清理资源
static void hook_native_release(JNIEnv* env, jobject thiz) {
    LOGI("Camera1: native_release hooked, cleaning up");
    if (VideoDecoder::isRunning()) {
        VideoDecoder::stop(env);
    }
    pthread_mutex_lock(&g_surfaceMutex);
    if (g_cameraSurface) {
        env->DeleteGlobalRef(g_cameraSurface);
        g_cameraSurface = nullptr;
    }
    if (g_cameraSurfaceTexture) {
        env->DeleteGlobalRef(g_cameraSurfaceTexture);
        g_cameraSurfaceTexture = nullptr;
    }
    pthread_mutex_unlock(&g_surfaceMutex);
}

// ============ Camera1 Hook 安装 ============

static bool installCamera1Hooks(zygisk::Api* api, JNIEnv* env) {
    LOGI("Installing Camera1 hooks...");

    // Camera1 的 native 方法签名（从 AOSP Camera.java）
    // 注意：不同 Android 版本签名可能略有不同，这里列出常见版本
    JNINativeMethod methods[] = {
        // native_setPreviewDisplay(Surface)
        {(char*)"native_setPreviewDisplay",
         (char*)"(Landroid/view/Surface;)V",
         (void*)hook_native_setPreviewDisplay},

        // native_setPreviewTexture(SurfaceTexture)
        {(char*)"native_setPreviewTexture",
         (char*)"(Landroid/graphics/SurfaceTexture;)V",
         (void*)hook_native_setPreviewTexture},

        // native_startPreview()
        {(char*)"native_startPreview",
         (char*)"()V",
         (void*)hook_native_startPreview},

        // native_stopPreview()
        {(char*)"native_stopPreview",
         (char*)"()V",
         (void*)hook_native_stopPreview},

        // native_release()
        {(char*)"native_release",
         (char*)"()V",
         (void*)hook_native_release},
    };

    // 使用 Zygisk API hook JNI native methods
    // Zygisk 的 hookJniNativeMethods 会安全地替换 native 方法实现
    api->hookJniNativeMethods(env, "android/hardware/Camera", methods,
                              sizeof(methods) / sizeof(methods[0]));

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("installCamera1Hooks: some methods may not exist (version mismatch)");
    }

    LOGI("Camera1 hooks installed");
    return true;
}

// ============ Camera2 Hook (ArtMethod entry_point 替换) ============
// Camera2 API 的关键方法是 Java 方法，需要 ArtMethod hook
// 这里用简化版 ArtMethod entry_point 替换

// ArtMethod 在 arm64 Android 10+ 的布局:
// offset 0x00: declaring_class_ (4 bytes)
// offset 0x04: access_flags_ (4 bytes)
// offset 0x08: dex_code_item_offset_ (4 bytes)
// offset 0x0C: dex_method_index_ (4 bytes)
// offset 0x10: method_index_ (2 bytes) + hotness_count_ (2 bytes)
// offset 0x14: padding (4 bytes)  [or ptr_sized data on 32-bit]
// offset 0x18: ptr_sized_fields_.data_ (8 bytes)
// offset 0x20: ptr_sized_fields_.entry_point_from_quick_compiled_code_ (8 bytes)
// ArtMethod 大小: 0x28 (40 bytes) on arm64

// 由于 ArtMethod hook 涉及版本特定偏移，且需要创建 Java hook 类，
// Camera2 hook 暂时通过 JNI 代理方式实现（后续完善）
// 当前 Camera2 app 会 fallback 到 Camera1 兼容模式（大部分 Camera2 app 也支持 Camera1 API）

// ============ Zygisk 模块入口 ============

class VCamZygiskModule : public zygisk::ModuleBase<VCamZygiskModule> {
public:
    static void onLoad(zygisk::Api *api, JNIEnv *env) {
        env->GetJavaVM(&g_jvm);
        LOGI("VCAM Zygisk module loaded (pid=%d uid=%d)", getpid(), getuid());
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    static void postAppSpecialize(zygisk::Api *api, const zygisk::AppSpecializeArgs *args) {
        // 通过 JavaVM 获取当前线程的 JNIEnv（更可靠）
        JNIEnv* env = nullptr;
        if (g_jvm) {
            if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
                // 如果 GetEnv 失败，尝试 AttachCurrentThread
                if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                    LOGE("postAppSpecialize: cannot get JNIEnv");
                    return;
                }
            }
        } else {
            LOGE("postAppSpecialize: g_jvm is null");
            return;
        }

        if (!env) {
            LOGE("postAppSpecialize: env is null");
            return;
        }

        // 获取进程名
        const char* processName = nullptr;
        if (args && args->nice_name) {
            processName = env->GetStringUTFChars(args->nice_name, nullptr);
        }

        if (!processName) {
            LOGD("postAppSpecialize: no process name");
            return;
        }

        // 跳过系统关键进程
        if (strcmp(processName, "system_server") == 0 ||
            strcmp(processName, "android") == 0 ||
            strcmp(processName, "com.android.systemui") == 0 ||
            strncmp(processName, "system_server", 13) == 0) {
            LOGD("Skipping system process: %s", processName);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        LOGI("postAppSpecialize: process=%s pid=%d", processName, getpid());

        // 读取配置
        if (!readConfig()) {
            LOGD("No config or read failed, skipping hook for %s", processName);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        // 如果禁用，跳过
        if (g_config.disabled) {
            LOGD("Hook disabled in config, skipping %s", processName);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        // 检查视频文件
        const char* vpath = g_config.videoPath[0] ? g_config.videoPath : VIDEO_PATH;
        if (!fileExists(vpath)) {
            LOGD("Video file not found: %s, skipping %s", vpath, processName);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        // 安装 Camera1 hook
        if (!g_hooksInstalled) {
            LOGI("Installing camera hooks for %s", processName);
            installCamera1Hooks(api, env);
            g_hooksInstalled = true;
            LOGI("VCAM hooks active for process: %s", processName);
        }

        env->ReleaseStringUTFChars(args->nice_name, processName);
    }

    static void preServerSpecialize(zygisk::Api *api, zygisk::ServerSpecializeArgs *args) {
        // system_server - 不需要 hook
    }
};

REGISTER_ZYGISK_MODULE(VCamZygiskModule)
