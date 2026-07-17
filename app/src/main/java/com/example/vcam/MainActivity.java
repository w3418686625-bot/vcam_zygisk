package com.example.vcam;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;
import android.widget.Button;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class MainActivity extends Activity {

    private static final int REQUEST_CODE_PICK_VIDEO = 100;
    private static final int REQUEST_PERMISSION = 1;

    private VCamConfig config;
    private TextView filePathText;
    private RadioGroup modeGroup;
    private RadioButton radioVideo;
    private RadioButton radioStream;
    private Switch switchCorrection;
    private Switch switchLoop;
    private Button btnReplace;
    private Button btnSystemHook;
    private Button btnFrontPreview;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        config = new VCamConfig(this);

        modeGroup = findViewById(R.id.mode_group);
        radioVideo = findViewById(R.id.radio_video);
        radioStream = findViewById(R.id.radio_stream);
        filePathText = findViewById(R.id.file_path_text);
        Button btnSelectVideo = findViewById(R.id.btn_select_video);
        btnReplace = findViewById(R.id.btn_replace);
        btnSystemHook = findViewById(R.id.btn_system_hook);
        btnFrontPreview = findViewById(R.id.btn_front_preview);
        switchCorrection = findViewById(R.id.switch_correction);
        switchLoop = findViewById(R.id.switch_loop);

        syncUiFromConfig();

        modeGroup.setOnCheckedChangeListener((group, checkedId) -> {
            if (checkedId == R.id.radio_video) {
                config.setMode(0);
            } else if (checkedId == R.id.radio_stream) {
                config.setMode(1);
            }
        });

        btnSelectVideo.setOnClickListener(v -> {
            if (!hasPermission()) {
                requestPermission();
                return;
            }
            openVideoPicker();
        });

        // ---- 替换 / 还原 ----
        btnReplace.setOnClickListener(v -> {
            if (config.isDisabled()) {
                // 当前未替换，点击"替换"
                doReplace();
            } else {
                // 当前已替换，点击"还原"
                config.setDisabled(true);
                Toast.makeText(this, "摄像头已还原，可通过前置预览验证", Toast.LENGTH_SHORT).show();
                syncUiFromConfig();
            }
        });

        // ---- 前置预览（始终可用，验证替换效果）----
        btnFrontPreview.setOnClickListener(v -> {
            Intent intent = new Intent(MainActivity.this, PreviewActivity.class);
            startActivity(intent);
        });

        // ---- 系统级激活 ----
        btnSystemHook.setOnClickListener(v -> {
            if (config.isSystemActivated()) {
                // 已激活，询问是否停用
                new AlertDialog.Builder(this)
                    .setTitle("全局替换已激活")
                    .setMessage("当前已激活全局摄像头替换。\n\n点击 [停用] 将恢复原始摄像头。")
                    .setPositiveButton("停用", (d, w) -> {
                        Toast.makeText(this, "正在停用...", Toast.LENGTH_SHORT).show();
                        new Thread(() -> {
                            SystemHookManager.deactivate();
                            config.setSystemActivated(false);
                            config.setDisabled(true);
                            runOnUiThread(() -> {
                                syncUiFromConfig();
                                Toast.makeText(this, "已停用，摄像头已恢复", Toast.LENGTH_LONG).show();
                            });
                        }).start();
                    })
                    .setNegativeButton("取消", null)
                    .show();
            } else {
                // 未激活，确认激活
                new AlertDialog.Builder(this)
                    .setTitle("激活全局摄像头替换")
                    .setMessage("将使用 Root 权限部署视频文件并配置全局替换。\n\n" +
                        "• 所有 App 的前后摄像头都将被替换\n" +
                        "• 需要 LSPosed + Root\n" +
                        "• 配置后可能需要重启手机\n\n" +
                        "激活后，点击 [替换] 即可启用替换效果。")
                    .setPositiveButton("检测并激活", (d, w) -> {
                        String su = VCamConfig.findSu();
                        if (su == null) {
                            new AlertDialog.Builder(MainActivity.this)
                                .setTitle("Root 不可用")
                                .setMessage("检测不到可用的 Root 权限。\n\n" +
                                    "可能的原因：\n" +
                                    "1. 需要在 KernelSU 管理器中授权 VCAM\n" +
                                    "2. 需要安装 KernelSU 的 su 模块\n\n" +
                                    "修复后重启手机再试。")
                                .setPositiveButton("确定", null)
                                .show();
                            return;
                        }
                        if (!hasVideo()) {
                            Toast.makeText(this, "请先选择视频文件", Toast.LENGTH_SHORT).show();
                            return;
                        }
                        doActivateSystemHook();
                    })
                    .setNegativeButton("取消", null)
                    .show();
            }
        });

        switchCorrection.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (buttonView.isPressed()) {
                config.setImageCorrection(isChecked);
            }
        });

        switchLoop.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (buttonView.isPressed()) {
                config.setLoopPlayback(isChecked);
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        syncUiFromConfig();
    }

    // ==================== UI 状态同步 ====================

    private boolean hasVideo() {
        String originalPath = config.getOriginalPath();
        return originalPath != null && !originalPath.isEmpty()
            && new File(VCamConfig.PREVIEW_VIDEO_PATH).exists()
            && new File(VCamConfig.PREVIEW_VIDEO_PATH).length() > 0;
    }

    private void syncUiFromConfig() {
        if (config.getMode() == 0) {
            radioVideo.setChecked(true);
        } else {
            radioStream.setChecked(true);
        }
        switchCorrection.setChecked(config.isImageCorrection());
        switchLoop.setChecked(config.isLoopPlayback());

        boolean hasVideo = hasVideo();
        boolean activated = config.isSystemActivated();
        boolean replacing = !config.isDisabled();

        // 文件路径显示
        if (!hasVideo) {
            filePathText.setText(R.string.no_file_selected);
        } else {
            filePathText.setText(R.string.file_selected);
        }

        // ---- 系统 Hook 按钮 ----
        if (activated) {
            btnSystemHook.setText("全局替换: 已激活 ✓");
            btnSystemHook.setTextColor(0xFF00FF00);
            btnSystemHook.setEnabled(true);
        } else {
            btnSystemHook.setText("1. 激活全局替换");
            btnSystemHook.setTextColor(0xFFFFFFFF);
            btnSystemHook.setEnabled(hasVideo); // 需要先选视频
        }

        // ---- 替换 / 还原按钮 ----
        if (!activated || !hasVideo) {
            // 未激活或无视频 → 替换按钮禁用
            btnReplace.setText("2. 替换");
            btnReplace.setTextColor(0xFF888888);
            btnReplace.setEnabled(false);
        } else if (replacing) {
            // 激活了且已替换 → 显示"还原"
            btnReplace.setText("还原摄像头");
            btnReplace.setTextColor(0xFFFF4444);
            btnReplace.setEnabled(true);
        } else {
            // 激活了但未替换 → 显示"替换"
            btnReplace.setText("2. 替换");
            btnReplace.setTextColor(0xFFFFFFFF);
            btnReplace.setEnabled(true);
        }

        // ---- 前置预览按钮（始终可用）----
        btnFrontPreview.setText("前置预览");
        btnFrontPreview.setEnabled(true);
    }

    // ==================== 操作 ====================

    private void doReplace() {
        File videoFile = new File(VCamConfig.PREVIEW_VIDEO_PATH);
        if (!videoFile.exists() || videoFile.length() == 0) {
            Toast.makeText(this, "视频文件不存在，请重新选择", Toast.LENGTH_SHORT).show();
            return;
        }

        // 部署视频到 Hook 路径
        boolean ok = VCamConfig.deployToHookPath();
        captureAndSaveVideoResolution();

        File hookFile = new File(VCamConfig.HOOK_VIDEO_PATH);
        if (ok && hookFile.length() > 0) {
            config.setDisabled(false);
            Toast.makeText(this, "替换成功！打开前置预览查看效果", Toast.LENGTH_LONG).show();
        } else {
            config.setDisabled(false);
            Toast.makeText(this, "部署失败，请检查 Root 权限", Toast.LENGTH_LONG).show();
        }
        syncUiFromConfig();
    }

    private void doActivateSystemHook() {
        Toast.makeText(this, "正在配置全局摄像头替换...", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            String result = SystemHookManager.activateWithResult();
            runOnUiThread(() -> {
                if (result == null) {
                    config.setSystemActivated(true);
                    config.setDisabled(true); // 初始不替换，等用户点替换
                    syncUiFromConfig();
                    new AlertDialog.Builder(MainActivity.this)
                        .setTitle("激活成功")
                        .setMessage("全局摄像头替换已配置完成！\n\n" +
                            "• 视频和配置已部署\n" +
                            "• 现在可以点击 [替换] 启用\n" +
                            "• 打开 [前置预览] 验证替换效果\n" +
                            "• 点击 [还原] 恢复原始摄像头")
                        .setPositiveButton("确定", null)
                        .show();
                } else {
                    new AlertDialog.Builder(MainActivity.this)
                        .setTitle("激活结果")
                        .setMessage(result)
                        .setPositiveButton("确定", null)
                        .show();
                }
            });
        }).start();
    }

    // ==================== 视频选择与权限 ====================

    private void openVideoPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("video/mp4");
        startActivityForResult(intent, REQUEST_CODE_PICK_VIDEO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_CODE_PICK_VIDEO && resultCode == Activity.RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                handleSelectedVideo(uri);
            }
        }
    }

    private void handleSelectedVideo(Uri uri) {
        ensureVideoDir();
        File destFile = new File(VCamConfig.PREVIEW_VIDEO_PATH);

        if (destFile.exists()) {
            destFile.delete();
        }

        if (copyUriToFile(uri, destFile)) {
            if (destFile.length() == 0) {
                Toast.makeText(this, "视频拷贝失败：文件大小为0", Toast.LENGTH_LONG).show();
                destFile.delete();
                return;
            }
            String originalName = getFileNameFromUri(uri);
            config.setOriginalPath(originalName);
            generateThumbnailFromVideo(destFile);
            filePathText.setText(R.string.file_selected);
            captureAndSaveVideoResolution();
            syncUiFromConfig();
            Toast.makeText(this, R.string.copy_success, Toast.LENGTH_SHORT).show();
        } else {
            Toast.makeText(this, R.string.copy_failed, Toast.LENGTH_SHORT).show();
        }
    }

    private void captureAndSaveVideoResolution() {
        File videoFile = new File(VCamConfig.PREVIEW_VIDEO_PATH);
        if (!videoFile.exists()) return;
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        try {
            retriever.setDataSource(videoFile.getAbsolutePath());
            String w = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH);
            String h = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT);
            if (w != null && h != null) {
                int width = Integer.parseInt(w);
                int height = Integer.parseInt(h);
                config.setVideoResolution(width, height);
                Log.i(VCamConfig.TAG, "captureResolution: " + width + "x" + height + " 已写入配置");
            }
        } catch (Exception e) {
            Log.e(VCamConfig.TAG, "captureResolution failed", e);
        } finally {
            try { retriever.release(); } catch (Exception ignored) {}
        }
    }

    private void generateThumbnailFromVideo(File videoFile) {
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        try {
            retriever.setDataSource(videoFile.getAbsolutePath());
            Bitmap frame = retriever.getFrameAtTime(0);
            if (frame != null) {
                File thumbFile = new File(VCamConfig.PREVIEW_VIDEO_DIR, "1000.jpg");
                FileOutputStream fos = new FileOutputStream(thumbFile);
                frame.compress(Bitmap.CompressFormat.JPEG, 100, fos);
                fos.flush();
                fos.close();
                frame.recycle();
            }
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            try { retriever.release(); } catch (Exception ignored) {}
        }
    }

    private void ensureVideoDir() {
        File dir = new File(VCamConfig.PREVIEW_VIDEO_DIR);
        if (!dir.exists()) dir.mkdirs();
    }

    private boolean copyUriToFile(Uri uri, File destFile) {
        InputStream inputStream = null;
        FileOutputStream outputStream = null;
        try {
            ContentResolver resolver = getContentResolver();
            inputStream = resolver.openInputStream(uri);
            if (inputStream == null) return false;
            outputStream = new FileOutputStream(destFile);
            byte[] buffer = new byte[8192];
            int read;
            long total = 0;
            while ((read = inputStream.read(buffer)) != -1) {
                outputStream.write(buffer, 0, read);
                total += read;
            }
            outputStream.flush();
            outputStream.getFD().sync();
            Log.d("VCAM", "拷贝完成: " + total + " bytes -> " + destFile.getAbsolutePath());
            return total > 0;
        } catch (Exception e) {
            Log.e("VCAM", "copyUriToFile failed", e);
            return false;
        } finally {
            try { if (outputStream != null) outputStream.close(); } catch (IOException e) {}
            try { if (inputStream != null) inputStream.close(); } catch (IOException e) {}
        }
    }

    private String getFileNameFromUri(Uri uri) {
        if (ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) {
            try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (idx >= 0) return cursor.getString(idx);
                }
            }
        }
        return uri.getLastPathSegment();
    }

    // ==================== 权限 ====================

    private void requestPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                new AlertDialog.Builder(this)
                    .setTitle(R.string.permission_lack_warn)
                    .setMessage(R.string.permission_description)
                    .setNegativeButton(R.string.negative, (d, w) ->
                        Toast.makeText(this, R.string.permission_lack_warn, Toast.LENGTH_SHORT).show())
                    .setPositiveButton(R.string.positive, (d, w) -> {
                        try {
                            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                                .setData(Uri.parse("package:" + getPackageName())));
                        } catch (Exception e) {
                            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                        }
                    }).show();
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_DENIED
                    || checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_DENIED) {
                requestPermissions(new String[]{
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE
                }, REQUEST_PERMISSION);
            }
        }
    }

    private boolean hasPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_DENIED
                    && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_DENIED;
        }
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_PERMISSION && grantResults.length > 0) {
            boolean granted = true;
            for (int result : grantResults) {
                if (result == PackageManager.PERMISSION_DENIED) { granted = false; break; }
            }
            if (granted) ensureVideoDir();
            else Toast.makeText(this, R.string.permission_lack_warn, Toast.LENGTH_SHORT).show();
        }
    }
}
