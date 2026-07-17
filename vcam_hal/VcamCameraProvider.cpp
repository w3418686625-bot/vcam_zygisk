/**
 * VcamCameraProvider.cpp - Camera metadata 生成和 Provider 管理
 *
 * 生成合法的 camera_characteristics metadata，
 * 包含摄像头基本信息、支持的分辨率、帧率、硬件级别等。
 *
 * 参考 Android CameraCharacteristics API:
 *   - ANDROID_LENS_FACING: 前/后置
 *   - ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS: 支持的输出格式
 *   - ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE: 传感器尺寸
 *   - ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL: 硬件支持级别
 */

#include "VcamCameraProvider.h"
#include <android/log.h>
#include <cstring>
#include <cstdlib>

#define LOG_TAG "VCAM-HAL-Provider"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace vcam {

// ---- 简易 CameraMetadata (system/camera_metadata.h 的子集) ----
// 在 NDK 中无法直接使用 camera_metadata_t，我们手动构造二进制 metadata

#define CAMERA_METADATA_TYPE_BYTE    0
#define CAMERA_METADATA_TYPE_INT32   1
#define CAMERA_METADATA_TYPE_FLOAT   2
#define CAMERA_METADATA_TYPE_INT64   3
#define CAMERA_METADATA_TYPE_DOUBLE  4
#define CAMERA_METADATA_TYPE_RATIONAL 5

// Camera metadata tags (部分)
#define ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES 0x0001
#define ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES      0x0010
#define ANDROID_CONTROL_AVAILABLE_SCENE_MODES               0x0020
#define ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES 0x0025
#define ANDROID_CONTROL_MAX_REGIONS                         0x0030
#define ANDROID_FLASH_INFO_AVAILABLE                       0x0400
#define ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL              0x0500
#define ANDROID_LENS_FACING                                0x0600
#define ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS          0x0610
#define ANDROID_LENS_INFO_AVAILABLE_APERTURES              0x0620
#define ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES       0x0630
#define ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION  0x0640
#define ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION       0x0650
#define ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE              0x0660
#define ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE           0x0670
#define ANDROID_REQUEST_AVAILABLE_CAPABILITIES             0x0800
#define ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS             0x0810
#define ANDROID_REQUEST_PIPELINE_MAX_DEPTH                 0x0820
#define ANDROID_REQUEST_PARTIAL_RESULT_COUNT               0x0830
#define ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM          0x0900
#define ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS     0x0910
#define ANDROID_SCALER_CROPPING_TYPE                       0x0920
#define ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE              0x0A00
#define ANDROID_SENSOR_INFO_PHYSICAL_SIZE                  0x0A10
#define ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE               0x0A20
#define ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE               0x0A30
#define ANDROID_SENSOR_INFO_WHITE_LEVEL                    0x0A40
#define ANDROID_SENSOR_ORIENTATION                         0x0B00
#define ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES 0x0C00
#define ANDROID_STATISTICS_INFO_MAX_FACE_COUNT             0x0C10
#define ANDROID_SYNC_MAX_LATENCY                           0x0E00
#define ANDROID_LENS_INFO_MIN_FOCUS_DISTANCE               0x0670

// Hardware levels
#define ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED      0
#define ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL         1
#define ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY       2
#define ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_3            3
#define ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL     4

// ---- 元数据项写入辅助 ----

struct MetaEntry {
    uint32_t tag;
    uint8_t  type;
    uint8_t  reserved[3];
    uint32_t count;
    union {
        uint64_t offset;
        uint8_t  value[4];
    } data;
};

// 简化写入: 直接构造内存块
class MetaBuilder {
public:
    MetaBuilder() { m_buf = (uint8_t*)malloc(16384); m_capacity = 16384; }

    ~MetaBuilder() { free(m_buf); }

    void addU8(uint32_t tag, uint8_t val) {
        writeEntry(tag, CAMERA_METADATA_TYPE_BYTE, 1, &val, 1);
    }

    void addI32(uint32_t tag, int32_t val) {
        writeEntry(tag, CAMERA_METADATA_TYPE_INT32, 1, (uint8_t*)&val, 4);
    }

    void addI32Array(uint32_t tag, const int32_t* vals, int count) {
        writeEntry(tag, CAMERA_METADATA_TYPE_INT32, count, (const uint8_t*)vals, count * 4);
    }

    void addI64(uint32_t tag, int64_t val) {
        writeEntry(tag, CAMERA_METADATA_TYPE_INT64, 1, (uint8_t*)&val, 8);
    }

    void addI64Array(uint32_t tag, const int64_t* vals, int count) {
        writeEntry(tag, CAMERA_METADATA_TYPE_INT64, count, (const uint8_t*)vals, count * 8);
    }

    void addFloat(uint32_t tag, float val) {
        writeEntry(tag, CAMERA_METADATA_TYPE_FLOAT, 1, (uint8_t*)&val, 4);
    }

    void addRational(uint32_t tag, int32_t num, int32_t den) {
        int32_t rat[2] = {num, den};
        writeEntry(tag, CAMERA_METADATA_TYPE_RATIONAL, 1, (uint8_t*)rat, 8);
    }

    uint8_t* build(size_t* outSize) {
        *outSize = m_offset;
        return m_buf;
    }

private:
    void writeEntry(uint32_t tag, uint8_t type, uint32_t count,
                    const uint8_t* data, uint32_t dataSize) {
        if (m_offset + sizeof(MetaEntry) + dataSize > m_capacity) return;

        // 写入 tag
        memcpy(m_buf + m_offset, &tag, 4); m_offset += 4;
        // 写入 type
        m_buf[m_offset++] = type;
        m_buf[m_offset++] = 0; m_buf[m_offset++] = 0; m_buf[m_offset++] = 0;
        // 写入 count
        memcpy(m_buf + m_offset, &count, 4); m_offset += 4;
        // 写入 data (inline)
        memcpy(m_buf + m_offset, data, dataSize); m_offset += dataSize;
        // 对齐到 4 字节
        while (m_offset % 4 != 0) m_offset++;
    }

    uint8_t* m_buf;
    size_t   m_offset = 0;
    size_t   m_capacity;
};

// ============================================================

VcamCameraProvider& VcamCameraProvider::instance() {
    static VcamCameraProvider inst;
    return inst;
}

VcamCameraProvider::VcamCameraProvider() {}

bool VcamCameraProvider::initialize() {
    LOGI("VcamCameraProvider initializing...");
    m_enabled = true;
    return true;
}

void VcamCameraProvider::shutdown() {
    LOGI("VcamCameraProvider shutting down");
    m_enabled = false;
    m_registered = false;
}

std::vector<std::string> VcamCameraProvider::getCameraIdList() const {
    return {VCAM_BACK_ID, VCAM_FRONT_ID};
}

const uint8_t* VcamCameraProvider::getCameraCharacteristics(
    const std::string& cameraId, size_t* outSize) const {

    int facing = (cameraId == VCAM_FRONT_ID) ? 0 : 1;  // 0=front, 1=back
    uint8_t* metadata = generateCharacteristics(cameraId, facing, 1920, 1080, outSize);
    return metadata;
}

uint8_t* VcamCameraProvider::generateCharacteristics(
    const std::string& cameraId, int facing, int width, int height,
    size_t* outSize) const {

    MetaBuilder mb;

    // --- 基本信息 ---
    mb.addU8(ANDROID_LENS_FACING, (uint8_t)facing);  // 0=FRONT, 1=BACK

    int32_t hwLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
    mb.addU8(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, (uint8_t)hwLevel);

    int32_t capabilities[] = {0, 1, 2, 3, 6}; // BACKWARD_COMPATIBLE, MANUAL_SENSOR, MANUAL_POST_PROCESSING, RAW, BURST_CAPTURE
    mb.addI32Array(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, capabilities, 5);

    // --- 传感器信息 ---
    int32_t activeArray[] = {0, 0, width, height};
    mb.addI32Array(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray, 4);

    int32_t pixelArray[] = {width, height};
    mb.addI32Array(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArray, 2);

    float physSize[] = {4.8f, 3.6f}; // 典型 1/3" 传感器
    mb.addFloat(ANDROID_SENSOR_INFO_PHYSICAL_SIZE, 0); // 简化

    int32_t timestampSource = 0; // UNKNOWN
    mb.addU8(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, (uint8_t)timestampSource);

    int32_t whiteLevel = 255;
    mb.addI32(ANDROID_SENSOR_INFO_WHITE_LEVEL, whiteLevel);

    int32_t orientation = (facing == 0) ? 270 : 90;
    mb.addI32(ANDROID_SENSOR_ORIENTATION, orientation);

    // --- 镜头信息 ---
    float focalLength = 3.5f;
    mb.addFloat(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, focalLength);

    float aperture = 2.0f;
    mb.addFloat(ANDROID_LENS_INFO_AVAILABLE_APERTURES, aperture);

    float filterDensity = 0.0f;
    mb.addFloat(ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES, filterDensity);

    uint8_t oisMode = 0; // 不支持光学防抖
    mb.addU8(ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION, oisMode);

    float hyperfocal = 0.0f;
    mb.addFloat(ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, hyperfocal);

    // --- 缩放 ---
    float maxZoom = 4.0f;
    mb.addFloat(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, maxZoom);

    int32_t cropType = 0; // CENTER_ONLY
    mb.addU8(ANDROID_SCALER_CROPPING_TYPE, (uint8_t)cropType);

    // --- 流配置: 支持的格式和分辨率 ---
    // 格式: [format, width, height, input/output] x N
    // HAL_PIXEL_FORMAT_YCbCr_420_888 = 0x23 = 35
    // HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED = 0x22 = 34
    // NV21 = 0x11 = 17
    // YV12 = 0x32315659

    int32_t streamConfigs[] = {
        // YUV_420_888 (preview/video)
        35, width, height, 0,  // OUTPUT
        35, width, height, 1,  // INPUT (reprocess)

        // IMPLEMENTATION_DEFINED
        34, width, height, 0,
        34, width, height, 1,

        // NV21 (legacy)
        17, width, height, 0,
        17, width, height, 1,

        // JPEG (still capture)
        0x100, width, height, 0,
        0x100, width, height, 1,

        // Lower resolutions
        35, 1280, 720, 0,
        35, 640,  480, 0,
        35, 352,  288, 0,
        35, 320,  240, 0,

        34, 1280, 720, 0,
        34, 640,  480, 0,

        // RAW_SENSOR (10-bit)
        0x20, width, height, 0,
    };

    mb.addI32Array(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                   streamConfigs, sizeof(streamConfigs) / sizeof(int32_t));

    // --- 帧率范围 [15, 30] ---
    int32_t fpsRanges[] = {15, 30, 15, 15, 30, 30};
    mb.addI32Array(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fpsRanges, 6);

    // --- 场景模式 ---
    uint8_t sceneMode = 0; // DISABLED
    mb.addU8(ANDROID_CONTROL_AVAILABLE_SCENE_MODES, sceneMode);

    // --- 人脸检测 ---
    uint8_t faceModes = 0; // OFF
    mb.addU8(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, faceModes);

    int32_t maxFaces = 0;
    mb.addI32(ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, maxFaces);

    // --- 同步 ---
    int32_t syncLatency = -1; // UNKNOWN
    mb.addI32(ANDROID_SYNC_MAX_LATENCY, syncLatency);

    // --- Pipeline ---
    int32_t maxOutputs[] = {3, 3, 3}; // raw, processed, stall
    mb.addI32Array(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, maxOutputs, 3);

    int32_t pipelineDepth = 4;
    mb.addU8(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, (uint8_t)pipelineDepth);

    int32_t partialCount = 1;
    mb.addI32(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, partialCount);

    // --- AE region ---
    int32_t maxRegions[] = {1, 0, 0};
    mb.addI32Array(ANDROID_CONTROL_MAX_REGIONS, maxRegions, 3);

    // --- 色差校正 ---
    uint8_t aberrationModes[] = {0, 1, 2}; // OFF, FAST, HIGH_QUALITY
    mb.addI32Array(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
                   (int32_t*)nullptr, 0); // 需要处理...

    return mb.build(outSize);
}

void* VcamCameraProvider::openDevice(const std::string& cameraId) {
    LOGI("openDevice: %s", cameraId.c_str());
    // 由 VcamCameraDevice 管理
    return nullptr; // placeholder - 实际设备在 HIDL 层创建
}

void VcamCameraProvider::closeDevice(void* device) {
    LOGI("closeDevice");
}

} // namespace vcam
