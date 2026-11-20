#pragma once

#include "common.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fc {

fs::path default_base_dir();
fs::path temp_root(const fs::path& base_dir);
void ensure_directory(const fs::path& dir);
bool is_directory_writable(const fs::path& dir);
bool file_exists(const fs::path& path);
uint64_t file_size_bytes(const fs::path& path);

std::string extension_lower(const fs::path& path);
std::string stem_utf8(const fs::path& path);

std::vector<uint8_t> read_binary_file(const fs::path& path);
void write_binary_file(const fs::path& path, const void* data, size_t size);
void write_binary_file(const fs::path& path, const std::vector<uint8_t>& data);
void write_text_file(const fs::path& path, const std::string& text);

/* 读取文本：UTF-8 BOM、UTF-8、Windows 上回退 GB18030，最后做替换解码。 */
std::string read_text_auto(const fs::path& path);

/* 在目标目录中分配不冲突的输出名；并行任务安全。 */
class OutputAllocator {
public:
    OutputAllocator() = default;
    ~OutputAllocator();
    fs::path claim(const fs::path& dir, const std::string& stem, const std::string& ext);
    void release(const fs::path& path);
    static OutputAllocator& shared();

private:
    static std::string key_for(const fs::path& path);
    std::mutex mutex_;
    std::unordered_map<std::string, fs::path> claimed_;
};

fs::path unique_output(const fs::path& dir, const std::string& stem, const std::string& ext);

/* 用 UTF-8 文件名拼接路径，避免 Windows 上窄字符串按 ANSI 解码。 */
fs::path path_join_utf8(const fs::path& dir, const std::string& name);
fs::path append_suffix_utf8(const fs::path& path, const std::string& suffix);

/* 先写 .part 再改名，避免留下半成品。 */
void replace_file(const fs::path& from, const fs::path& to);
void remove_file_quiet(const fs::path& path);

/* 在 PATH 与候选路径中查找可执行文件。 */
fs::path find_executable(const std::string& name, const std::vector<fs::path>& candidates);

class TempDir {
public:
    explicit TempDir(const fs::path& root, const std::string& prefix);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

bool is_valid_utf8(const std::string& value);

}  // namespace fc