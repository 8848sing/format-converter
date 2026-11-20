package com.formatconverter.app;

import java.util.Locale;

/**
 * 文件名 → MIME 的纯逻辑映射。
 *
 * <p>抽出来是为了能在 JVM 单元测试里直接验证（不依赖 android.net.Uri 之类的框架类）。
 */
public final class MimeTypes {

    private MimeTypes() {
    }

    public static String of(String fileName) {
        String name = fileName.toLowerCase(Locale.ROOT);
        int dot = name.lastIndexOf('.');
        String ext = dot >= 0 ? name.substring(dot + 1) : "";
        switch (ext) {
            case "jpg":
            case "jpeg":
                return "image/jpeg";
            case "png":
                return "image/png";
            case "webp":
                return "image/webp";
            case "gif":
                return "image/gif";
            case "bmp":
                return "image/bmp";
            case "ico":
                return "image/x-icon";
            case "tga":
                return "image/x-tga";
            case "ppm":
            case "pgm":
                return "image/x-portable-pixmap";
            case "tif":
            case "tiff":
                return "image/tiff";
            case "heic":
            case "heif":
                return "image/heic";
            case "avif":
                return "image/avif";
            case "pdf":
                return "application/pdf";
            case "doc":
                return "application/msword";
            case "docx":
                return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
            case "xls":
                return "application/vnd.ms-excel";
            case "xlsx":
                return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
            case "csv":
                return "text/csv";
            case "json":
                return "application/json";
            case "xml":
                return "application/xml";
            case "yaml":
            case "yml":
                return "application/yaml";
            case "txt":
                return "text/plain";
            case "mp4":
            case "m4v":
                return "video/mp4";
            case "mkv":
                return "video/x-matroska";
            case "mov":
                return "video/quicktime";
            case "webm":
                return "video/webm";
            case "mp3":
                return "audio/mpeg";
            case "wav":
                return "audio/wav";
            case "m4a":
                return "audio/mp4";
            case "flac":
                return "audio/flac";
            case "ogg":
                return "audio/ogg";
            default:
                return "application/octet-stream";
        }
    }
}