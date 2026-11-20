package com.formatconverter.app;

import android.content.Context;

import java.io.File;

/**
 * 任意音视频 → MP3：先用 MediaCodec 解成 PCM，再交给 C++ 里的 shine 编码。
 *
 * <p>系统没有 MP3 编码器，所以编码放在 native 侧；解码仍用系统能力，避免引入 FFmpeg。
 */
final class AudioToMp3 {

    private static final int DEFAULT_BITRATE = 192;

    private AudioToMp3() {
    }

    static File convert(Context context, File input, File outDir, String stem) throws Exception {
        File pcm = new File(context.getCacheDir(), "pcm_" + System.nanoTime() + ".raw");
        try {
            AudioPcmDecoder.PcmInfo info = AudioPcmDecoder.decodeToPcm(input, pcm);
            if (info.sampleRate != 32000 && info.sampleRate != 44100 && info.sampleRate != 48000) {
                throw new Exception("MP3 只支持 32 / 44.1 / 48 kHz，当前音轨是 "
                        + info.sampleRate + " Hz");
            }
            if (info.channels < 1 || info.channels > 2) {
                throw new Exception("MP3 编码只支持单声道或双声道，当前是 "
                        + info.channels + " 声道");
            }

            File out = new File(outDir, stem + ".mp3");
            int index = 1;
            while (out.exists()) {
                out = new File(outDir, stem + "_" + index + ".mp3");
                index++;
            }

            String error = NativeCore.encodeMp3(pcm.getAbsolutePath(), info.sampleRate,
                    info.channels, DEFAULT_BITRATE, out.getAbsolutePath());
            if (error != null && !error.isEmpty()) {
                //noinspection ResultOfMethodCallIgnored
                out.delete();
                throw new Exception(error);
            }
            if (!out.exists() || out.length() == 0) {
                throw new Exception("没有生成 MP3 文件");
            }
            return out;
        } finally {
            //noinspection ResultOfMethodCallIgnored
            pcm.delete();
        }
    }
}