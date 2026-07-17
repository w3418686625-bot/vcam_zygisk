package com.example.vcam;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.YuvImage;

import java.io.ByteArrayOutputStream;

/**
 * 视频帧缩放与画面纠正工具。
 */
public class FrameScaler {

    /**
     * 将 NV21 数据缩放并可选水平镜像到目标宽高。
     */
    public static byte[] scaleMirrorNV21(byte[] nv21, int srcWidth, int srcHeight,
                                         int dstWidth, int dstHeight, boolean mirror) {
        Bitmap src = nv21ToBitmap(nv21, srcWidth, srcHeight);
        if (src == null) {
            return nv21;
        }
        Bitmap scaled = scaleAndMirrorBitmap(src, dstWidth, dstHeight, mirror);
        src.recycle();
        byte[] result = bitmapToNV21(scaled);
        scaled.recycle();
        return result;
    }

    public static Bitmap nv21ToBitmap(byte[] nv21, int width, int height) {
        try {
            YuvImage yuvImage = new YuvImage(nv21, ImageFormat.NV21, width, height, null);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            yuvImage.compressToJpeg(new Rect(0, 0, width, height), 100, out);
            byte[] imageBytes = out.toByteArray();
            return android.graphics.BitmapFactory.decodeByteArray(imageBytes, 0, imageBytes.length);
        } catch (Exception e) {
            return null;
        }
    }

    public static byte[] bitmapToNV21(Bitmap bitmap) {
        int width = bitmap.getWidth();
        int height = bitmap.getHeight();
        int[] pixels = new int[width * height];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        return rgb2YCbCr420(pixels, width, height);
    }

    public static Bitmap scaleAndMirrorBitmap(Bitmap src, int dstWidth, int dstHeight, boolean mirror) {
        Bitmap scaled = Bitmap.createBitmap(dstWidth, dstHeight, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(scaled);
        Matrix matrix = new Matrix();
        // 保持比例并裁剪填充
        float scaleX = (float) dstWidth / src.getWidth();
        float scaleY = (float) dstHeight / src.getHeight();
        float scale = Math.max(scaleX, scaleY);
        float dx = (dstWidth - src.getWidth() * scale) * 0.5f;
        float dy = (dstHeight - src.getHeight() * scale) * 0.5f;
        matrix.postScale(scale, scale);
        matrix.postTranslate(dx, dy);
        if (mirror) {
            matrix.postScale(-1, 1, dstWidth / 2f, dstHeight / 2f);
        }
        Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        canvas.drawBitmap(src, matrix, paint);
        return scaled;
    }

    private static byte[] rgb2YCbCr420(int[] pixels, int width, int height) {
        int len = width * height;
        byte[] yuv = new byte[len * 3 / 2];
        int y, u, v;
        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                int rgb = pixels[i * width + j] & 0x00FFFFFF;
                int r = rgb & 0xFF;
                int g = (rgb >> 8) & 0xFF;
                int b = (rgb >> 16) & 0xFF;
                y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                y = y < 16 ? 16 : (Math.min(y, 255));
                u = u < 0 ? 0 : (Math.min(u, 255));
                v = v < 0 ? 0 : (Math.min(v, 255));
                yuv[i * width + j] = (byte) y;
                // NV21: V 在前，U 在后
                yuv[len + (i >> 1) * width + (j & ~1)] = (byte) v;
                yuv[len + (i >> 1) * width + (j & ~1) + 1] = (byte) u;
            }
        }
        return yuv;
    }
}
