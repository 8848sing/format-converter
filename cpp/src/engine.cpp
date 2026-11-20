#include "engine.h"

#include "fs_utils.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <thread>

namespace fc {

namespace {

class CountingSink : public EventSink {
public:
    explicit CountingSink(EventSink& inner) : inner_(inner) {}

    void file_ok(const std::string& input, const std::vector<std::string>& outputs,
                 const std::string& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++ok_;
        seen_.insert(input);
        inner_.file_ok(input, outputs, message);
    }

    void file_error(const std::string& input, const std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++failed_;
        seen_.insert(input);
        inner_.file_error(input, error);
    }

    void message(const std::string& text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        inner_.message(text);
    }

    bool seen(const std::string& input) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return seen_.count(input) > 0;
    }

    int ok() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ok_;
    }
    int failed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }

private:
    EventSink& inner_;
    mutable std::mutex mutex_;
    int ok_ = 0;
    int failed_ = 0;
    std::set<std::string> seen_;
};

class TaskSink : public EventSink {
public:
    explicit TaskSink(EventSink& parent) : parent_(parent) {}

    void file_ok(const std::string& input, const std::vector<std::string>& outputs,
                 const std::string& message = {}) override {
        emitted_ = true;
        parent_.file_ok(input, outputs, message);
    }
    void file_error(const std::string& input, const std::string& error) override {
        emitted_ = true;
        parent_.file_error(input, error);
    }
    void message(const std::string& text) override { parent_.message(text); }

    bool emitted() const { return emitted_; }

private:
    EventSink& parent_;
    bool emitted_ = false;
};

bool has_extension(const std::vector<std::string>& extensions, const fs::path& path) {
    std::string ext = extension_lower(path);
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

}  // namespace

Engine::Engine(fs::path base_dir, json settings)
    : base_dir_(std::move(base_dir)), settings_(std::move(settings)) {}

RunResult Engine::run(const Job& job, EventSink& sink, std::atomic<bool>* cancel) const {
    RunResult result;
    const ConversionSpec* spec = find_conversion(job.op);
    if (!spec) {
        throw Error("未知转换类型: " + job.op);
    }
    if (job.inputs.empty()) {
        throw Error("没有输入文件");
    }
    fs::path out_dir = job.out_dir;
    if (out_dir.empty()) {
        throw Error("未指定输出目录");
    }
    ensure_directory(out_dir);
    if (!is_directory_writable(out_dir)) {
        throw Error("输出目录不可写: " + path_to_utf8(out_dir));
    }

    Job work = job;
    if (work.base_dir.empty()) {
        work.base_dir = base_dir_;
    }
    if (work.settings.empty()) {
        work.settings = settings_;
    }
    work.out_dir = out_dir;

    CountingSink counting(sink);
    std::vector<fs::path> inputs;
    inputs.reserve(job.inputs.size());
    for (const fs::path& input : job.inputs) {
        if (!file_exists(input)) {
            counting.file_error(path_to_utf8(input), "文件不存在");
            continue;
        }
        if (!has_extension(spec->extensions, input)) {
            counting.file_error(path_to_utf8(input), "文件扩展名与当前转换方向不匹配");
            continue;
        }
        inputs.push_back(input);
    }
    if (inputs.empty()) {
        result.ok = counting.ok();
        result.failed = counting.failed();
        return result;
    }

    if (!spec->per_file) {
        Job batch = work;
        batch.inputs = inputs;
        try {
            spec->handler(batch, counting);
        } catch (const std::exception& e) {
            for (const fs::path& input : inputs) {
                std::string key = path_to_utf8(input);
                if (!counting.seen(key)) {
                    counting.file_error(key, e.what());
                }
            }
        }
    } else {
        int workers = std::max(1, spec->workers);
        // 界面可以把「同时处理文件数」传进来；0 或不传表示按硬件自动决定。
        int requested = job.option_int("workers", 0);
        if (requested > 0) {
            workers = std::max(1, std::min(16, requested));
        } else if (spec->src == CAT_IMG) {
            // 图片解码/编码是纯 CPU 活，按逻辑线程数铺满；本机 8 核 12 线程实测
            // 12 路比旧的 8 路快约 10%。上限 16，避免超大图批量时内存峰值过高。
            unsigned hardware = std::thread::hardware_concurrency();
            int logical = static_cast<int>(hardware == 0 ? 4 : hardware);
            workers = std::max(workers, std::min(16, logical));
        }
        workers = std::min(workers, static_cast<int>(inputs.size()));
#ifdef __ANDROID__
        // 移动端内存有限：限制并发解码数量，避免多张大图同时解码导致 OOM。
        workers = std::min(workers, 3);
#endif
        std::atomic<size_t> next{0};
        std::atomic<bool> cancelled{false};
        auto worker = [&]() {
            while (true) {
                if (cancel && cancel->load()) {
                    cancelled.store(true);
                    return;
                }
                size_t index = next.fetch_add(1);
                if (index >= inputs.size()) {
                    return;
                }
                const fs::path& input = inputs[index];
                Job one = work;
                one.inputs = {input};
                TaskSink task(counting);
                try {
                    spec->handler(one, task);
                } catch (const std::exception& e) {
                    task.file_error(path_to_utf8(input), e.what());
                }
                if (!task.emitted()) {
                    task.file_ok(path_to_utf8(input), {});
                }
            }
        };
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(workers));
        for (int i = 0; i < workers; ++i) {
            threads.emplace_back(worker);
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        result.cancelled = cancelled.load();
    }

    result.ok = counting.ok();
    result.failed = counting.failed();
    return result;
}

}  // namespace fc