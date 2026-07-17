cmake_minimum_required(VERSION 3.18)
project(vcam_hal C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 查找系统库 (device paths for cross-compilation) ---
# 这些 .so 在 Android 系统分区中，编译时需要导入
set(SYSLIB_DIR ${CMAKE_SOURCE_DIR}/../syslibs)

# HIDL transport
find_library(libhidlbase hidlbase)
find_library(libhidltransport hidltransport)
find_library(libutils utils)
find_library(libcutils cutils)
find_library(liblog log)
find_library(libbinder libbinder)
find_library(libui ui)
find_library(libgui gui)
find_library(libmediandk mediandk)
find_library(libnativewindow nativewindow)
find_library(libsync sync)

# HIDL Camera 接口库（需要从 AOSP 编译或从设备提取）
set(HIDL_CAM_PROVIDER "android.hardware.camera.provider@2.4")
set(HIDL_CAM_DEVICE   "android.hardware.camera.device@3.2")
set(HIDL_CAM_COMMON   "android.hardware.camera.common@1.0")

# --- 源文件 ---
set(SOURCES
    main.cpp
    VcamCameraProvider.cpp
    VcamCameraDevice.cpp
    VcamCameraSession.cpp
    FrameProvider.cpp
    VcamConfig.cpp
)

# --- 编译目标 ---
add_library(vcam_camera_provider SHARED ${SOURCES})

target_include_directories(vcam_camera_provider PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../syslibs/include
)

target_link_libraries(vcam_camera_provider
    ${libhidlbase}
    ${libhidltransport}
    ${libutils}
    ${libcutils}
    ${liblog}
    ${libbinder}
    ${libui}
    ${libgui}
    ${libmediandk}
    ${libnativewindow}
    ${libsync}
    ${HIDL_CAM_PROVIDER}
    ${HIDL_CAM_DEVICE}
    ${HIDL_CAM_COMMON}
    android
)

# --- 可执行 daemon ---
add_executable(vcam_daemon vcam_daemon_main.cpp)

target_include_directories(vcam_daemon PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../syslibs/include
)

target_link_libraries(vcam_daemon
    vcam_camera_provider
    ${libhidlbase}
    ${libhidltransport}
    ${liblog}
    ${libbinder}
    ${libutils}
    ${libcutils}
    android
)
