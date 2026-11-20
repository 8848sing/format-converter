package com.formatconverter.app;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * 转换项与分类的英文名。
 *
 * <p>C++ 注册表里的 label 只有中文，英文界面下需要这一层映射；缺失时回退到原 label。
 */
public final class OpLabels {

    private static final Map<String, String> EN_NAMES = new HashMap<>();
    private static final Map<String, String> EN_CATEGORIES = new HashMap<>();

    static {
        EN_NAMES.put("image_convert", "Convert image");
        EN_NAMES.put("image_to_pdf", "Image to PDF");
        EN_NAMES.put("images_merge_pdf", "Images to one PDF");
        EN_NAMES.put("images_to_gif", "Images to GIF");
        EN_NAMES.put("pdf_to_image", "PDF to images");
        EN_NAMES.put("pdf_to_word", "PDF to Word");
        EN_NAMES.put("pdf_to_excel", "PDF to Excel");
        EN_NAMES.put("pdf_to_ppt", "PDF to PPT");
        EN_NAMES.put("pdf_to_text", "PDF to text");
        EN_NAMES.put("pdf_merge", "Merge PDFs");
        EN_NAMES.put("pdf_split", "Split PDF");
        EN_NAMES.put("pdf_rotate", "Rotate PDF");
        EN_NAMES.put("pdf_compress", "Compress PDF");
        EN_NAMES.put("data_convert", "Convert data");
        EN_NAMES.put("data_to_excel", "Data to Excel");
        EN_NAMES.put("table_to_json", "Excel / CSV to JSON");

        EN_NAMES.put("android_extract_audio", "Extract audio (M4A)");
        EN_NAMES.put("android_video_to_gif", "Video to GIF");
        EN_NAMES.put("android_video_transcode", "Video to MP4 (H.264)");
        EN_NAMES.put("android_audio_transcode", "Audio to M4A (AAC)");
        EN_NAMES.put("android_audio_mp3", "Audio to MP3");

        EN_CATEGORIES.put("图片", "Images");
        EN_CATEGORIES.put("文档", "Docs");
        EN_CATEGORIES.put("音视频", "Media");
        EN_CATEGORIES.put("数据", "Data");
    }

    private OpLabels() {
    }

    public static boolean isEnglish() {
        return "en".equals(Locale.getDefault().getLanguage());
    }

    public static String name(String id, String fallback) {
        if (!isEnglish()) {
            return fallback;
        }
        String name = EN_NAMES.get(id);
        return name != null ? name : fallback;
    }

    public static String category(String src) {
        if (!isEnglish()) {
            return src;
        }
        String name = EN_CATEGORIES.get(src);
        return name != null ? name : src;
    }
}