package com.formatconverter.app;

public final class NativeCore {
    static {
        System.loadLibrary("fccore_jni");
    }

    /** C++ 核心的流式事件回调：每条事件是一行 UTF-8 JSON。 */
    public interface EventListener {
        void onEvent(String eventJson);
    }

    private NativeCore() {
    }

    public static native String version();

    public static native String registryJson();

    /** 同步执行任务，事件收集到返回值的 events 数组里（适合一次性拿完整结果）。 */
    public static native String runJob(String jobJson, String baseDir);

    /** 同步执行任务，事件实时回调给 listener；返回值是 {"code","ok","failed","cancelled","error"}。 */
    public static native String runJobStreaming(String jobJson, String baseDir, EventListener listener);

    /** 把 16-bit LE 交错 PCM 编码成 MP3；返回空串表示成功，否则是错误信息。 */
    public static native String encodeMp3(String pcmPath, int sampleRate, int channels,
                                          int bitrateKbps, String outPath);

    /** 请求取消当前任务：尚未开始的文件会被跳过。 */
    public static native void cancel();
}