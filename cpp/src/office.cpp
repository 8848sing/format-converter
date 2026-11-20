#include "office.h"

#include "fs_utils.h"
#include "image.h"
#include "pdf_ops.h"
#include "process.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_set>

namespace fc {

namespace {

std::string path_to_file_uri(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::string generic = path_to_utf8(absolute);
    std::replace(generic.begin(), generic.end(), '\\', '/');
    if (generic.empty() || generic.front() != '/') {
        generic.insert(generic.begin(), '/');
    }
    std::string uri = "file://";
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char ch : generic) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == ':' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            uri.push_back(static_cast<char>(ch));
        } else {
            uri.push_back('%');
            uri.push_back(hex[ch >> 4]);
            uri.push_back(hex[ch & 0x0F]);
        }
    }
    return uri;
}

std::string last_error_line(const ProcessResult& result) {
    std::string detail = trim(result.err.empty() ? result.out : result.err);
    if (detail.empty()) {
        return "soffice 退出码 " + std::to_string(result.exit_code) + "（未生成有效输出）";
    }
    std::istringstream stream(detail);
    std::string line;
    std::string last;
    while (std::getline(stream, line)) {
        if (!trim(line).empty()) {
            last = trim(line);
        }
    }
    return last.empty() ? detail : last;
}

std::vector<std::vector<fs::path>> plan_batches(const std::vector<fs::path>& files,
                                                size_t max_chars = 6000) {
    std::vector<std::vector<fs::path>> batches;
    std::vector<fs::path> current;
    std::unordered_set<std::string> stems;
    size_t length = 0;
    for (const fs::path& file : files) {
        std::string key = lower_ascii(stem_utf8(file));
        size_t estimate = path_to_utf8(file).size() + 3;
        if ((!stems.empty() && stems.count(key)) ||
            (length + estimate > max_chars && !current.empty())) {
            batches.push_back(current);
            current.clear();
            stems.clear();
            length = 0;
        }
        current.push_back(file);
        stems.insert(key);
        length += estimate;
    }
    if (!current.empty()) {
        batches.push_back(current);
    }
    return batches;
}

fs::path convert_one(const fs::path& soffice, const fs::path& input, const fs::path& out_dir,
                     const std::string& filter, const std::string& out_ext, int timeout_seconds,
                     const fs::path& temp_root_dir) {
    TempDir profile(temp_root_dir, "lo_profile_");
    TempDir output(temp_root_dir, "lo_out_");
    std::vector<std::string> args = {
        path_to_utf8(soffice),
        "-env:UserInstallation=" + path_to_file_uri(profile.path()),
        "--headless",
        "--norestore",
        "--invisible",
        "--convert-to",
        filter,
        "--outdir",
        path_to_utf8(output.path()),
        path_to_utf8(input),
    };
    ProcessResult result = run_process(args, timeout_seconds * 1000);
    fs::path produced = path_join_utf8(output.path(), stem_utf8(input) + out_ext);
    if (!file_exists(produced) || file_size_bytes(produced) == 0) {
        throw Error(last_error_line(result));
    }
    fs::path out = unique_output(out_dir, stem_utf8(input), out_ext);
    replace_file(produced, out);
    return out;
}

class CollectSink : public EventSink {
public:
    std::vector<std::string> outputs;
    bool failed = false;
    std::string error;

    void file_ok(const std::string& input, const std::vector<std::string>& files,
                 const std::string& message) override {
        (void)input;
        (void)message;
        outputs.insert(outputs.end(), files.begin(), files.end());
    }
    void file_error(const std::string& input, const std::string& message) override {
        (void)input;
        failed = true;
        error = message;
    }
};

}  // namespace

fs::path find_soffice(const Job& job) {
    fs::path base = job.base_dir;
    json settings = job.settings;
    if (settings.contains("soffice") && settings["soffice"].is_string()) {
        std::string value = settings["soffice"].get<std::string>();
        if (!value.empty()) {
            fs::path configured = path_from_utf8(value);
            if (configured.is_relative()) {
                configured = base / configured;
            }
            if (file_exists(configured)) {
                return configured;
            }
        }
    }

    std::vector<fs::path> candidates = {
        base / "tools" / "LibreOffice" / "program" / "soffice.exe",
        base / "tools" / "LibreOfficePortable" / "App" / "libreoffice" / "program" / "soffice.exe",
        base / "tools" / "LibreOfficePortable" / "App" / "LibreOffice" / "program" / "soffice.exe",
        base / "tools" / "soffice.exe",
        base / "tools" / "soffice",
    };
    std::error_code ec;
    fs::path tools = base / "tools";
    if (fs::is_directory(tools, ec)) {
        fs::recursive_directory_iterator it(tools, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) {
                continue;
            }
            std::string name = path_to_utf8(it->path().filename());
            std::string full = lower_ascii(path_to_utf8(it->path()));
            if ((iequals(name, "soffice.exe") || iequals(name, "soffice")) &&
                full.find("libreoffice") != std::string::npos) {
                candidates.push_back(it->path());
            }
        }
    }
    candidates.push_back(path_from_utf8("C:\\Program Files\\LibreOffice\\program\\soffice.exe"));
    candidates.push_back(path_from_utf8("C:\\Program Files (x86)\\LibreOffice\\program\\soffice.exe"));
    fs::path found = find_executable("soffice", candidates);
    if (!found.empty()) {
        return found;
    }
    throw Error(
        "未找到 LibreOffice（soffice.exe）。请将其便携版解压到 tools/ 目录，"
        "或在 settings.json 中写入 {\"soffice\": \"绝对路径\"}。");
}

bool has_soffice(const Job& job) {
    try {
        find_soffice(job);
        return true;
    } catch (...) {
        return false;
    }
}

void office_export_job(const Job& job, EventSink& sink, const std::string& filter,
                       const std::string& out_ext) {
    if (job.inputs.empty()) {
        throw Error("没有可转换的文件");
    }
    fs::path soffice;
    try {
        soffice = find_soffice(job);
    } catch (const std::exception& e) {
        for (const fs::path& input : job.inputs) {
            sink.file_error(path_to_utf8(input), e.what());
        }
        return;
    }

    fs::path temp = temp_root(job.base_dir);
    std::vector<fs::path> pending;
    for (const auto& batch : plan_batches(job.inputs)) {
        try {
            TempDir profile(temp, "lo_profile_");
            TempDir output(temp, "lo_out_");
            std::vector<std::string> args = {
                path_to_utf8(soffice),
                "-env:UserInstallation=" + path_to_file_uri(profile.path()),
                "--headless",
                "--norestore",
                "--invisible",
                "--convert-to",
                filter,
                "--outdir",
                path_to_utf8(output.path()),
            };
            for (const fs::path& file : batch) {
                args.push_back(path_to_utf8(file));
            }
            // 单文件 120 秒、每多一个文件加 90 秒，上限 900 秒。
            // 之前 180+90n 的余量太大，soffice 卡死时用户要等好几分钟才看到错误。
            int timeout = std::min(900, 120 + 90 * static_cast<int>(batch.size()));
            ProcessResult result = run_process(args, timeout * 1000);
            if (!result.launched) {
                pending.insert(pending.end(), batch.begin(), batch.end());
                continue;
            }
            for (const fs::path& file : batch) {
                fs::path produced = path_join_utf8(output.path(), stem_utf8(file) + out_ext);
                if (!file_exists(produced) || file_size_bytes(produced) == 0) {
                    pending.push_back(file);
                    continue;
                }
                try {
                    fs::path out = unique_output(job.out_dir, stem_utf8(file), out_ext);
                    replace_file(produced, out);
                    sink.file_ok(path_to_utf8(file), {path_to_utf8(out)});
                } catch (const std::exception& e) {
                    sink.file_error(path_to_utf8(file), e.what());
                }
            }
        } catch (const std::exception& e) {
            for (const fs::path& file : batch) {
                sink.file_error(path_to_utf8(file), e.what());
            }
        }
    }

    for (const fs::path& file : pending) {
        try {
            fs::path out = convert_one(soffice, file, job.out_dir, filter, out_ext, 120, temp);
            sink.file_ok(path_to_utf8(file), {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(path_to_utf8(file), std::string("转换失败: ") + e.what());
        }
    }
}

void office_to_images(const Job& job, EventSink& sink) {
    std::string target = normalize_image_format(job.target_or("png"));
    fs::path soffice;
    try {
        soffice = find_soffice(job);
    } catch (const std::exception& e) {
        for (const fs::path& input : job.inputs) {
            sink.file_error(path_to_utf8(input), e.what());
        }
        return;
    }
    fs::path temp = temp_root(job.base_dir);
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            TempDir work(temp, "o2i_");
            fs::path pdf = convert_one(soffice, input, work.path(), "pdf", ".pdf", 120, temp);
            Job render;
            render.op = "pdf_to_image";
            render.inputs = {pdf};
            render.out_dir = job.out_dir;
            render.target = target;
            render.options = job.options;
            // 允许界面按需覆盖渲染 DPI；保持旧默认 200。
            render.options["dpi"] = job.option_int("dpi", 200);
            render.base_dir = job.base_dir;
            render.settings = job.settings;
            CollectSink collector;
            pdf_to_images(render, collector);
            if (collector.failed) {
                throw Error(collector.error);
            }
            sink.file_ok(input_text, collector.outputs);
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

}  // namespace fc
