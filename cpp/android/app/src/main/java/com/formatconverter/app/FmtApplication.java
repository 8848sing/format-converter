package com.formatconverter.app;

import android.app.Application;
import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * 应用入口：接住未捕获异常，把堆栈写到私有目录，下次启动时可以查看/导出。
 *
 * <p>手机上没有 adb 的时候，这份日志是排查 JNI 崩溃之外问题的唯一线索。
 */
public class FmtApplication extends Application {

    private static final String CRASH_FILE = "last_crash.txt";

    @Override
    public void onCreate() {
        super.onCreate();
        final Thread.UncaughtExceptionHandler previous =
                Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            saveCrash(this, thread, error);
            if (previous != null) {
                previous.uncaughtException(thread, error);
            }
        });
    }

    private static void saveCrash(Context context, Thread thread, Throwable error) {
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT)
                .format(new Date());
        String text = "时间：" + stamp + "\n线程：" + thread.getName() + "\n\n"
                + Log.getStackTraceString(error);
        try (FileOutputStream stream = new FileOutputStream(crashFile(context))) {
            stream.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (Throwable ignored) {
        }
    }

    public static File crashFile(Context context) {
        return new File(context.getFilesDir(), CRASH_FILE);
    }

    public static boolean hasCrash(Context context) {
        return crashFile(context).exists();
    }

    public static String readCrash(Context context) {
        File file = crashFile(context);
        if (!file.exists()) {
            return "";
        }
        try {
            return new String(java.nio.file.Files.readAllBytes(file.toPath()),
                    StandardCharsets.UTF_8);
        } catch (IOException | RuntimeException exc) {
            return "读取崩溃日志失败：" + exc;
        }
    }

    public static void clearCrash(Context context) {
        //noinspection ResultOfMethodCallIgnored
        crashFile(context).delete();
    }
}