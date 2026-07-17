package com.example.vcam;

import android.util.Log;
import java.io.File;

/**
 * 全局摄像头替换管理器（Zygisk 模式）。
 *
 * 职责：部署视频文件和配置到 /data/local/tmp/，供 Zygisk 模块读取。
 * Zygisk 模块自动注入所有 App 进程，在 Native 层 Hook Camera API。
 * 不依赖 LSPosed。
 *
 * 工作流：
 *   1. 用户安装 vcam_zygisk.zip (KernelSU/Magisk 一键导入)
 *   2. 重启手机 → Zygisk 自动加载模块
 *   3. 打开 VCAM App → 选视频 → 点"激活" → 部署文件
 *   4. 点"替换" → 写入 disabled=false → 所有 App 自动生效
 */
public class SystemHookManager {
    private static final String TAG = "VCAM-SystemHook";
    private static final String HOOK_DIR = "/data/local/tmp/vcam";

    // KernelSU/Magisk 模块路径
    private static final String KSU_MODULE_DIR = "/data/adb/modules/vcam_zygisk";
    private static final String MAGISK_MODULE_DIR = "/data/adb/modules/vcam_zygisk";

    /**
     * 检测 Zygisk 模块是否已安装
     */
    public static boolean isZygiskModuleInstalled() {
        // 检查 KernelSU 模块目录
        String result = execAndRead("test -d " + KSU_MODULE_DIR + " && echo OK || echo FAIL");
        if ("OK".equals(result)) return true;
        // 检查 Magisk 模块目录
        result = execAndRead("test -d " + MAGISK_MODULE_DIR + " && echo OK || echo FAIL");
        return "OK".equals(result);
    }

    /**
     * 检测 Zygisk 模块是否已加载（检查 .so 文件是否存在）
     */
    public static boolean isZygiskModuleLoaded() {
        String result = execAndRead(
            "test -f " + KSU_MODULE_DIR + "/zygisk/arm64-v8a.so && echo OK || echo FAIL");
        return "OK".equals(result);
    }

    /**
     * 激活：部署视频文件 + 写入配置（启用替换）
     * @return null=成功, 非null=失败原因/状态信息
     */
    public static String activateWithResult() {
        StringBuilder log = new StringBuilder();

        // 1. 创建目录
        log.append("1. 创建目录...");
        if (!VCamConfig.execRootCmd("mkdir -p " + HOOK_DIR + " && chmod 755 " + HOOK_DIR)) {
            return log + "\n失败: 无法创建 " + HOOK_DIR;
        }
        log.append("OK\n");

        // 2. 部署视频
        log.append("2. 部署视频文件...");
        boolean deployed = VCamConfig.deployToHookPath();
        long size = new File(VCamConfig.HOOK_VIDEO_PATH).length();
        log.append(deployed && size > 0 ? "OK (" + size + " bytes)\n" : "失败\n");

        // 3. 写入配置（disabled=false 启用替换）
        log.append("3. 写入配置文件...");
        boolean configOk = writeHookConfigViaRoot(false);
        log.append(configOk ? "OK (替换已启用)\n" : "失败\n");

        // 4. 权限
        log.append("4. 设置权限...");
        VCamConfig.execRootCmd("chmod 644 " + VCamConfig.HOOK_VIDEO_PATH);
        VCamConfig.execRootCmd("chmod 644 " + VCamConfig.HOOK_CONFIG_FILE);
        VCamConfig.execRootCmd("chmod 755 " + HOOK_DIR);
        log.append("OK\n");

        // 5. 检测 Zygisk 模块状态
        log.append("5. 检测 Zygisk 模块...");
        boolean installed = isZygiskModuleInstalled();
        boolean loaded = isZygiskModuleLoaded();
        if (installed && loaded) {
            log.append("已安装且已加载\n");
        } else if (installed) {
            log.append("已安装(需重启加载 .so)\n");
        } else {
            log.append("未安装\n");
        }

        log.append("\n✓ 配置部署完成\n");
        log.append("---\n");
        if (installed && loaded) {
            log.append("摄像头替换已激活！\n");
            log.append("打开任意相机 App 即可看到替换画面。\n");
            log.append("Zygisk 自动注入所有进程，无需 LSPosed。\n");
        } else if (installed) {
            log.append("请重启手机以加载 Zygisk 模块。\n");
            log.append("重启后再次点击 [替换] 即可生效。\n");
        } else {
            log.append("⚠ 未检测到 Zygisk 模块\n");
            log.append("请安装 vcam_zygisk.zip (KernelSU/Magisk 模块)\n");
            log.append("获取方式: GitHub Actions 编译后下载\n");
        }

        Log.i(TAG, "Activate result:\n" + log.toString());
        return null;
    }

    /**
     * 写入配置文件（通过 root 复制到 /data/local/tmp/）
     * @param disabled true=禁用替换, false=启用替换
     */
    private static boolean writeHookConfigViaRoot(boolean disabled) {
        try {
            // 读取 app 本地配置
            java.io.BufferedReader reader = new java.io.BufferedReader(
                new java.io.FileReader("/data/data/com.example.vcam/files/vcam_settings.json"));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) sb.append(line);
            reader.close();

            org.json.JSONObject obj = new org.json.JSONObject(sb.toString());
            obj.put("video_path", VCamConfig.HOOK_VIDEO_PATH);
            obj.put("disabled", disabled);  // 关键：控制替换开关

            // 写到 app cache 目录（app 有权限写）
            String tmpFile = "/data/data/com.example.vcam/cache/vcam_config_tmp.json";
            java.io.FileWriter fw = new java.io.FileWriter(tmpFile);
            fw.write(obj.toString());
            fw.flush();
            fw.close();

            // 用 root 复制到目标位置
            boolean ok = VCamConfig.execRootCmd(
                "cp " + tmpFile + " " + VCamConfig.HOOK_CONFIG_FILE +
                " && chmod 644 " + VCamConfig.HOOK_CONFIG_FILE +
                " && rm -f " + tmpFile);

            Log.i(TAG, "writeHookConfigViaRoot: disabled=" + disabled + " ok=" + ok);
            return ok;
        } catch (Exception e) {
            Log.e(TAG, "writeHookConfigViaRoot failed", e);
            return false;
        }
    }

    /**
     * 还原摄像头（禁用替换）
     */
    public static boolean deactivate() {
        Log.i(TAG, "Deactivating (disabling replacement)...");
        // 写入 disabled=true 禁用替换
        boolean ok = writeHookConfigViaRoot(true);
        Log.i(TAG, "Deactivated: " + ok);
        return ok;
    }

    /**
     * 检查替换是否激活
     */
    public static boolean isActive() {
        // 检查配置文件中 disabled 字段
        try {
            String result = execAndRead(
                "cat " + VCamConfig.HOOK_CONFIG_FILE + " 2>/dev/null | grep -o '\"disabled\":[a-z]*'");
            if (result != null && result.contains("false")) {
                return true;
            }
        } catch (Exception e) {
            // ignore
        }
        return false;
    }

    private static String execAndRead(String cmd) {
        return execAndRead("su", "-c", cmd);
    }

    private static String execAndRead(String... cmd) {
        try {
            if (cmd.length > 0 && "su".equals(cmd[0])) {
                String suPath = VCamConfig.findSu();
                if (suPath == null) return null;
                if ("ksud".equals(suPath)) {
                    cmd = new String[]{"/data/adb/ksud", "exec", cmd[2]};
                } else if ("sh".equals(suPath) || "self".equals(suPath)) {
                    cmd = new String[]{"sh", "-c", cmd[2]};
                } else {
                    cmd[0] = suPath;
                }
            }
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.redirectErrorStream(true);
            Process p = pb.start();
            java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(p.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append("\n");
            r.close();
            p.waitFor();
            return sb.toString().trim();
        } catch (Exception e) {
            return null;
        }
    }
}
