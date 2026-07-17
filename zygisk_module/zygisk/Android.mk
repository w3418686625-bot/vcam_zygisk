LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := vcam_zygisk
LOCAL_SRC_FILES := main.cpp
LOCAL_CFLAGS := -std=c++17 -Wall -O2 -fvisibility=hidden -fvisibility-inlines-hidden -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections
LOCAL_CPPFLAGS := -std=c++17 -fno-exceptions -fno-rtti -fvisibility=hidden
LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--hash-style=sysv -Wl,--exclude-libs,ALL
LOCAL_LDLIBS := -llog -landroid -lnativewindow
include $(BUILD_SHARED_LIBRARY)
