/*
 * Zygisk API header (compatible with Magisk Zygisk and KernelSU Zygisk Next)
 * Based on Magisk's zygisk API v4
 *
 * 参考实现: https://github.com/topjohnwu/Magisk/tree/master/native/src/zygisk
 */

#pragma once

#include <jni.h>
#include <cstdint>

#define ZYGISK_API_VERSION 4

namespace zygisk {

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

enum class Option : int {
    DLCLOSE_MODULE_LIBRARY = 0,
    FORCE_DENYLIST_UNMOUNT = 1,
    DISABLE_APP_SPECIALIZE = 2,
    DISABLE_SERVER_SPECIALIZE = 3,
};

enum StateFlag : uint32_t {
    PROCESS_GRANTED_ROOT = (1u << 0),
    PROCESS_ON_DENYLIST = (1u << 1),
};

namespace internal {

    struct api_table {
        // Internal Zygisk functions
        void *impl;
        void (*registerModule)(api_table *, void *);
        void (*hookJniNativeMethods)(JNIEnv *, const char *, JNINativeMethod *, int);
        void (*pltHookRegister)(const char *, const char *, void *, void **);
        bool (*exemptFd)(int);
        bool (*pltHookCommit)();
        int (*connectCompanion)(void *);
        void (*setOption)(void *, Option);
        uint32_t (*getFlags)(void *);
        int (*getModuleDir)(void *);
    };

    struct module_abi {
        long api_version;
        void (*preAppSpecialize)(Api *, AppSpecializeArgs *);
        void (*postAppSpecialize)(Api *, const AppSpecializeArgs *);
        void (*preServerSpecialize)(Api *, ServerSpecializeArgs *);
        void (*postServerSpecialize)(Api *, const ServerSpecializeArgs *);
    };
}

struct AppSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jint &mount_external;
    jstring &nice_name;
    jstring &app_dir;
    const void *const app_data_info_dir;
    const void *const se_info;
    jint &target_sdk_version;
    jobjectArray &pkg_data_info_list;
    jobjectArray &whitelisted_data_info_list;
    jboolean &mount_data_dirs;
    jboolean &mount_storage_dirs;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;
};

struct Api {
    // These are set by Zygisk framework before calling callbacks
    internal::api_table *table;
    JNIEnv *env;

    void setOption(Option opt) {
        table->setOption(table->impl, opt);
    }

    uint32_t getFlags() {
        return table->getFlags(table->impl);
    }

    int connectCompanion(void *ptr) {
        return table->connectCompanion(ptr);
    }

    int getModuleDir() {
        return table->getModuleDir(table->impl);
    }

    void pltHookRegister(const char *regex, const char *symbol, void *fn, void **backup) {
        table->pltHookRegister(regex, symbol, fn, backup);
    }

    bool pltHookCommit() {
        return table->pltHookCommit();
    }

    void hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int count) {
        table->hookJniNativeMethods(env, className, methods, count);
    }

    bool exemptFd(int fd) {
        return table->exemptFd(fd);
    }
};

template <class T>
struct ModuleBase {
    static void onLoad(Api *, JNIEnv *) {}

    static void preAppSpecialize(Api *, AppSpecializeArgs *) {}
    static void postAppSpecialize(Api *, const AppSpecializeArgs *) {}

    static void preServerSpecialize(Api *, ServerSpecializeArgs *) {}
    static void postServerSpecialize(Api *, const ServerSpecializeArgs *) {}
};

}  // namespace zygisk

#define REGISTER_ZYGISK_MODULE(clazz) \
extern "C" [[gnu::visibility("default")]] \
void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) { \
    static zygisk::internal::module_abi abi { \
        .api_version = ZYGISK_API_VERSION, \
        .preAppSpecialize = [](zygisk::Api *api, zygisk::AppSpecializeArgs *args) -> void { \
            clazz::preAppSpecialize(api, args); \
        }, \
        .postAppSpecialize = [](zygisk::Api *api, const zygisk::AppSpecializeArgs *args) -> void { \
            clazz::postAppSpecialize(api, args); \
        }, \
        .preServerSpecialize = [](zygisk::Api *api, zygisk::ServerSpecializeArgs *args) -> void { \
            clazz::preServerSpecialize(api, args); \
        }, \
        .postServerSpecialize = [](zygisk::Api *api, const zygisk::ServerSpecializeArgs *args) -> void { \
            clazz::postServerSpecialize(api, args); \
        }, \
    }; \
    zygisk::Api api; \
    api.table = table; \
    api.env = env; \
    clazz::onLoad(&api, env); \
    table->registerModule(table, &abi); \
}

#define REGISTER_ZYGISK_COMPANION(func) \
extern "C" [[gnu::visibility("default")]] \
void zygisk_companion_entry(int fd) { func(fd); }
