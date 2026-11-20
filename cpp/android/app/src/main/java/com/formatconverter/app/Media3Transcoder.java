package com.formatconverter.app;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import androidx.annotation.Nullable;
import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.transformer.Composition;
import androidx.media3.transformer.EditedMediaItem;
import androidx.media3.transformer.ExportException;
import androidx.media3.transformer.ExportResult;
import androidx.media3.transformer.ProgressHolder;
import androidx.media3.transformer.Transformer;

import java.io.File;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * 真转码：用官方的 Media3 Transformer，底层走系统 MediaCodec。
 *
 * <p>与 {@link MediaConverter} 的区别：
 * <ul>
 *   <li>提取音轨 = 无损 remux，不解码；</li>
 *   <li>这里的转码 = 解码 + 重新编码，能把 mpeg4/VP9/MKV 之类统一成 H.264 + AAC 的 MP4，
 *       也能把 MP3 等音频转成 AAC 的 M4A。</li>
 * </ul>
 *
 * 能力边界由设备上的 MediaCodec 决定：MP3 编码、WMV/FLV 这类容器系统不支持，会明确报错。
 */
final class Media3Transcoder {

    private Media3Transcoder() {
    }

    /** 视频 → MP4（H.264 + AAC）。 */
    static File toMp4(Context context, File input, File outDir, String stem,
                      @Nullable ProgressSink sink) throws Exception {
        return run(context, input, outDir, stem, ".mp4", false, sink);
    }

    /**
     * 音频 → M4A（AAC），去掉视频轨。
     *
     * <p>Transformer 只认 .mp4/.webm 这类已知输出后缀，所以先产出 .mp4 再改名为 .m4a
     * （容器同为 MPEG-4，播放器和系统相册都能识别）。
     */
    static File toM4a(Context context, File input, File outDir, String stem,
                      @Nullable ProgressSink sink) throws Exception {
        File produced = run(context, input, outDir, stem + "_aac", ".mp4", true, sink);
        File target = new File(outDir, stem + ".m4a");
        int index = 1;
        while (target.exists()) {
            target = new File(outDir, stem + "_" + index + ".m4a");
            index++;
        }
        if (!produced.renameTo(target)) {
            throw new Exception("无法重命名为 .m4a");
        }
        return target;
    }

    interface ProgressSink {
        void onProgress(int percent);
    }

    private static File run(Context context, File input, File outDir, String stem,
                            String suffix, boolean audioOnly,
                            @Nullable ProgressSink sink) throws Exception {
        File out = new File(outDir, stem + suffix);
        int index = 1;
        while (out.exists()) {
            out = new File(outDir, stem + "_" + index + suffix);
            index++;
        }

        CountDownLatch latch = new CountDownLatch(1);
        AtomicReference<String> failure = new AtomicReference<>();

        // Transformer 要求在应用主线程上调用 start()。
        Handler main = new Handler(Looper.getMainLooper());
        // setVideoMimeType 不接受 null：纯音频时干脆不设置视频输出格式。
        Transformer.Builder builder = new Transformer.Builder(context)
                .setAudioMimeType(MimeTypes.AUDIO_AAC)
                .addListener(new Transformer.Listener() {
                    @Override
                    public void onCompleted(Composition composition, ExportResult result) {
                        latch.countDown();
                    }

                    @Override
                    public void onError(Composition composition, ExportResult result,
                                        ExportException exception) {
                        failure.set(exception.getMessage() != null
                                ? exception.getMessage() : exception.toString());
                        latch.countDown();
                    }
                });
        if (!audioOnly) {
            builder.setVideoMimeType(MimeTypes.VIDEO_H264);
        }
        Transformer transformer = builder.build();

        EditedMediaItem item = new EditedMediaItem.Builder(MediaItem.fromUri(Uri.fromFile(input)))
                .setRemoveVideo(audioOnly)
                .build();

        final Exception[] startError = new Exception[1];
        final File outputFile = out;
        final String outputPath = out.getAbsolutePath();
        main.post(() -> {
            try {
                transformer.start(item, outputPath);
            } catch (Exception exc) {
                startError[0] = exc;
                latch.countDown();
            }
        });

        // 轮询进度，顺便汇报给通知栏。
        // Transformer 必须在主线程访问，所以每次读取都 post 回主线程。
        while (!latch.await(500, TimeUnit.MILLISECONDS)) {
            if (sink != null) {
                main.post(() -> {
                    ProgressHolder holder = new ProgressHolder();
                    if (transformer.getProgress(holder)
                            == Transformer.PROGRESS_STATE_AVAILABLE) {
                        sink.onProgress(holder.progress);
                    }
                });
            }
        }
        if (startError[0] != null) {
            outputFile.delete();
            throw new Exception("转码启动失败：" + startError[0].getMessage());
        }
        if (failure.get() != null) {
            outputFile.delete();
            throw new Exception("转码失败：" + failure.get());
        }
        if (!outputFile.exists() || outputFile.length() == 0) {
            throw new Exception("没有生成输出文件");
        }
        return outputFile;
    }
}
