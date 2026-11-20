#include <jni.h>

#include "fc/core.h"
#include "image_android.h"
#include "mp3_encoder.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

JavaVM* g_vm = nullptr;

std::mutex g_job_mutex;
fc_context* g_active_context = nullptr;
bool g_cancel_requested = false;

/* 单次任务的事件转发状态（runJob 非流式模式使用）。 */
struct CollectState {
    std::vector<std::string> events;
};

/* 流式模式：把每条事件回调给 Java 监听器，同时统计成功/失败数。 */
struct StreamState {
    JavaVM* vm = nullptr;
    jobject listener = nullptr;
    jmethodID on_event = nullptr;
    std::atomic<int> ok{0};
    std::atomic<int> failed{0};
    std::atomic<bool> cancelled{false};
};

std::string jstring_to_utf8(JNIEnv* env, jstring value) {
    if (!value) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) {
        return {};
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

/* 工作线程不是 Java 线程时挂到虚拟机上；守护线程退出时自动分离。 */
JNIEnv* attach_env(JavaVM* vm) {
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

jstring to_jstring(JNIEnv* env, const std::string& text) {
    return env->NewStringUTF(text.c_str());
}

void collect_callback(const char* event_json, void* user_data) {
    auto* state = static_cast<CollectState*>(user_data);
    if (!state || !event_json) {
        return;
    }
    state->events.emplace_back(event_json);
}

void stream_callback(const char* event_json, void* user_data) {
    auto* state = static_cast<StreamState*>(user_data);
    if (!state || !event_json) {
        return;
    }
    try {
        auto event = nlohmann::json::parse(event_json, nullptr, false);
        if (event.is_object()) {
            const std::string type = event.value("type", "");
            if (type == "file_ok") {
                state->ok.fetch_add(1);
            } else if (type == "file_error") {
                state->failed.fetch_add(1);
            } else if (type == "done") {
                state->cancelled.store(event.value("cancelled", false));
            }
        }
    } catch (...) {
    }
    JNIEnv* env = attach_env(state->vm);
    if (!env || !state->listener || !state->on_event) {
        return;
    }
    jstring text = env->NewStringUTF(event_json);
    if (text) {
        env->CallVoidMethod(state->listener, state->on_event, text);
        env->DeleteLocalRef(text);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

std::string config_for(const std::string& base_dir) {
    nlohmann::json config;
    config["base_dir"] = base_dir;
    return config.dump();
}

jstring error_result(JNIEnv* env, int code, const std::string& error) {
    nlohmann::json result;
    result["code"] = code;
    result["ok"] = 0;
    result["failed"] = 0;
    result["cancelled"] = false;
    result["error"] = error;
    return to_jstring(env, result.dump());
}

void bind_active_context(fc_context* ctx, bool* cancel_now) {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    g_active_context = ctx;
    *cancel_now = g_cancel_requested;
}

void release_active_context() {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    g_active_context = nullptr;
    g_cancel_requested = false;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        return JNI_ERR;
    }
    jclass codec = env->FindClass("com/formatconverter/app/ImageCodec");
    if (codec) {
        fc::android_bind_java(vm, codec);
        env->DeleteLocalRef(codec);
    } else {
        env->ExceptionClear();
    }
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_formatconverter_app_NativeCore_version(JNIEnv* env, jclass) {
    return to_jstring(env, fc_version());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_formatconverter_app_NativeCore_registryJson(JNIEnv* env, jclass) {
    return to_jstring(env, fc_registry_json());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_formatconverter_app_NativeCore_runJob(JNIEnv* env, jclass, jstring job_json,
                                               jstring base_dir) {
    const std::string job = jstring_to_utf8(env, job_json);
    const std::string base = jstring_to_utf8(env, base_dir);

    fc_context* ctx = fc_context_create(config_for(base).c_str());
    if (!ctx) {
        return error_result(env, 2, "无法初始化转换核心");
    }

    bool cancel_now = false;
    bind_active_context(ctx, &cancel_now);
    if (cancel_now) {
        fc_context_cancel(ctx);
    }

    CollectState state;
    char* error = nullptr;
    const int code = fc_run_job(ctx, job.c_str(), collect_callback, &state, &error);
    std::string error_text;
    if (error) {
        try {
            auto parsed = nlohmann::json::parse(error, nullptr, false);
            error_text = parsed.is_object() ? parsed.value("error", std::string(error)) : std::string(error);
        } catch (...) {
            error_text = error;
        }
        fc_free_string(error);
    }
    fc_context_destroy(ctx);
    release_active_context();

    std::string result = "{\"code\":" + std::to_string(code) + ",\"events\":[";
    for (size_t i = 0; i < state.events.size(); ++i) {
        if (i) {
            result.push_back(',');
        }
        result += state.events[i];
    }
    result += "],\"error\":";
    result += nlohmann::json(error_text).dump();
    result += "}";
    return to_jstring(env, result);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_formatconverter_app_NativeCore_runJobStreaming(JNIEnv* env, jclass, jstring job_json,
                                                        jstring base_dir, jobject listener) {
    const std::string job = jstring_to_utf8(env, job_json);
    const std::string base = jstring_to_utf8(env, base_dir);

    StreamState state;
    state.vm = g_vm;
    if (listener) {
        jclass listener_class = env->GetObjectClass(listener);
        if (listener_class) {
            state.on_event = env->GetMethodID(listener_class, "onEvent", "(Ljava/lang/String;)V");
            env->DeleteLocalRef(listener_class);
        }
        if (!state.on_event) {
            env->ExceptionClear();
            return error_result(env, 2, "事件监听器签名不正确");
        }
        // 回调来自 C++ 工作线程：必须用全局引用，局部引用只在创建它的线程/栈帧有效。
        state.listener = env->NewGlobalRef(listener);
        if (!state.listener) {
            return error_result(env, 2, "无法创建事件监听器引用");
        }
    } else {
        return error_result(env, 2, "缺少事件监听器");
    }

    fc_context* ctx = fc_context_create(config_for(base).c_str());
    if (!ctx) {
        env->DeleteGlobalRef(state.listener);
        return error_result(env, 2, "无法初始化转换核心");
    }

    bool cancel_now = false;
    bind_active_context(ctx, &cancel_now);
    if (cancel_now) {
        fc_context_cancel(ctx);
    }

    char* error = nullptr;
    const int code = fc_run_job(ctx, job.c_str(), stream_callback, &state, &error);
    std::string error_text;
    if (error) {
        try {
            auto parsed = nlohmann::json::parse(error, nullptr, false);
            error_text = parsed.is_object() ? parsed.value("error", std::string(error)) : std::string(error);
        } catch (...) {
            error_text = error;
        }
        fc_free_string(error);
    }
    fc_context_destroy(ctx);
    release_active_context();
    env->DeleteGlobalRef(state.listener);

    nlohmann::json result;
    result["code"] = code;
    result["ok"] = state.ok.load();
    result["failed"] = state.failed.load();
    result["cancelled"] = state.cancelled.load();
    result["error"] = error_text;
    return to_jstring(env, result.dump());
}

extern "C" JNIEXPORT void JNICALL
Java_com_formatconverter_app_NativeCore_cancel(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    g_cancel_requested = true;
    if (g_active_context) {
        fc_context_cancel(g_active_context);
    }
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_formatconverter_app_NativeCore_encodeMp3(JNIEnv* env, jclass, jstring pcm_path,
                                                  jint sample_rate, jint channels,
                                                  jint bitrate, jstring out_path) {
    try {
        const std::string pcm = jstring_to_utf8(env, pcm_path);
        const std::string out = jstring_to_utf8(env, out_path);
        std::string error;
        if (!fc::encode_pcm_file_to_mp3(fc::path_from_utf8(pcm), sample_rate, channels, bitrate,
                                        fc::path_from_utf8(out), error)) {
            return to_jstring(env, error);
        }
        return to_jstring(env, std::string());
    } catch (const std::exception& e) {
        return to_jstring(env, std::string("MP3 编码失败：") + e.what());
    }
}