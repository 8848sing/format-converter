package com.formatconverter.app;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Locale;

/**
 * 把转换结果从应用私有目录发布到系统可见的位置，方便用户在「下载」里找到、也能直接分享。
 *
 * <ul>
 *   <li>Android 10+：写入 MediaStore 的 Download 集合，不需要任何存储权限；</li>
 *   <li>Android 8/9：复制到 <code>Download/格式转换工具</code>，需要 WRITE_EXTERNAL_STORAGE。</li>
 * </ul>
 */
public final class OutputPublisher {

    public static final String FOLDER = "格式转换工具";

    private OutputPublisher() {
    }

    /** 发布单个文件；返回可分享的 content:// Uri，低版本返回 null（仅复制）。 */
    public static Uri publish(Context context, File file, String mime) throws Exception {
        if (Build.VERSION.SDK_INT >= 29) {
            return publishToMediaStore(context, file, mime);
        }
        publishLegacy(file);
        return null;
    }

    public static Intent shareIntent(ArrayList<Uri> uris, String mime) {
        Intent intent = new Intent(uris.size() > 1 ? Intent.ACTION_SEND_MULTIPLE : Intent.ACTION_SEND);
        intent.setType(mime);
        if (uris.size() > 1) {
            intent.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
        } else {
            intent.putExtra(Intent.EXTRA_STREAM, uris.get(0));
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return intent;
    }

    public static String mimeOf(String fileName) {
        return MimeTypes.of(fileName);
    }

    private static Uri publishToMediaStore(Context context, File file, String mime) throws Exception {
        ContentResolver resolver = context.getContentResolver();
        // 图片进相册（Pictures），其它文件进下载目录，用户更容易找到。
        final boolean image = mime.startsWith("image/");
        final Uri collection = image ? MediaStore.Images.Media.EXTERNAL_CONTENT_URI
                : MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        final String baseDir = image ? Environment.DIRECTORY_PICTURES : Environment.DIRECTORY_DOWNLOADS;
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, file.getName());
        values.put(MediaStore.MediaColumns.MIME_TYPE, mime);
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, baseDir + File.separator + FOLDER);
        values.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = resolver.insert(collection, values);
        if (uri == null) {
            throw new Exception("无法在系统媒体库创建文件");
        }
        try {
            try (InputStream in = new FileInputStream(file);
                 OutputStream out = resolver.openOutputStream(uri)) {
                if (out == null) {
                    throw new Exception("无法写入下载目录");
                }
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            }
            ContentValues done = new ContentValues();
            done.put(MediaStore.MediaColumns.IS_PENDING, 0);
            resolver.update(uri, done, null, null);
            return uri;
        } catch (Exception exc) {
            resolver.delete(uri, null, null);
            throw exc;
        }
    }

    private static void publishLegacy(File file) throws Exception {
        File dir = new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS), FOLDER);
        if (!dir.exists() && !dir.mkdirs()) {
            throw new Exception("无法创建下载目录");
        }
        File target = new File(dir, file.getName());
        int index = 1;
        String name = file.getName();
        int dot = name.lastIndexOf('.');
        String stem = dot > 0 ? name.substring(0, dot) : name;
        String ext = dot > 0 ? name.substring(dot) : "";
        while (target.exists()) {
            target = new File(dir, stem + "_" + index + ext);
            index++;
        }
        try (InputStream in = new FileInputStream(file);
             OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        }
    }
}