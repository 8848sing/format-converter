#include "image.h"
#include "image_android.h"
#include "fs_utils.h"

#include <jni.h>

#include <mutex>
#include <string>

namespace fc {

namespace {

std::mutex g_java_mutex;
JavaVM* g_vm = nullptr;
jclass g_codec_class = nullptr;
jmethodID g_decode_method = nullptr;

/* 取当前线程的 JNIEnv；工作线程不是 Java 线程时挂到虚拟机（守护线程，退出自动分离）。 */
JNIEnv* thread_env() {
    JavaVM* vm = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_java_mutex);
        vm = g_vm;
    }
    if (!vm) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    const jint status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
        return env;
    }
    if (status == JNI_EDETACHED && vm->AttachCurrentThreadAsDaemon(&env, nullptr) == JNI_OK) {
        return env;
    }
    return nullptr;
}

std::string describe_java_error(JNIEnv* env, jthrowable throwable) {
    if (!throwable) {
        return "Java 解码器抛出未知异常";
    }
    std::string message = "Java 解码器失败";
    jclass cls = env->GetObjectClass(throwable);
    if (cls) {
        jmethodID to_string = env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
        if (to_string) {
            auto* text = static_cast<jstring>(env->CallObjectMethod(throwable, to_string));
            if (!env->ExceptionCheck() && text) {
                const char* chars = env->GetStringUTFChars(text, nullptr);
                if (chars) {
                    message = chars;
                    env->ReleaseStringUTFChars(text, chars);
                }
                env->DeleteLocalRef(text);
            } else {
                env->ExceptionClear();
            }
        }
        env->DeleteLocalRef(cls);
    }
    return message;
}

}  // namespace

void android_bind_java(JavaVM* vm, jclass image_codec_class) {
    std::lock_guard<std::mutex> lock(g_java_mutex);
    g_vm = vm;
    if (!image_codec_class || g_codec_class) {
        return;
    }
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        return;
    }
    g_codec_class = static_cast<jclass>(env->NewGlobalRef(image_codec_class));
    if (g_codec_class) {
        g_decode_method = env->GetStaticMethodID(g_codec_class, "decodeRgba",
                                                 "(Ljava/lang/String;[I)[B");
        if (!g_decode_method) {
            env->ExceptionClear();
            env->DeleteGlobalRef(g_codec_class);
            g_codec_class = nullptr;
        }
    }
}

bool platform_decode_image(const fs::path& path, Image& image, std::string& error) {
    jclass cls = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_java_mutex);
        cls = g_codec_class;
        method = g_decode_method;
    }
    const std::string ext = extension_lower(path);
    if (!cls || !method) {
        error = "Android 图片解码器未初始化";
        return false;
    }
    JNIEnv* env = thread_env();
    if (!env) {
        error = "无法获取 JNI 环境";
        return false;
    }

    const std::string utf8_path = path_to_utf8(path);
    auto* jpath = env->NewStringUTF(utf8_path.c_str());
    auto* size = env->NewIntArray(2);
    if (!jpath || !size) {
        if (jpath) {
            env->DeleteLocalRef(jpath);
        }
        if (size) {
            env->DeleteLocalRef(size);
        }
        error = "内存不足，无法调用解码器";
        return false;
    }

    auto* data = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, method, jpath, size));
    bool ok = false;
    if (env->ExceptionCheck()) {
        jthrowable throwable = env->ExceptionOccurred();
        env->ExceptionClear();
        error = describe_java_error(env, throwable);
        if (throwable) {
            env->DeleteLocalRef(throwable);
        }
    } else if (!data) {
        error = "解码器返回空数据";
    } else {
        const jsize length = env->GetArrayLength(data);
        jint dims[2] = {0, 0};
        env->GetIntArrayRegion(size, 0, 2, dims);
        const long long expected = static_cast<long long>(dims[0]) * dims[1] * 4;
        if (dims[0] <= 0 || dims[1] <= 0) {
            error = "解码结果尺寸无效";
        } else if (static_cast<long long>(length) != expected) {
            error = "解码结果长度与尺寸不匹配";
        } else {
            image.width = dims[0];
            image.height = dims[1];
            image.rgba.resize(static_cast<size_t>(length));
            env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte*>(image.rgba.data()));
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                image.width = 0;
                image.height = 0;
                image.rgba.clear();
                error = "读取解码数据失败";
            } else {
                ok = true;
            }
        }
        env->DeleteLocalRef(data);
    }

    env->DeleteLocalRef(size);
    env->DeleteLocalRef(jpath);
    if (!ok) {
        // 失败时给用户能看懂的原因，别把 "unknown image type" 之类抛到界面上。
        if (ext == ".tif" || ext == ".tiff") {
            error = "Android 系统没有内置 TIFF 解码器，请先把图转成 PNG / JPEG / WebP";
        } else if (error.empty() || error.find("未知") != std::string::npos) {
            error = "Android 系统无法解码这张图片（可能是特殊色彩配置或损坏）";
        }
    }
    return ok;
}

bool platform_encode_image(const Image& image, const fs::path& path, const std::string& format,
                           std::string& error) {
    (void)image;
    (void)path;
    error = "Android 端暂不支持编码 " + format + "（可用 PNG / JPEG / WebP）";
    return false;
}

}  // namespace fc