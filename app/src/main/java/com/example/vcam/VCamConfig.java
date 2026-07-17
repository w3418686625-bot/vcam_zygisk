package com.example.vcam;

import android.content.Context;
import android.os.Environment;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;

/**
 * 在 App UI 和 Xposed Hook 之间共享配置。
 * 由于 Hook 运行在其它应用进程，不能访问本应用的 SharedPreferences，
 * 因此同时写入 /data/local/tmp/vcam_config.json 供 Hook 读取。
 *
 * 路径说明：
 * - PREVIEW_VIDEO_PATH：/sdcard/DCIM/Camera1/virtual.mp4，VCAM 应用和 PreviewActivity 使用
 * - HOOK_VIDEO_PATH：/data/local/tmp/vcam/virtual.mp4，Hook 进程使用（通过 root 设置全局可读权限）
 */
public class VCamConfig {
    public static final String TAG = "VCAM_CONFIG";

    public static final String VIDEO_FILE_NAME = "virtual.mp4";

    // 预览路径：VCAM 应用和 PreviewActivity 使用（应用有存储权限可直接读写）
    // 注意：不能用 Environment.getExternalStorageDirectory()，因为在 system_server 早期启动时
    // 会因 IPackageManager 未初始化抛出 NPE，导致 HookMain 类加载失败。
    // /sdcard 在所有进程中都是有效的符号链接。
    public static final String PREVIEW_VIDEO_DIR = "/sdcard/DCIM/Camera1";
    public static final String PREVIEW_VIDEO_PATH = PREVIEW_VIDEO_DIR + "/" + VIDEO_FILE_NAME;

    // Hook 路径：Hook 进程使用（通过 root 设置权限，所有应用可读）
    public static final String HOOK_VIDEO_DIR = "/data/local/tmp/vcam";
    public static final String HOOK_VIDEO_PATH = HOOK_VIDEO_DIR + "/" + VIDEO_FILE_NAME;

    // 兼容旧代码
    public static final String DEFAULT_VIDEO_DIR = PREVIEW_VIDEO_DIR;
    public static final String DEFAULT_VIDEO_PATH = PREVIEW_VIDEO_PATH;

    public static final String HOOK_CONFIG_FILE = "/data/local/tmp/vcam_config.json";

    private static final String KEY_ORIGINAL_PATH = "original_path";
    private static final String KEY_MODE = "mode"; // 0=video, 1=stream
    private static final String KEY_DISABLED = "disabled";
    private static final String KEY_IMAGE_CORRECTION = "image_correction";
    private static final String KEY_LOOP_PLAYBACK = "loop_playback";
    private static final String KEY_VIDEO_WIDTH = "video_width";
    private static final String KEY_VIDEO_HEIGHT = "video_height";
    private static final String KEY_SYSTEM_ACTIVATED = "system_activated";

    private final Context context;

    public VCamConfig(Context context) {
        this.context = context.getApplicationContext();
    }

    private File getLocalConfigFile() {
        return new File(context.getFilesDir(), "vcam_settings.json");
    }

    private JSONObject readLocal() {
        File file = getLocalConfigFile();
        if (!file.exists()) {
            return new JSONObject();
        }
        try {
            BufferedReader reader = new BufferedReader(new FileReader(file));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line);
            }
            reader.close();
            return new JSONObject(sb.toString());
        } catch (IOException | JSONException e) {
            Log.e(TAG, "readLocal failed", e);
            return new JSONObject();
        }
    }

    private void writeLocal(JSONObject obj) {
        try {
            FileWriter writer = new FileWriter(getLocalConfigFile());
            writer.write(obj.toString());
            writer.flush();
            writer.close();
        } catch (IOException e) {
            Log.e(TAG, "writeLocal failed", e);
        }
    }

    private void putLocal(String key, Object value) {
        JSONObject obj = readLocal();
        try {
            obj.put(key, value);
        } catch (JSONException e) {
            Log.e(TAG, "putLocal failed", e);
        }
        writeLocal(obj);
        syncToHookConfig();
    }

    private Object getLocal(String key, Object defaultValue) {
        JSONObject obj = readLocal();
        return obj.opt(key) != null ? obj.opt(key) : defaultValue;
    }

    public void setOriginalPath(String path) {
        putLocal(KEY_ORIGINAL_PATH, path);
    }

    public String getOriginalPath() {
        Object value = getLocal(KEY_ORIGINAL_PATH, "");
        return value != null ? value.toString() : "";
    }

    public void setMode(int mode) {
        putLocal(KEY_MODE, mode);
    }

    public int getMode() {
        Object value = getLocal(KEY_MODE, 0);
        return value instanceof Number ? ((Number) value).intValue() : 0;
    }

    public void setDisabled(boolean disabled) {
        putLocal(KEY_DISABLED, disabled);
    }

    public boolean isDisabled() {
        Object value = getLocal(KEY_DISABLED, false);
        return Boolean.TRUE.equals(value);
    }

    public void setImageCorrection(boolean enabled) {
        putLocal(KEY_IMAGE_CORRECTION, enabled);
    }

    public boolean isImageCorrection() {
        Object value = getLocal(KEY_IMAGE_CORRECTION, false);
        return Boolean.TRUE.equals(value);
    }

    public void setLoopPlayback(boolean enabled) {
        putLocal(KEY_LOOP_PLAYBACK, enabled);
    }

    public boolean isLoopPlayback() {
        Object value = getLocal(KEY_LOOP_PLAYBACK, true);
        return Boolean.TRUE.equals(value);
    }

    public void setSystemActivated(boolean activated) {
        putLocal(KEY_SYSTEM_ACTIVATED, activated);
    }

    public boolean isSystemActivated() {
        Object value = getLocal(KEY_SYSTEM_ACTIVATED, false);
        return Boolean.TRUE.equals(value);
    }

    public void setVideoResolution(int width, int height) {
        putLocal(KEY_VIDEO_WIDTH, width);
        putLocal(KEY_VIDEO_HEIGHT, height);
    }

    public int getVideoWidth() {
        Object value = getLocal(KEY_VIDEO_WIDTH, 0);
        return value instanceof Number ? ((Number) value).intValue() : 0;
    }

    public int getVideoHeight() {
        Object value = getLocal(KEY_VIDEO_HEIGHT, 0);
        return value instanceof Number ? ((Number) value).intValue() : 0;
    }

    /**
     * 将当前本地配置同步一份到 /data/local/tmp，供 Xposed Hook 读取。
     * video_path 指向 HOOK_VIDEO_PATH（/data/local/tmp/vcam/virtual.mp4）。
     *
     * 策略：先尝试直接写入（部分设备 /data/local/tmp 可写），
     * 失败则写临时文件 + root 复制，root 不可用时仅警告不报错。
     */
    private void syncToHookConfig() {
        try {
            JSONObject obj = readLocal();
            obj.put("video_path", HOOK_VIDEO_PATH);
            String content = obj.toString();

            // 1. 先尝试直接写入 /data/local/tmp/（部分设备可直接写）
            try {
                File file = new File(HOOK_CONFIG_FILE);
                File parent = file.getParentFile();
                if (parent != null && !parent.exists()) {
                    parent.mkdirs();
                }
                FileWriter writer = new FileWriter(file);
                writer.write(content);
                writer.flush();
                writer.close();
                file.setReadable(true, false);
                Log.d(TAG, "syncToHookConfig: written directly to " + HOOK_CONFIG_FILE);
                return;
            } catch (Exception e) {
                Log.d(TAG, "syncToHookConfig: direct write failed (" + e.getMessage() + "), trying root...");
            }

            // 2. 写入 app 临时目录，再用 root 复制
            File tmpFile = new File(context.getCacheDir(), "vcam_config_tmp.json");
            FileWriter fw = new FileWriter(tmpFile);
            fw.write(content);
            fw.flush();
            fw.close();

            boolean ok = execRootCmd("cp " + tmpFile.getAbsolutePath() + " " + HOOK_CONFIG_FILE
                + " && chmod 644 " + HOOK_CONFIG_FILE);
            tmpFile.delete();

            if (ok) {
                Log.d(TAG, "syncToHookConfig: synced via root");
            } else {
                Log.w(TAG, "syncToHookConfig: cannot sync to " + HOOK_CONFIG_FILE
                    + " (no root, no permission). Hook will use defaults.");
            }
        } catch (Exception e) {
            Log.e(TAG, "syncToHookConfig failed", e);
        }
    }

    /**
     * 供 Xposed Hook（运行在其他应用进程）读取配置。
     */
    public static JSONObject readHookConfig() {
        try {
            File file = new File(HOOK_CONFIG_FILE);
            if (!file.exists()) {
                return null;
            }
            BufferedReader reader = new BufferedReader(new FileReader(file));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line);
            }
            reader.close();
            return new JSONObject(sb.toString());
        } catch (IOException | JSONException e) {
            Log.e(TAG, "readHookConfig failed", e);
            return null;
        }
    }

    public static String getHookVideoPath() {
        JSONObject config = readHookConfig();
        if (config != null) {
            return config.optString("video_path", HOOK_VIDEO_PATH);
        }
        return HOOK_VIDEO_PATH;
    }

    public static boolean isHookDisabled() {
        JSONObject config = readHookConfig();
        if (config != null) {
            return config.optBoolean(KEY_DISABLED, false);
        }
        return false;
    }

    public static boolean isHookImageCorrection() {
        JSONObject config = readHookConfig();
        if (config != null) {
            return config.optBoolean(KEY_IMAGE_CORRECTION, false);
        }
        return false;
    }

    public static boolean isHookLoopPlayback() {
        JSONObject config = readHookConfig();
        if (config != null) {
            return config.optBoolean(KEY_LOOP_PLAYBACK, true);
        }
        return true;
    }

    public static int getHookVideoWidth() {
        JSONObject config = readHookConfig();
        if (config != null) {
            return config.optInt(KEY_VIDEO_WIDTH, 0);
        }
        return 0;
    }

    public static int getHookVideoHeight() {
        JSONObject config = readHookConfig();
        if (config != null) {
            return config.optInt(KEY_VIDEO_HEIGHT, 0);
        }
        return 0;
    }

    /**
     * 以 root 权限执行命令（需要手机已 root）。
     * 自动查找 su 二进制文件路径（Magisk su 可能不在系统 PATH 中）。
     */
    private static String SU_PATH = null;
    private static long SU_LAST_CHECK = 0;
    private static final long SU_CHECK_COOLDOWN = 5000; // 缓存结果 5 秒

    public static String findSu() {
        if (SU_PATH != null) return SU_PATH;
        if (SU_LAST_CHECK > 0 && System.currentTimeMillis() - SU_LAST_CHECK < SU_CHECK_COOLDOWN) {
            Log.d(TAG, "findSu: using cached result (null)");
            return null;
        }
        SU_LAST_CHECK = System.currentTimeMillis();
        Log.d(TAG, "findSu: starting detection...");

        // 0. 检查 app 自身是否已经有 root (uid=0)
        if (checkSelfUid()) {
            SU_PATH = "self";
            Log.i(TAG, "findSu: app already running as root (uid=0)");
            return SU_PATH;
        }

        // 1. 用户手动设置的 su 路径
        String manualPath = getManualSuPath();
        if (manualPath != null && !manualPath.isEmpty()) {
            Log.d(TAG, "findSu: trying manual path: " + manualPath);
            if (testSuPath(manualPath)) {
                SU_PATH = manualPath;
                Log.i(TAG, "findSu: using manual su: " + manualPath);
                return SU_PATH;
            }
            Log.d(TAG, "findSu: manual path " + manualPath + " failed");
        }

        // 2. KernelSU ksud daemon
        Log.d(TAG, "findSu: trying ksud...");
        if (tryKsud()) {
            SU_PATH = "ksud";
            Log.i(TAG, "findSu: using ksud daemon");
            return SU_PATH;
        }
        Log.d(TAG, "findSu: ksud failed");

        // 3. KernelSU / APatch sh 直接 root
        Log.d(TAG, "findSu: trying sh root...");
        if (tryShRoot()) {
            SU_PATH = "sh";
            Log.i(TAG, "findSu: using sh directly (app has root)");
            return SU_PATH;
        }
        Log.d(TAG, "findSu: sh root failed");

        // 4. 标准 su 路径
        Log.d(TAG, "findSu: trying standard su paths...");
        if (tryStandardSu()) {
            Log.i(TAG, "findSu: found su at " + SU_PATH);
            return SU_PATH;
        }
        Log.d(TAG, "findSu: all standard su paths failed");

        // 5. 自动复制 su
        Log.d(TAG, "findSu: trying auto copy su...");
        if (autoCopySu()) {
            SU_PATH = "/data/local/tmp/su";
            Log.i(TAG, "findSu: auto-copied su to /data/local/tmp/su");
            return SU_PATH;
        }
        Log.d(TAG, "findSu: auto copy su failed");

        // 6. 最后尝试：直接执行 su 触发 Magisk 授权弹窗
        Log.d(TAG, "findSu: trying su -c whoami as last resort...");
        if (tryLastResortSu()) {
            Log.i(TAG, "findSu: last resort su succeeded");
            return SU_PATH;
        }

        // 7. 诊断 su 文件权限和 SELinux 上下文，帮助定位问题
        diagnoseSuFiles();

        Log.e(TAG, "findSu: ALL methods failed - su not found.");
        return null;
    }

    /** 诊断 su 文件权限和 SELinux 上下文 */
    private static void diagnoseSuFiles() {
        try {
            java.lang.Process p = Runtime.getRuntime().exec(new String[]{
                "sh", "-c",
                "echo '=== SU DIAG ===';" +
                "echo 'uid='$(id -u);" +
                "echo 'selinux='$(getenforce 2>/dev/null || echo unknown);" +
                "echo 'context='$(cat /proc/self/attr/current 2>/dev/null || echo unknown);" +
                "for d in /data/adb /data/adb/ksu /data/adb/ksu/bin /data/adb/ap /data/adb/ap/bin /data/adb/magisk; do " +
                "  ls -ld \"$d\" 2>&1;" +
                "done;" +
                "for f in /data/adb/ksu/bin/su /data/adb/ap/bin/su /data/adb/magisk/su /data/adb/ksud /data/adb/ksu/bin/ksud; do " +
                "  [ -f \"$f\" ] && ls -lZ \"$f\" 2>&1 || echo \"$f: not found\";" +
                "done;" +
                "for f in /dev/ksud /dev/kernel_su; do " +
                "  [ -e \"$f\" ] && ls -l \"$f\" 2>&1 || echo \"$f: not found\";" +
                "done"
            });
            java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                Log.i(TAG, "SUDIAG: " + line);
            }
            r.close();
            p.waitFor();
        } catch (Exception e) {
            Log.d(TAG, "diagnoseSuFiles error: " + e.getMessage());
        }
    }

    /** 检查当前进程是否已经是 root (uid=0) */
    private static boolean checkSelfUid() {
        try {
            int uid = android.os.Process.myUid();
            Log.d(TAG, "checkSelfUid: uid=" + uid);
            return uid == 0;
        } catch (Exception e) {
            Log.d(TAG, "checkSelfUid: error=" + e.getMessage());
            return false;
        }
    }

    /** 测试单个 su 路径是否可用 */
    private static boolean testSuPath(String path) {
        try {
            ProcessBuilder pb = new ProcessBuilder(path, "-c", "id");
            pb.redirectErrorStream(true);
            Process proc = pb.start();
            java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(proc.getInputStream()));
            StringBuilder out = new StringBuilder();
            String line;
            long deadline = System.currentTimeMillis() + 5000; // 5秒超时
            while ((line = r.readLine()) != null && System.currentTimeMillis() < deadline) {
                out.append(line);
            }
            r.close();
            int exit = proc.waitFor();
            Log.d(TAG, "testSuPath: " + path + " exit=" + exit + " output=" + out.toString().trim());
            return exit == 0 && out.toString().contains("uid=0");
        } catch (Exception e) {
            Log.d(TAG, "testSuPath: " + path + " exception=" + e.getClass().getSimpleName() + ": " + e.getMessage());
            return false;
        }
    }

    private static boolean tryKsud() {
        String[] ksudPaths = {"/data/adb/ksud", "/data/adb/ksu/bin/ksud"};
        for (String ksudPath : ksudPaths) {
            try {
                ProcessBuilder pb = new ProcessBuilder(ksudPath, "exec", "id");
                pb.redirectErrorStream(true);
                Process proc = pb.start();
                java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.InputStreamReader(proc.getInputStream()));
                StringBuilder out = new StringBuilder();
                String line;
                while ((line = r.readLine()) != null) out.append(line);
                r.close();
                int exit = proc.waitFor();
                Log.d(TAG, "tryKsud: " + ksudPath + " exit=" + exit + " out=" + out.toString().trim());
                if (out.toString().contains("uid=0")) return true;
            } catch (Exception e) {
                Log.d(TAG, "tryKsud: " + ksudPath + " error: " + e.getClass().getSimpleName());
            }
        }
        return false;
    }

    private static boolean tryShRoot() {
        try {
            // 测试1: 尝试写 /data/local/tmp
            ProcessBuilder pb = new ProcessBuilder("sh", "-c",
                "echo test > /data/local/tmp/.vcam_test 2>/dev/null && echo OK || echo FAIL");
            pb.redirectErrorStream(true);
            Process proc = pb.start();
            java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(proc.getInputStream()));
            String result = r.readLine();
            r.close();
            int exit = proc.waitFor();
            Log.d(TAG, "tryShRoot: test1 exit=" + exit + " result=" + result);
            if ("OK".equals(result)) return true;

            // 测试2: 尝试直接运行 id 看是否是 root
            pb = new ProcessBuilder("sh", "-c", "id 2>/dev/null");
            pb.redirectErrorStream(true);
            proc = pb.start();
            r = new java.io.BufferedReader(new java.io.InputStreamReader(proc.getInputStream()));
            StringBuilder out = new StringBuilder();
            String line;
            while ((line = r.readLine()) != null) out.append(line);
            r.close();
            proc.waitFor();
            Log.d(TAG, "tryShRoot: test2 id=" + out.toString().trim());
            if (out.toString().contains("uid=0")) return true;
        } catch (Exception e) {
            Log.d(TAG, "tryShRoot: error: " + e.getClass().getSimpleName());
        }
        return false;
    }

    private static boolean tryStandardSu() {
        String[] paths = {
            "/data/adb/ksu/bin/su",
            "/data/adb/ap/bin/su",
            "/data/adb/apd/bin/su",
            "/debug_ramdisk/su",
            "/system/bin/su",
            "/system/xbin/su",
            "/sbin/su",
            "/su/bin/su",
            "/data/adb/magisk/su",
            "/data/adb/magisk/busybox",
            "su"
        };
        for (String p : paths) {
            Log.d(TAG, "tryStandardSu: testing " + p);
            if (testSuPath(p)) {
                SU_PATH = p;
                return true;
            }
        }
        return false;
    }

    /**
     * 最后手段：直接执行 su 命令，可能会触发 Magisk/KernelSU 的授权弹窗
     */
    private static boolean tryLastResortSu() {
        // 尝试用 Runtime.exec 直接调 su（某些设备上 ProcessBuilder 行为不同）
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", "id"});
            java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(p.getInputStream()));
            StringBuilder out = new StringBuilder();
            String line;
            long deadline = System.currentTimeMillis() + 8000; // 等8秒等用户授权
            while ((line = r.readLine()) != null && System.currentTimeMillis() < deadline) {
                out.append(line);
            }
            r.close();
            int exit = p.waitFor();
            Log.d(TAG, "tryLastResortSu: Runtime.exec su exit=" + exit + " out=" + out.toString().trim());
            if (exit == 0 && out.toString().contains("uid=0")) {
                SU_PATH = "su";
                return true;
            }
        } catch (Exception e) {
            Log.d(TAG, "tryLastResortSu: error=" + e.getClass().getSimpleName() + ": " + e.getMessage());
        }
        return false;
    }

    /**
     * 自动从常见位置复制 su 到 /data/local/tmp/su
     */
    private static boolean autoCopySu() {
        // 方法1：从 MT 管理器等终端模拟器复制
        String[] sources = {
            "/data/user/0/bin.mt.plus.canary/files/term/bin/su",
            "/data/data/bin.mt.plus.canary/files/term/bin/su",
            "/data/user/0/bin.mt.plus/files/term/bin/su",
            "/data/data/bin.mt.plus/files/term/bin/su",
        };
        for (String src : sources) {
            try {
                java.io.File srcFile = new java.io.File(src);
                if (!srcFile.exists()) {
                    Log.d(TAG, "autoCopySu: source not found: " + src);
                    continue;
                }
                java.io.FileInputStream fis = new java.io.FileInputStream(src);
                java.io.FileOutputStream fos = new java.io.FileOutputStream("/data/local/tmp/su");
                byte[] buf = new byte[8192];
                int len;
                while ((len = fis.read(buf)) > 0) fos.write(buf, 0, len);
                fis.close();
                fos.close();
                Runtime.getRuntime().exec(new String[]{"chmod", "755", "/data/local/tmp/su"}).waitFor();
                Log.i(TAG, "autoCopySu: copied from " + src);
                return true;
            } catch (Exception e) {
                Log.d(TAG, "autoCopySu: copy from " + src + " failed: " + e.getClass().getSimpleName());
            }
        }

        // 方法2：用 ksud 创建 su
        try {
            ProcessBuilder pb = new ProcessBuilder(
                "/data/adb/ksud", "exec", "cp", "-f",
                "/data/adb/ksu/bin/su", "/data/local/tmp/su");
            pb.redirectErrorStream(true);
            Process p = pb.start();
            int exit = p.waitFor();
            Log.d(TAG, "autoCopySu: ksud cp exit=" + exit);
            if (new java.io.File("/data/local/tmp/su").exists()) {
                Runtime.getRuntime().exec(new String[]{"chmod", "755", "/data/local/tmp/su"}).waitFor();
                Log.i(TAG, "autoCopySu: created su via ksud");
                return true;
            }
        } catch (Exception e) {
            Log.d(TAG, "autoCopySu: ksud method failed: " + e.getClass().getSimpleName());
        }

        // 方法3：从系统路径搜索并复制
        try {
            ProcessBuilder pb = new ProcessBuilder("sh", "-c",
                "for p in /system/bin/su /system/xbin/su /sbin/su " +
                "/data/adb/magisk/su /data/adb/ksu/bin/su /data/adb/ap/bin/su /debug_ramdisk/su; do " +
                "[ -f \"$p\" ] && cp \"$p\" /data/local/tmp/su && chmod 755 /data/local/tmp/su && echo OK:$p && exit 0; " +
                "done; echo FAIL");
            pb.redirectErrorStream(true);
            Process p = pb.start();
            java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(p.getInputStream()));
            String result = r.readLine();
            r.close();
            p.waitFor();
            Log.d(TAG, "autoCopySu: search result=" + result);
            if (result != null && result.startsWith("OK:")) {
                Log.i(TAG, "autoCopySu: copied from " + result.substring(3));
                return true;
            }
        } catch (Exception e) {
            Log.d(TAG, "autoCopySu: search method failed: " + e.getClass().getSimpleName());
        }

        return false;
    }

    /**
     * 获取用户手动设置的 su 路径（存在 App 自己的目录，不需要 root）
     */
    static String getManualSuPath() {
        try {
            java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.FileReader("/data/data/com.example.vcam/files/vcam_su_path"));
            String path = r.readLine();
            r.close();
            return (path != null && !path.trim().isEmpty()) ? path.trim() : null;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * 保存用户手动设置的 su 路径（存在 App 自己的目录，不需要 root）
     */
    public static void setManualSuPath(String path) {
        try {
            java.io.FileWriter w = new java.io.FileWriter("/data/data/com.example.vcam/files/vcam_su_path");
            w.write(path);
            w.close();
            SU_PATH = null;
        } catch (Exception e) {
            Log.e(TAG, "setManualSuPath failed", e);
        }
    }

    public static boolean execRootCmd(String cmd) {
        try {
            String su = findSu();
            if (su == null) {
                Log.e(TAG, "execRootCmd failed: no root, cmd=" + cmd);
                return false;
            }
            String[] args;
            if ("ksud".equals(su)) {
                args = new String[]{"/data/adb/ksud", "exec", cmd};
            } else if ("sh".equals(su)) {
                args = new String[]{"sh", "-c", cmd};
            } else if ("self".equals(su)) {
                // app 本身就是 root，直接执行
                args = new String[]{"sh", "-c", cmd};
            } else {
                args = new String[]{su, "-c", cmd};
            }
            ProcessBuilder pb = new ProcessBuilder(args);
            pb.redirectErrorStream(true);
            Process p = pb.start();
            java.io.InputStream is = p.getInputStream();
            byte[] buf = new byte[4096];
            int len;
            while ((len = is.read(buf)) != -1) { }
            is.close();
            int exitCode = p.waitFor();
            Log.d(TAG, "execRootCmd: '" + cmd + "' exit=" + exitCode);
            return exitCode == 0;
        } catch (Exception e) {
            Log.e(TAG, "execRootCmd failed: " + cmd, e);
            return false;
        }
    }

    /**
     * 将视频文件和缩略图从预览路径拷贝到 Hook 路径，并设置全局可读权限。
     * 在点击"替换"时调用。
     */
    public static boolean deployToHookPath() {
        // 用 root 创建目录并设置权限为 777（让 VCAM 应用可以直接写入）
        execRootCmd("mkdir -p " + HOOK_VIDEO_DIR);
        execRootCmd("chmod 777 " + HOOK_VIDEO_DIR);
        execRootCmd("chmod 1777 /data/local/tmp");

        File srcFile = new File(PREVIEW_VIDEO_PATH);
        if (!srcFile.exists() || srcFile.length() == 0) {
            Log.e(TAG, "deployToHookPath: 源视频文件不存在或为空: " + PREVIEW_VIDEO_PATH);
            return false;
        }
        long srcSize = srcFile.length();
        Log.d(TAG, "deployToHookPath: 源文件大小=" + srcSize);

        // 先删除旧文件（防止旧空文件干扰）
        execRootCmd("rm -f " + HOOK_VIDEO_PATH);

        // 尝试直接拷贝（不需要 root，因为目录权限已设为 777）
        boolean copied = false;
        try {
            FileInputStream fis = new FileInputStream(srcFile);
            FileOutputStream fos = new FileOutputStream(HOOK_VIDEO_PATH);
            byte[] buf = new byte[8192];
            int len;
            while ((len = fis.read(buf)) != -1) {
                fos.write(buf, 0, len);
            }
            fos.flush();
            fos.getFD().sync();
            fos.close();
            fis.close();
            copied = true;
            Log.d(TAG, "deployToHookPath: 直接拷贝成功");
        } catch (Exception e) {
            Log.e(TAG, "deployToHookPath: 直接拷贝失败: " + e);
        }

        // 如果直接拷贝失败，用 root 命令拷贝
        if (!copied) {
            boolean ok = execRootCmd("cp " + PREVIEW_VIDEO_PATH + " " + HOOK_VIDEO_PATH);
            Log.d(TAG, "deployToHookPath: root cp 结果=" + ok);
        }

        // 用 root 设置文件权限为 644（全局可读）
        execRootCmd("chmod 644 " + HOOK_VIDEO_PATH);

        // 验证拷贝结果
        File hookFile = new File(HOOK_VIDEO_PATH);
        long hookSize = hookFile.length();
        Log.d(TAG, "deployToHookPath: 目标文件大小=" + hookSize + " canRead=" + hookFile.canRead());

        // 拷贝缩略图
        File thumbSrc = new File(PREVIEW_VIDEO_DIR, "1000.jpg");
        if (thumbSrc.exists()) {
            execRootCmd("rm -f " + HOOK_VIDEO_DIR + "/1000.jpg");
            execRootCmd("cp " + thumbSrc.getAbsolutePath() + " " + HOOK_VIDEO_DIR + "/1000.jpg");
            execRootCmd("chmod 644 " + HOOK_VIDEO_DIR + "/1000.jpg");
        }

        return hookSize > 0;
    }
}
