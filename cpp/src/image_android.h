#pragma once

/*
 * Android 图片解码桥。
 *
 * 桌面端图片解码依赖 WIC（image_codec_win.cpp），Android 没有 WIC，
 * 因此这里通过 JNI 复用系统的 BitmapFactory / ImageDecoder：
 *  - 支持 HEIC/HEIF/AVIF 等系统解码器能识别的格式；
 *  - 由 JNI_OnLoad 在启动时把 JavaVM 与 Java 侧辅助类交给核心。
 */

#include <jni.h>

namespace fc {

/* 由 JNI_OnLoad 调用；image_codec_class 对应 com.formatconverter.app.ImageCodec。 */
void android_bind_java(JavaVM* vm, jclass image_codec_class);

}  // namespace fc