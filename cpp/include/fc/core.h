#pragma once

/*
 * 格式转换工具 C++ 核心的稳定 C API。
 *
 * 设计目标：
 *  - Python/Tkinter 桌面界面通过 fccli 子进程使用同一套核心；
 *  - Android 通过 JNI 调用 fccore，不需要依赖 Python；
 *  - 所有跨边界字符串均为 UTF-8 JSON。
 *
 * 典型调用：
 *   fc_context* ctx = fc_context_create(nullptr);
 *   fc_run_job(ctx, "{\"op\":\"image_convert\",...}", on_event, user, &err);
 *   fc_context_destroy(ctx);
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(FC_CORE_EXPORTS)
#define FC_API __declspec(dllexport)
#elif defined(FC_CORE_STATIC)
#define FC_API
#elif defined(_WIN32)
#define FC_API __declspec(dllimport)
#else
#define FC_API __attribute__((visibility("default")))
#endif

typedef struct fc_context fc_context;

/* 事件回调：event_json 为一行 UTF-8 JSON，回调期间有效。 */
typedef void (*fc_event_callback)(const char* event_json, void* user_data);

/* 返回静态字符串，不需要释放。 */
FC_API const char* fc_version(void);

/* 返回转换注册表 JSON（数组）。不需要释放。 */
FC_API const char* fc_registry_json(void);

/* config_json 可为 NULL；支持 {"base_dir":"...","settings":{...}}。 */
FC_API fc_context* fc_context_create(const char* config_json);

FC_API void fc_context_destroy(fc_context* ctx);

/* 请求取消尚未开始的任务；正在运行的外部进程不会被强制结束。 */
FC_API void fc_context_cancel(fc_context* ctx);

/*
 * 同步执行一个任务。返回 0 表示全部成功，1 表示有文件失败，2 表示参数/环境错误。
 * 失败时 *error_json 指向需要 fc_free_string 释放的 JSON 字符串；成功时置为 NULL。
 */
FC_API int fc_run_job(fc_context* ctx, const char* job_json,
                      fc_event_callback callback, void* user_data,
                      char** error_json);

FC_API void fc_free_string(char* text);

#ifdef __cplusplus
}
#endif