package com.example.vcam;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import android.widget.ImageButton;
import android.widget.Toast;

import java.util.Arrays;

/**
 * 前置预览：通过 Camera2 API 打开摄像头并显示预览。
 *
 * 当系统级 Hook 或 LSPosed Hook 激活时，摄像头数据会被替换为选择的视频，从而实现
 * "查看替换后效果" 的目的。如果 Hook 未激活，则显示原始摄像头画面。
 */
public class PreviewActivity extends Activity implements TextureView.SurfaceTextureListener {

    private static final String TAG = "VCAM_Preview";
    private static final int REQUEST_CAMERA = 200;

    private TextureView textureView;
    private ImageButton btnClose;

    private CameraDevice cameraDevice;
    private CameraCaptureSession captureSession;
    private HandlerThread backgroundThread;
    private Handler backgroundHandler;
    private String cameraId;
    private boolean isSurfaceReady = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_preview);

        textureView = findViewById(R.id.preview_surface);
        btnClose = findViewById(R.id.btn_close_preview);

        textureView.setSurfaceTextureListener(this);
        btnClose.setOnClickListener(v -> finish());
    }

    @Override
    protected void onResume() {
        super.onResume();
        startBackgroundThread();
        if (isSurfaceReady) {
            openCamera();
        }
    }

    @Override
    protected void onPause() {
        closeCamera();
        stopBackgroundThread();
        super.onPause();
    }

    // ---------- SurfaceTextureListener ----------

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
        isSurfaceReady = true;
        openCamera();
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {
    }

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
        isSurfaceReady = false;
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surface) {
    }

    // ---------- Camera2 ----------

    private void startBackgroundThread() {
        backgroundThread = new HandlerThread("CameraPreview");
        backgroundThread.start();
        backgroundHandler = new Handler(backgroundThread.getLooper());
    }

    private void stopBackgroundThread() {
        if (backgroundThread != null) {
            backgroundThread.quitSafely();
            try {
                backgroundThread.join();
            } catch (InterruptedException e) {
                Log.e(TAG, "stopBackgroundThread interrupted", e);
            }
            backgroundThread = null;
            backgroundHandler = null;
        }
    }

    private void openCamera() {
        // Android 6+ 需要运行时请求 CAMERA 权限
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.CAMERA}, REQUEST_CAMERA);
                return;
            }
        }

        CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
        if (manager == null) {
            Toast.makeText(this, "无法获取 CameraManager", Toast.LENGTH_SHORT).show();
            return;
        }

        try {
            // 优先使用前置摄像头
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics chars = manager.getCameraCharacteristics(id);
                Integer facing = chars.get(CameraCharacteristics.LENS_FACING);
                if (facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT) {
                    cameraId = id;
                    break;
                }
            }
            // 没有前置就用后置
            if (cameraId == null && manager.getCameraIdList().length > 0) {
                cameraId = manager.getCameraIdList()[0];
            }
            if (cameraId == null) {
                Toast.makeText(this, "未找到摄像头", Toast.LENGTH_SHORT).show();
                return;
            }

            Log.d(TAG, "openCamera: opening " + cameraId);
            manager.openCamera(cameraId, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    cameraDevice = camera;
                    Log.d(TAG, "Camera opened: " + cameraId);
                    createPreviewSession();
                }

                @Override
                public void onDisconnected(CameraDevice camera) {
                    Log.w(TAG, "Camera disconnected");
                    camera.close();
                    cameraDevice = null;
                }

                @Override
                public void onError(CameraDevice camera, int error) {
                    Log.e(TAG, "Camera error: " + error);
                    camera.close();
                    cameraDevice = null;
                }
            }, backgroundHandler);

        } catch (CameraAccessException e) {
            Log.e(TAG, "openCamera failed", e);
            Toast.makeText(this, "摄像头访问失败: " + e.getMessage(), Toast.LENGTH_LONG).show();
        } catch (SecurityException e) {
            Log.e(TAG, "Camera permission denied", e);
            Toast.makeText(this, "缺少摄像头权限", Toast.LENGTH_LONG).show();
        }
    }

    private void createPreviewSession() {
        if (cameraDevice == null || !isSurfaceReady) return;

        SurfaceTexture surfaceTexture = textureView.getSurfaceTexture();
        if (surfaceTexture == null) return;

        // 用 TextureView 的实际尺寸作为预览尺寸
        Surface surface = new Surface(surfaceTexture);

        try {
            cameraDevice.createCaptureSession(
                Arrays.asList(surface),
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession session) {
                        captureSession = session;
                        try {
                            CaptureRequest.Builder builder = cameraDevice.createCaptureRequest(
                                CameraDevice.TEMPLATE_PREVIEW);
                            builder.addTarget(surface);
                            session.setRepeatingRequest(builder.build(), null, backgroundHandler);
                            Log.d(TAG, "Preview started - 如果 Hook 生效，将显示替换后的画面");
                        } catch (CameraAccessException e) {
                            Log.e(TAG, "startPreview failed", e);
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession session) {
                        Log.e(TAG, "createCaptureSession failed");
                        Toast.makeText(PreviewActivity.this, "预览配置失败", Toast.LENGTH_SHORT).show();
                    }
                },
                backgroundHandler
            );
        } catch (CameraAccessException e) {
            Log.e(TAG, "createCaptureSession error", e);
        }
    }

    private void closeCamera() {
        if (captureSession != null) {
            captureSession.close();
            captureSession = null;
        }
        if (cameraDevice != null) {
            cameraDevice.close();
            cameraDevice = null;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_CAMERA && grantResults.length > 0
                && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            openCamera();
        } else {
            Toast.makeText(this, "需要摄像头权限才能预览替换效果", Toast.LENGTH_LONG).show();
        }
    }
}
