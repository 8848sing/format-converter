#include "fc/core.h"
#include "fc/version.h"

#include "common.h"
#include "engine.h"
#include "fs_utils.h"
#include "registry.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

struct fc_context {
    fc::fs::path base_dir;
    fc::json settings = fc::json::object();
    std::atomic<bool> cancel{false};
};

namespace {

fc::json load_settings_file(const fc::fs::path& base_dir) {
    std::error_code ec;
    fc::fs::path file = base_dir / "settings.json";
    if (!fc::fs::is_regular_file(file, ec)) {
        return fc::json::object();
    }
    try {
        std::string text = fc::read_text_auto(file);
        fc::json value = fc::json::parse(text);
        return value.is_object() ? value : fc::json::object();
    } catch (...) {
        return fc::json::object();
    }
}

char* duplicate_string(const std::string& value) {
    char* buffer = static_cast<char*>(std::malloc(value.size() + 1));
    if (!buffer) {
        return nullptr;
    }
    std::memcpy(buffer, value.data(), value.size());
    buffer[value.size()] = '\0';
    return buffer;
}

std::string json_string(const fc::json& value, const char* key, const std::string& fallback = {}) {
    if (value.contains(key) && value[key].is_string()) {
        return value[key].get<std::string>();
    }
    return fallback;
}

class JsonEventSink : public fc::EventSink {
public:
    JsonEventSink(fc_event_callback callback, void* user_data)
        : callback_(callback), user_(user_data) {}

    void file_ok(const std::string& input, const std::vector<std::string>& outputs,
                 const std::string& message) override {
        fc::json event;
        event["type"] = "file_ok";
        event["input"] = input;
        event["outputs"] = outputs;
        if (!message.empty()) {
            event["message"] = message;
        }
        emit(event);
    }

    void file_error(const std::string& input, const std::string& error) override {
        fc::json event;
        event["type"] = "file_error";
        event["input"] = input;
        event["error"] = error;
        emit(event);
    }

    void message(const std::string& text) override {
        fc::json event;
        event["type"] = "message";
        event["message"] = text;
        emit(event);
    }

    void emit(const fc::json& event) {
        if (!callback_) {
            return;
        }
        std::string line = event.dump(-1, ' ', false, fc::json::error_handler_t::replace);
        std::lock_guard<std::mutex> lock(mutex_);
        callback_(line.c_str(), user_);
    }

private:
    fc_event_callback callback_ = nullptr;
    void* user_ = nullptr;
    std::mutex mutex_;
};

fc::Job parse_job(const fc::json& value, const fc::fs::path& base_dir, const fc::json& settings) {
    fc::Job job;
    if (!value.is_object()) {
        throw fc::Error("任务必须是 JSON 对象");
    }
    job.op = json_string(value, "op");
    if (job.op.empty()) {
        throw fc::Error("任务缺少 op 字段");
    }
    if (!value.contains("inputs") || !value["inputs"].is_array()) {
        throw fc::Error("任务缺少 inputs 数组");
    }
    for (const auto& item : value["inputs"]) {
        if (!item.is_string()) {
            throw fc::Error("inputs 中的路径必须是字符串");
        }
        job.inputs.push_back(fc::path_from_utf8(item.get<std::string>()));
    }
    job.out_dir = fc::path_from_utf8(json_string(value, "out_dir"));
    if (job.out_dir.empty()) {
        throw fc::Error("任务缺少 out_dir");
    }
    job.target = json_string(value, "target");
    if (value.contains("options") && value["options"].is_object()) {
        job.options = value["options"];
    }
    job.base_dir = base_dir;
    job.settings = settings;
    return job;
}

}  // namespace

extern "C" {

const char* fc_version(void) {
    return FC_VERSION_TEXT;
}

const char* fc_registry_json(void) {
    static std::once_flag once;
    static std::string cached;
    std::call_once(once, [] { cached = fc::registry_json(); });
    return cached.c_str();
}

fc_context* fc_context_create(const char* config_json) {
    auto* ctx = new (std::nothrow) fc_context();
    if (!ctx) {
        return nullptr;
    }
    try {
        ctx->base_dir = fc::default_base_dir();
        bool settings_provided = false;
        if (config_json && *config_json) {
            fc::json config = fc::json::parse(config_json);
            if (config.is_object()) {
                if (config.contains("base_dir") && config["base_dir"].is_string()) {
                    ctx->base_dir = fc::path_from_utf8(config["base_dir"].get<std::string>());
                }
                if (config.contains("settings") && config["settings"].is_object()) {
                    ctx->settings = config["settings"];
                    settings_provided = true;
                }
            }
        }
        if (!settings_provided) {
            ctx->settings = load_settings_file(ctx->base_dir);
        }
        return ctx;
    } catch (...) {
        delete ctx;
        return nullptr;
    }
}

void fc_context_destroy(fc_context* ctx) {
    delete ctx;
}

void fc_context_cancel(fc_context* ctx) {
    if (ctx) {
        ctx->cancel.store(true);
    }
}

int fc_run_job(fc_context* ctx, const char* job_json,
               fc_event_callback callback, void* user_data,
               char** error_json) {
    if (error_json) {
        *error_json = nullptr;
    }
    if (!ctx || !job_json) {
        if (error_json) {
            *error_json = duplicate_string("{\"error\":\"上下文或任务为空\"}");
        }
        return 2;
    }
    try {
        fc::json value = fc::json::parse(job_json);
        fc::Job job = parse_job(value, ctx->base_dir, ctx->settings);
        fc::Engine engine(ctx->base_dir, ctx->settings);
        JsonEventSink sink(callback, user_data);
        fc::RunResult result = engine.run(job, sink, &ctx->cancel);

        fc::json done;
        done["type"] = "done";
        done["ok"] = result.ok;
        done["failed"] = result.failed;
        done["cancelled"] = result.cancelled;
        sink.emit(done);
        return result.failed == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        if (error_json) {
            fc::json error;
            error["error"] = e.what();
            *error_json = duplicate_string(error.dump());
        }
        return 2;
    }
}

void fc_free_string(char* text) {
    std::free(text);
}

}  // extern "C"