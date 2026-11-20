package com.formatconverter.app;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Build;

import java.io.File;
import java.io.IOException;

/**
 * 图片解码桥：给 C++ 核心提供 Android 系统解码器。
 *
 * <p>核心自带的 stb 只能解 JPEG/PNG/BMP/TGA/GIF/PPM，HEIC/HEIF/AVIF 等格式在 Android 上
 * 走这里：BitmapFactory 优先，失败再退回 ImageDecoder。返回的数据是逐行从上到下的 RGBA8，
 * 与核心的 Image 结构保持一致；EXIF 方向仍由核心统一处理。
 */
public final class ImageCodec {

    /** 超过该像素数时按 2 的幂降采样，避免手机内存被一张巨图打爆。 */
    private static final long MAX_PIXELS = 24_000_000L;

    private ImageCodec() {
    }

    /**
     * 由 C++ 核心通过 JNI 调用。
     *
     * @param path    图片路径
     * @param outSize 长度至少 2 的数组，返回时填入 [宽, 高]
     * @return RGBA8 字节数组
     */
    public static byte[] decodeRgba(String path, int[] outSize) throws IOException {
        Bitmap bitmap = decode(path);
        if (bitmap == null) {
            throw new IOException("Android 解码器无法识别该图片");
        }
        try {
            final int width = bitmap.getWidth();
            final int height = bitmap.getHeight();
            if (width <= 0 || height <= 0) {
                throw new IOException("图片尺寸无效");
            }
            final int[] argb = new int[width * height];
            bitmap.getPixels(argb, 0, width, 0, 0, width, height);
            final byte[] rgba = new byte[width * height * 4];
            for (int i = 0, j = 0; i < argb.length; i++) {
                final int pixel = argb[i];
                rgba[j++] = (byte) (pixel >> 16);
                rgba[j++] = (byte) (pixel >> 8);
                rgba[j++] = (byte) pixel;
                rgba[j++] = (byte) (pixel >>> 24);
            }
            if (outSize != null && outSize.length >= 2) {
                outSize[0] = width;
                outSize[1] = height;
            }
            return rgba;
        } finally {
            bitmap.recycle();
        }
    }

    private static Bitmap decode(String path) {
        int sample = 1;
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(path, bounds);
            sample = sampleSize(bounds.outWidth, bounds.outHeight);
        } catch (Throwable ignored) {
            sample = 1;
        }

        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        options.inSampleSize = sample;
        try {
            Bitmap bitmap = BitmapFactory.decodeFile(path, options);
            if (bitmap != null) {
                return bitmap;
            }
        } catch (Throwable ignored) {
        }
        try {
            return decodeWithImageDecoder(path, sample);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static int sampleSize(int width, int height) {
        if (width <= 0 || height <= 0) {
            return 1;
        }
        long pixels = (long) width * height;
        int sample = 1;
        while (pixels / ((long) sample * sample) > MAX_PIXELS) {
            sample *= 2;
        }
        return sample;
    }

    private static Bitmap decodeWithImageDecoder(String path, int sample) throws Exception {
        if (Build.VERSION.SDK_INT < 28) {
            return null;
        }
        android.graphics.ImageDecoder.Source source =
                android.graphics.ImageDecoder.createSource(new File(path));
        return android.graphics.ImageDecoder.decodeBitmap(source, (decoder, info, src) -> {
            decoder.setAllocator(android.graphics.ImageDecoder.ALLOCATOR_SOFTWARE);
            if (Build.VERSION.SDK_INT >= 31) {
                decoder.setMemorySizePolicy(android.graphics.ImageDecoder.MEMORY_POLICY_LOW_RAM);
            }
            if (sample > 1) {
                decoder.setTargetSampleSize(sample);
            }
        });
    }
}