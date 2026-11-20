package com.formatconverter.app;

import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;

/**
 * 把任意音轨解码成 16-bit little-endian 的裸 PCM，供 MP3 编码器使用。
 *
 * <p>系统 MediaCodec 只负责解码；编码交给 C++ 里的 shine（见 {@link NativeCore#encodeMp3}）。
 */
final class AudioPcmDecoder {

    static final class PcmInfo {
        final int sampleRate;
        final int channels;

        PcmInfo(int sampleRate, int channels) {
            this.sampleRate = sampleRate;
            this.channels = channels;
        }
    }

    private AudioPcmDecoder() {
    }

    static PcmInfo decodeToPcm(File input, File pcmFile) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec codec = null;
        try {
            extractor.setDataSource(input.getAbsolutePath());
            int trackIndex = -1;
            MediaFormat format = null;
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat candidate = extractor.getTrackFormat(i);
                String mime = candidate.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("audio/")) {
                    trackIndex = i;
                    format = candidate;
                    break;
                }
            }
            if (trackIndex < 0 || format == null) {
                throw new Exception("这个文件没有音轨");
            }

            final String mime = format.getString(MediaFormat.KEY_MIME);
            final int sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            final int channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            int pcmEncoding = AudioFormat.ENCODING_PCM_16BIT;
            if (format.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                pcmEncoding = format.getInteger(MediaFormat.KEY_PCM_ENCODING);
            }

            codec = MediaCodec.createDecoderByType(mime);
            codec.configure(format, null, null, 0);
            codec.start();
            extractor.selectTrack(trackIndex);

            final boolean floatPcm = pcmEncoding == AudioFormat.ENCODING_PCM_FLOAT;
            try (FileOutputStream out = new FileOutputStream(pcmFile)) {
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                boolean inputDone = false;
                boolean outputDone = false;
                final long timeoutUs = 10000;
                while (!outputDone) {
                    if (!inputDone) {
                        int inIndex = codec.dequeueInputBuffer(timeoutUs);
                        if (inIndex >= 0) {
                            ByteBuffer buffer = codec.getInputBuffer(inIndex);
                            if (buffer == null) {
                                throw new Exception("解码器输入缓冲区不可用");
                            }
                            int size = extractor.readSampleData(buffer, 0);
                            if (size < 0) {
                                codec.queueInputBuffer(inIndex, 0, 0, 0,
                                        MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                inputDone = true;
                            } else {
                                codec.queueInputBuffer(inIndex, 0, size,
                                        extractor.getSampleTime(), 0);
                                extractor.advance();
                            }
                        }
                    }

                    int outIndex = codec.dequeueOutputBuffer(info, timeoutUs);
                    if (outIndex >= 0) {
                        ByteBuffer buffer = codec.getOutputBuffer(outIndex);
                        if (info.size > 0 && buffer != null) {
                            buffer.position(info.offset);
                            buffer.limit(info.offset + info.size);
                            if (floatPcm) {
                                writeFloatAsInt16(buffer, out);
                            } else {
                                byte[] chunk = new byte[info.size];
                                buffer.get(chunk);
                                out.write(chunk);
                            }
                        }
                        codec.releaseOutputBuffer(outIndex, false);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            outputDone = true;
                        }
                    } else if (outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        // 采样率/声道以输入格式为准，这里不需要重新读取
                    }
                }
            }
            return new PcmInfo(sampleRate, channels);
        } finally {
            if (codec != null) {
                try {
                    codec.stop();
                } catch (Exception ignored) {
                }
                try {
                    codec.release();
                } catch (Exception ignored) {
                }
            }
            try {
                extractor.release();
            } catch (Exception ignored) {
            }
        }
    }

    private static void writeFloatAsInt16(ByteBuffer buffer, FileOutputStream out)
            throws Exception {
        final int samples = buffer.remaining() / 4;
        byte[] converted = new byte[samples * 2];
        for (int i = 0; i < samples; i++) {
            float value = buffer.getFloat();
            if (value > 1f) {
                value = 1f;
            } else if (value < -1f) {
                value = -1f;
            }
            short sample = (short) (value * 32767f);
            converted[i * 2] = (byte) sample;
            converted[i * 2 + 1] = (byte) (sample >> 8);
        }
        out.write(converted);
    }
}