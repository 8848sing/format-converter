#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fc {

namespace fs = std::filesystem;
using json = nlohmann::json;

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

struct EventSink {
    virtual ~EventSink() = default;
    virtual void file_ok(const std::string& input,
                         const std::vector<std::string>& outputs,
                         const std::string& message = {}) = 0;
    virtual void file_error(const std::string& input, const std::string& error) = 0;
    virtual void message(const std::string& text) { (void)text; }
};

struct Job {
    std::string op;
    std::vector<fs::path> inputs;
    fs::path out_dir;
    std::string target;
    json options = json::object();
    fs::path base_dir;
    json settings = json::object();

    std::string target_or(const std::string& fallback) const {
        return target.empty() ? fallback : target;
    }
    int option_int(const std::string& key, int fallback) const {
        if (options.contains(key) && options[key].is_number_integer()) {
            return options[key].get<int>();
        }
        return fallback;
    }
};

using Handler = std::function<void(const Job&, EventSink&)>;

struct ConversionSpec {
    std::string id;
    std::string label;
    std::string src;
    std::string dst;
    std::vector<std::string> extensions;
    Handler handler;
    std::vector<std::string> accepts;
    std::string accepts_label;
    bool per_file = true;
    bool batch_report = false;
    int workers = 1;
    std::string description;

    bool takes_target() const { return !accepts.empty(); }
};

/* ---------- 字符串与路径 ---------- */
std::string trim(const std::string& value);
std::string lower_ascii(std::string value);
bool iequals(const std::string& a, const std::string& b);
bool ends_with_ci(const std::string& value, const std::string& suffix);
std::string replace_all(std::string value, const std::string& from, const std::string& to);
std::string join_strings(const std::vector<std::string>& items, const std::string& sep);
std::vector<std::string> split_strings(const std::string& value, char sep);

#ifdef _WIN32
std::wstring to_wide(const std::string& utf8);
std::string to_utf8(const std::wstring& wide);
#endif

/* 把 JSON 字符串安全解析为路径；非法 UTF-8 或空字符串会抛 Error。 */
fs::path path_from_utf8(const std::string& utf8);
std::string path_to_utf8(const fs::path& path);

/* 输出一行事件所需的 JSON 转义字符串（不带引号）。 */
std::string json_escape(const std::string& value);

std::string utf16le_to_utf8(const std::vector<uint16_t>& units);

}  // namespace fc