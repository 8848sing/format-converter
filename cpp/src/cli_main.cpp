#include "fc/core.h"
#include "fc/version.h"

#include "common.h"
#include "fs_utils.h"
#include "image.h"
#include "office.h"
#include "media.h"

#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = fc::fs;

namespace {

std::vector<std::string> collect_args(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int wide_count = 0;
    LPWSTR* wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (!wide_args) {
        return args;
    }
    args.reserve(static_cast<size_t>(wide_count));
    for (int i = 0; i < wide_count; ++i) {
        args.push_back(fc::to_utf8(wide_args[i]));
    }
    LocalFree(wide_args);
#else
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
#endif
    return args;
}

void print_usage() {
    std::cout
        << FC_APP_NAME << " " << FC_VERSION << " C++ 核心命令行\n\n"
        << "用法:\n"
        << "  fccli --version\n"
        << "  fccli list\n"
        << "  fccli convert --job job.json\n"
        << "  fccli convert --job-line -      （从标准输入读一行 JSON，之后可写 #cancel 取消）\n"
        << "  fccli convert --op image_convert --out DIR --target webp file1.png file2.png\n"
        << "  fccli self-test\n\n"
        << "convert 选项:\n"
        << "  --job <file|->    从 JSON 文件或标准输入读取任务\n"
        << "  --op <id>         转换类型 ID（见 fccli list）\n"
        << "  --out <dir>       输出目录\n"
        << "  --target <fmt>    目标格式\n"
        << "  --base-dir <dir>  便携包根目录（tools/ 所在目录）\n"
        << "  --option k=v      额外选项，可重复\n";
}

std::string read_stdin() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return buffer.str();
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw fc::Error("无法读取任务文件 " + fc::path_to_utf8(path));
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

int run_job_json(fc_context* ctx, const std::string& job_json, bool print_events) {
    auto callback = [](const char* event_json, void* user) {
        bool should_print = *static_cast<bool*>(user);
        if (should_print) {
            std::cout << event_json << "\n";
            std::cout.flush();
        }
    };
    char* error = nullptr;
    int code = fc_run_job(ctx, job_json.c_str(), callback, &print_events, &error);
    if (error) {
        std::cerr << error << "\n";
        fc_free_string(error);
    }
    return code;
}

fc::json build_job_json(const std::string& op, const std::string& out_dir,
                        const std::string& target, const std::vector<std::string>& files,
                        const std::vector<std::string>& option_pairs) {
    fc::json job;
    job["op"] = op;
    job["inputs"] = files;
    job["out_dir"] = out_dir;
    if (!target.empty()) {
        job["target"] = target;
    }
    fc::json options = fc::json::object();
    for (const std::string& pair : option_pairs) {
        size_t pos = pair.find('=');
        if (pos == std::string::npos || pos == 0) {
            continue;
        }
        std::string key = pair.substr(0, pos);
        std::string value = pair.substr(pos + 1);
        if (value == "true" || value == "false") {
            options[key] = value == "true";
        } else {
            char* end = nullptr;
            long long number = std::strtoll(value.c_str(), &end, 10);
            if (end && *end == '\0' && !value.empty()) {
                options[key] = number;
            } else {
                options[key] = value;
            }
        }
    }
    if (!options.empty()) {
        job["options"] = options;
    }
    return job;
}

struct SelfTest {
    fc::json checks = fc::json::array();
    bool ok = true;

    void add(const std::string& name, bool passed, const std::string& error = {}) {
        fc::json item;
        item["name"] = name;
        item["ok"] = passed;
        item["error"] = passed ? "" : (error.empty() ? "empty output" : error);
        checks.push_back(item);
        ok = ok && passed;
    }
};

bool output_exists(const fc::json& events, const std::string& type) {
    for (const auto& event : events) {
        if (event.contains("type") && event["type"] == type) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> args = collect_args(argc, argv);
    if (!args.empty()) {
        args.erase(args.begin());
    }
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return 0;
    }
    if (args[0] == "--version" || args[0] == "version") {
        std::cout << FC_VERSION_TEXT << "\n";
        return 0;
    }
    if (args[0] == "list") {
        std::cout << fc_registry_json() << "\n";
        return 0;
    }

    fc::fs::path base_dir = fc::default_base_dir();
    std::string base_dir_option;
    std::string job_file;
    std::string job_line_file;
    std::string op;
    std::string out_dir;
    std::string target;
    std::vector<std::string> files;
    std::vector<std::string> option_pairs;
    bool print_events = true;

    size_t index = 1;
    if (args[0] == "convert") {
        while (index < args.size()) {
            const std::string& arg = args[index];
            if (arg == "--job" && index + 1 < args.size()) {
                job_file = args[++index];
            } else if (arg == "--job-line" && index + 1 < args.size()) {
                job_line_file = args[++index];
            } else if (arg == "--op" && index + 1 < args.size()) {
                op = args[++index];
            } else if (arg == "--out" && index + 1 < args.size()) {
                out_dir = args[++index];
            } else if (arg == "--target" && index + 1 < args.size()) {
                target = args[++index];
            } else if (arg == "--base-dir" && index + 1 < args.size()) {
                base_dir_option = args[++index];
            } else if (arg == "--option" && index + 1 < args.size()) {
                option_pairs.push_back(args[++index]);
            } else if (arg == "--quiet") {
                print_events = false;
            } else if (arg.rfind("--", 0) == 0) {
                std::cerr << "未知选项: " << arg << "\n";
                return 2;
            } else {
                files.push_back(arg);
            }
            ++index;
        }
    } else if (args[0] == "self-test") {
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--base-dir" && i + 1 < args.size()) {
                base_dir_option = args[++i];
            }
        }
    } else {
        std::cerr << "未知命令: " << args[0] << "\n";
        print_usage();
        return 2;
    }

    if (!base_dir_option.empty()) {
        base_dir = fc::path_from_utf8(base_dir_option);
    }
    fc::json config;
    config["base_dir"] = fc::path_to_utf8(base_dir);
    fc_context* ctx = fc_context_create(config.dump().c_str());
    if (!ctx) {
        std::cerr << "{\"error\":\"无法初始化转换核心\"}\n";
        return 2;
    }

    int exit_code = 0;
    if (args[0] == "self-test") {
        try {
            fc::fs::path temp = fc::temp_root(base_dir);
            fc::TempDir work(temp, "fc_selftest_");
            fc::fs::path inputs = work.path() / "inputs";
            fc::fs::path outputs = work.path() / "outputs";
            fc::ensure_directory(inputs);
            fc::ensure_directory(outputs);

            fc::Image image;
            image.width = 320;
            image.height = 180;
            image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 255);
            for (size_t p = 0; p < image.rgba.size(); p += 4) {
                image.rgba[p] = 57;
                image.rgba[p + 1] = 117;
                image.rgba[p + 2] = 255;
            }
            fc::fs::path png = inputs / "sample.png";
            fc::save_image(image, png, "png");

            SelfTest test;
            auto run = [&](const std::string& job_op, const std::vector<std::string>& in,
                           const std::string& out, const std::string& fmt,
                           std::vector<fc::json>* events = nullptr) {
                fc::json job = build_job_json(job_op, out, fmt, in, {});
                std::string text = job.dump();
                auto callback = [](const char* event_json, void* user) {
                    auto* list = static_cast<std::vector<fc::json>*>(user);
                    if (list) {
                        try {
                            list->push_back(fc::json::parse(event_json));
                        } catch (...) {
                        }
                    }
                };
                char* error = nullptr;
                int code = fc_run_job(ctx, text.c_str(), callback, events, &error);
                if (error) {
                    fc_free_string(error);
                }
                return code;
            };

            std::vector<fc::json> events;
            int code = run("image_convert", {fc::path_to_utf8(png)}, fc::path_to_utf8(outputs), "jpg", &events);
            test.add("image-jpg", code == 0 && output_exists(events, "file_ok"));
            test.add("image-webp", run("image_convert", {fc::path_to_utf8(png)}, fc::path_to_utf8(outputs), "webp", nullptr) == 0);

            events.clear();
            code = run("image_to_pdf", {fc::path_to_utf8(png)}, fc::path_to_utf8(outputs), "", &events);
            test.add("image-to-pdf", code == 0 && output_exists(events, "file_ok"));

            events.clear();
            code = run("pdf_to_image", {fc::path_to_utf8(outputs / "sample.pdf")}, fc::path_to_utf8(outputs), "png", &events);
            test.add("pdf-to-image", code == 0 && output_exists(events, "file_ok"));

            events.clear();
            code = run("pdf_merge", {fc::path_to_utf8(outputs / "sample.pdf"), fc::path_to_utf8(outputs / "sample.pdf")},
                       fc::path_to_utf8(outputs), "", &events);
            test.add("pdf-merge", code == 0 && output_exists(events, "file_ok"));

            fc::json data_in = fc::json::array();
            data_in.push_back({{"name", "A"}, {"value", 1}});
            data_in.push_back({{"name", "B"}, {"value", 2}});
            fc::fs::path json_file = inputs / "data.json";
            fc::write_text_file(json_file, data_in.dump());
            test.add("data-yaml", run("data_convert", {fc::path_to_utf8(json_file)}, fc::path_to_utf8(outputs), "yaml") == 0);
            test.add("data-xlsx", run("data_convert", {fc::path_to_utf8(json_file)}, fc::path_to_utf8(outputs), "xlsx") == 0);

            fc::Job probe;
            probe.base_dir = base_dir;
            probe.settings = fc::json::object();
            try {
                fc::find_soffice(probe);
                test.add("libreoffice-found", true);
            } catch (const std::exception& e) {
                test.add("libreoffice-found", false, e.what());
            }
            try {
                fc::find_ffmpeg(probe);
                test.add("ffmpeg-found", true);
            } catch (const std::exception& e) {
                test.add("ffmpeg-found", false, e.what());
            }

            fc::json result;
            result["ok"] = test.ok;
            result["checks"] = test.checks;
            fc::write_text_file(base_dir / "self-test-result.json", result.dump(2));
            std::cout << result.dump(2) << "\n";
            exit_code = test.ok ? 0 : 1;
        } catch (const std::exception& e) {
            std::cerr << "{\"error\":\"" << fc::json_escape(e.what()) << "\"}\n";
            exit_code = 2;
        }
    } else {
        std::string job_json;
        if (!job_line_file.empty()) {
            try {
                if (job_line_file == "-") {
                    if (!std::getline(std::cin, job_json)) {
                        throw fc::Error("标准输入没有任务内容");
                    }
                    while (!job_json.empty() &&
                           (job_json.back() == '\r' || job_json.back() == '\n')) {
                        job_json.pop_back();
                    }
                    if (job_json.empty()) {
                        throw fc::Error("标准输入的任务为空");
                    }
                    // 任务读入后继续监听标准输入：收到 cancel 就请求在文件之间停止。
                    // 桌面端因此可以在整批任务进行中取消，而不是等当前文件结束后
                    // 直接杀掉进程（强杀会留下未重命名的 .part 临时文件）。
                    std::thread watcher([ctx]() {
                        std::string line;
                        while (std::getline(std::cin, line)) {
                            if (line.find("cancel") != std::string::npos) {
                                fc_context_cancel(ctx);
                            }
                        }
                    });
                    watcher.detach();
                } else {
                    job_json = read_file(fc::path_from_utf8(job_line_file));
                }
            } catch (const std::exception& e) {
                std::cerr << "{\"error\":\"" << fc::json_escape(e.what()) << "\"}\n";
                fc_context_destroy(ctx);
                return 2;
            }
        } else if (!job_file.empty()) {
            try {
                job_json = job_file == "-" ? read_stdin() : read_file(fc::path_from_utf8(job_file));
            } catch (const std::exception& e) {
                std::cerr << "{\"error\":\"" << fc::json_escape(e.what()) << "\"}\n";
                fc_context_destroy(ctx);
                return 2;
            }
        } else {
            if (op.empty()) {
                std::cerr << "{\"error\":\"缺少 --op 或 --job\"}\n";
                fc_context_destroy(ctx);
                return 2;
            }
            if (out_dir.empty()) {
                std::cerr << "{\"error\":\"缺少 --out\"}\n";
                fc_context_destroy(ctx);
                return 2;
            }
            fc::json job = build_job_json(op, out_dir, target, files, option_pairs);
            job["base_dir"] = fc::path_to_utf8(base_dir);
            job_json = job.dump();
        }
        exit_code = run_job_json(ctx, job_json, print_events);
    }

    fc_context_destroy(ctx);
    return exit_code;
}