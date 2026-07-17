APP_ABI := arm64-v8a armeabi-v7a
APP_PLATFORM := android-31
APP_STL := c++_static
APP_OPTIM := release
APP_CPPFLAGS := -std=c++17 -fno-exceptions -fno-rtti -fvisibility=hidden
APP_LDFLAGS := -Wl,--hash-style=sysv -Wl,--gc-sections -Wl,--exclude-libs,ALL
