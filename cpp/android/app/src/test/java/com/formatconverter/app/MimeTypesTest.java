package com.formatconverter.app;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

/** 纯 JVM 单元测试：验证 MIME 映射这类不依赖 Android 框架的逻辑。 */
public class MimeTypesTest {

    @Test
    public void mapsCommonImageFormats() {
        assertEquals("image/png", MimeTypes.of("a.png"));
        assertEquals("image/png", MimeTypes.of("A.PNG"));
        assertEquals("image/jpeg", MimeTypes.of("photo.JPEG"));
        assertEquals("image/webp", MimeTypes.of("x.webp"));
        assertEquals("image/tiff", MimeTypes.of("scan.tif"));
        assertEquals("image/heic", MimeTypes.of("IMG_0001.HEIC"));
        assertEquals("image/avif", MimeTypes.of("cover.avif"));
    }

    @Test
    public void mapsDocumentsAndData() {
        assertEquals("application/pdf", MimeTypes.of("doc.pdf"));
        assertEquals("application/json", MimeTypes.of("data.json"));
        assertEquals("text/csv", MimeTypes.of("table.csv"));
        assertEquals("application/yaml", MimeTypes.of("cfg.yml"));
    }

    @Test
    public void fallsBackForUnknownAndMissingExtension() {
        assertEquals("application/octet-stream", MimeTypes.of("noext"));
        assertEquals("application/octet-stream", MimeTypes.of("archive.7z"));
        assertEquals("application/octet-stream", MimeTypes.of("weird."));
    }

    @Test
    public void handlesNamesWithDotsAndSpaces() {
        assertEquals("image/png", MimeTypes.of("封面 设计稿_1.png"));
        assertEquals("application/pdf", MimeTypes.of("第 2.0 版.pdf"));
    }
}