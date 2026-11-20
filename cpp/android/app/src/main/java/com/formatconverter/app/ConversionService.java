package com.formatconverter.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

import androidx.core.app.NotificationCompat;

/**
 * 转换期间的前台服务：唯一职责是「占住进程 + 显示进度通知」。
 *
 * <p>真正的转换仍在 {@link MainActivity} 的线程池里跑，这样 UI 与 C++ 核心的交互不用改，
 * 但系统不会因为应用退到后台就把进程回收掉，通知栏也能看到进度。
 */
public class ConversionService extends Service {

    private static final String CHANNEL_ID = "conversion_progress";
    private static final int NOTIFICATION_ID = 2001;

    private static volatile ConversionService instance;

    public static void start(Context context) {
        Intent intent = new Intent(context, ConversionService.class);
        if (Build.VERSION.SDK_INT >= 26) {
            context.startForegroundService(intent);
        } else {
            context.startService(intent);
        }
    }

    public static void stop(Context context) {
        context.stopService(new Intent(context, ConversionService.class));
    }

    /** 有任务在跑时更新通知进度；服务没起来就忽略。 */
    public static void updateProgress(Context context, int done, int total, String detail) {
        ConversionService service = instance;
        if (service == null) {
            return;
        }
        service.publish(buildNotification(context, done, total, detail));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        instance = this;
        createChannel();
        Notification notification = buildNotification(this, 0, 0,
                getString(R.string.notify_preparing));
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void publish(Notification notification) {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) {
            manager.notify(NOTIFICATION_ID, notification);
        }
    }

    private void createChannel() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager == null) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                getString(R.string.notify_channel), NotificationManager.IMPORTANCE_LOW);
        channel.setDescription(getString(R.string.notify_channel_desc));
        channel.setShowBadge(false);
        manager.createNotificationChannel(channel);
    }

    private static Notification buildNotification(Context context, int done, int total,
                                                  String detail) {
        Intent launch = new Intent(context, MainActivity.class);
        launch.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int pendingFlags = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        PendingIntent pending = PendingIntent.getActivity(context, 0, launch, pendingFlags);

        NotificationCompat.Builder builder = new NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_convert)
                .setContentTitle(total > 0
                        ? context.getString(R.string.notify_progress, done, total)
                        : context.getString(R.string.notify_converting))
                .setContentText(detail)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setSilent(true)
                .setContentIntent(pending);
        if (total > 0) {
            builder.setProgress(total, done, false);
        } else {
            builder.setProgress(0, 0, true);
        }
        return builder.build();
    }
}