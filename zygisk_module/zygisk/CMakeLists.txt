cmake_minimum_required(VERSION 3.10)
project(vcam_zygisk CXX)

# Zygisk 模块 - 以 shared library 形式编译
# 输出: libvcam_zygisk.so → 重命名为 arm64-v8a.so / armeabi-v7a.so

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
add_compile_options(
    -O2
    -Wall
    -Wextra
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-exceptions
    -fno-rtti
    -ffunction-sections
    -fdata-sections
)

# 链接选项
add_link_options(
    -Wl,--gc-sections
    -Wl,--hash-style=sysv
    -Wl,--exclude-libs,ALL
    -Wl,-soname,libvcam_zygisk.so
)

# 编译目标
add_library(vcam_zygisk SHARED
    main.cpp
)

# 包含目录
target_include_directories(vcam_zygisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接库
target_link_libraries(vcam_zygisk
    log
    android
    nativewindow
)

# 设置输出名称
set_target_properties(vcam_zygisk PROPERTIES
    OUTPUT_NAME "vcam_zygisk"
    PREFIX "lib"
)
