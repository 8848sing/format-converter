package com.formatconverter.app;

import android.graphics.Bitmap;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMetadataRetriever;
import android.media.MediaMuxer;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * 安卓端的音视频子集：不依赖 FFmpeg，只用系统 MediaExtractor / MediaMuxer / MediaMetadataRetriever。
 *
 * <ul>
 *   <li>提取音轨：把视频里的 AAC 音轨无损复制成 .m4a（不解码、不重编码，几秒完成）；</li>
 *   <li>视频转 GIF：按固定间隔抽帧，再交给 C++ 核心的 images_to_gif 合成。</li>
 * </ul>
 *
 * 真正的转码（MP4 ↔ MKV、码率调整）需要 MediaCodec 全链路，这里不做。
 */
final class MediaConverter {

    /** GIF 最多抽多少帧；太多会让体积和内存都失控。 */
    private static final int GIF_MAX_FRAMES = 24;
    private static final int GIF_MAX_WIDTH = 480;

    private MediaConverter() {
    }

    static File extractAudio(File input, File outDir, String stem) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaMuxer muxer = null;
        try {
            extractor.setDataSource(input.getAbsolutePath());
            int audioTrack = -1;
            MediaFormat format = null;
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat candidate = extractor.getTrackFormat(i);
                String mime = candidate.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("audio/")) {
                    audioTrack = i;
                    format = candidate;
                    break;
                }
            }
            if (audioTrack < 0 || format == null) {
                throw new Exception("这段视频没有音轨");
            }
            String mime = format.getString(MediaFormat.KEY_MIME);
            if (!"audio/mp4a-latm".equals(mime)) {
                throw new Exception("音轨编码是 " + mime + "，手机端只能无损封装 AAC 音轨");
            }

            File out = new File(outDir, stem + ".m4a");
            int index = 1;
            while (out.exists()) {
                out = new File(outDir, stem + "_" + index + ".m4a");
                index++;
            }

            extractor.selectTrack(audioTrack);
            muxer = new MediaMuxer(out.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            int trackIndex = muxer.addTrack(format);
            muxer.start();

            ByteBuffer buffer = ByteBuffer.allocateDirect(512 * 1024);
            android.media.MediaCodec.BufferInfo info = new android.media.MediaCodec.BufferInfo();
            int samples = 0;
            while (true) {
                int size = extractor.readSampleData(buffer, 0);
                if (size < 0) {
                    break;
                }
                info.offset = 0;
                info.size = size;
                info.presentationTimeUs = extractor.getSampleTime();
                info.flags = extractor.getSampleFlags();
                muxer.writeSampleData(trackIndex, buffer, info);
                extractor.advance();
                samples++;
            }
            muxer.stop();
            if (samples == 0) {
                throw new Exception("没有读到音频数据");
            }
            return out;
        } finally {
            try {
                extractor.release();
            } catch (Exception ignored) {
            }
            if (muxer != null) {
                try {
                    muxer.release();
                } catch (Exception ignored) {
                }
            }
        }
    }

    static File videoToGif(File input, File outDir, String stem, String baseDir) throws Exception {
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        File framesDir = new File(outDir, ".frames_" + System.nanoTime());
        try {
            retriever.setDataSource(input.getAbsolutePath());
            long durationMs;
            try {
                durationMs = Long.parseLong(
                        retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION));
            } catch (Exception exc) {
                throw new Exception("读不出视频时长");
            }
            if (durationMs <= 0) {
                throw new Exception("读不出视频时长");
            }
            long step = Math.max(120, durationMs / GIF_MAX_FRAMES);
            if (!framesDir.mkdirs()) {
                throw new Exception("无法创建临时帧目录");
            }

            List<File> frames = new ArrayList<>();
            int index = 0;
            for (long timeMs = 0; timeMs < durationMs && index < GIF_MAX_FRAMES; timeMs += step) {
                Bitmap bitmap = retriever.getFrameAtTime(timeMs * 1000,
                        MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
                if (bitmap == null) {
                    continue;
                }
                Bitmap scaled = bitmap;
                if (bitmap.getWidth() > GIF_MAX_WIDTH) {
                    int height = Math.max(1,
                            Math.round(bitmap.getHeight() * (GIF_MAX_WIDTH / (float) bitmap.getWidth())));
                    scaled = Bitmap.createScaledBitmap(bitmap, GIF_MAX_WIDTH, height, true);
                }
                File frame = new File(framesDir, String.format("frame_%03d.png", index));
                try (FileOutputStream stream = new FileOutputStream(frame)) {
                    if (!scaled.compress(Bitmap.CompressFormat.PNG, 100, stream)) {
                        throw new Exception("写入帧失败");
                    }
                } finally {
                    if (scaled != bitmap) {
                        scaled.recycle();
                    }
                    bitmap.recycle();
                }
                frames.add(frame);
                index++;
            }
            if (frames.size() < 2) {
                throw new Exception("视频太短，抽不到足够的帧");
            }

            JSONObject job = new JSONObject();
            job.put("op", "images_to_gif");
            JSONArray inputs = new JSONArray();
            for (File frame : frames) {
                inputs.put(frame.getAbsolutePath());
            }
            job.put("inputs", inputs);
            job.put("out_dir", outDir.getAbsolutePath());
            JSONObject options = new JSONObject();
            options.put("duration_ms", step);
            job.put("options", options);

            String result = NativeCore.runJobStreaming(job.toString(), baseDir, event -> {
            });
            JSONObject parsed = new JSONObject(result);
            if (parsed.optInt("code", 2) != 0) {
                throw new Exception("合成 GIF 失败：" + parsed.optString("error"));
            }

            File produced = new File(outDir, stripExtension(frames.get(0).getName()) + "-动图.gif");
            File target = new File(outDir, stem + ".gif");
            int suffix = 1;
            while (target.exists()) {
                target = new File(outDir, stem + "_" + suffix + ".gif");
                suffix++;
            }
            if (!produced.exists() || !produced.renameTo(target)) {
                throw new Exception("找不到生成的 GIF");
            }
            return target;
        } finally {
            try {
                retriever.release();
            } catch (Exception ignored) {
            }
            deleteDir(framesDir);
        }
    }

    private static String stripExtension(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    private static void deleteDir(File dir) {
        if (dir == null || !dir.exists()) {
            return;
        }
        File[] children = dir.listFiles();
        if (children != null) {
            for (File child : children) {
                //noinspection ResultOfMethodCallIgnored
                child.delete();
            }
        }
        //noinspection ResultOfMethodCallIgnored
        dir.delete();
    }
}