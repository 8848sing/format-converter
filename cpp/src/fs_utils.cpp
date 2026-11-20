#include "fs_utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fc {

fs::path default_base_dir() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        fs::path exe_path(buffer.data(), buffer.data() + length);
        fs::path original = exe_path.parent_path();
        fs::path dir = original;
        std::error_code ec;
        for (int depth = 0; depth < 4; ++depth) {
            if (fs::is_directory(dir / "tools", ec)) {
                return dir;
            }
            fs::path parent = dir.parent_path();
            if (parent.empty() || parent == dir) {
                break;
            }
            dir = parent;
        }
        return original;
    }
#else
    std::vector<char> buffer(4096);
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer[static_cast<size_t>(length)] = '\0';
        fs::path original = fs::path(buffer.data()).parent_path();
        fs::path dir = original;
        std::error_code ec;
        for (int depth = 0; depth < 4; ++depth) {
            if (fs::is_directory(dir / "tools", ec)) {
                return dir;
            }
            fs::path parent = dir.parent_path();
            if (parent.empty() || parent == dir) {
                break;
            }
            dir = parent;
        }
        return original;
    }
#endif
    std::error_code ec;
    return fs::current_path(ec);
}

fs::path temp_root(const fs::path& base_dir) {
    for (const char* name : {"TMPDIR", "TEMP", "TMP"}) {
        const char* value = std::getenv(name);
        if (value && *value) {
            std::error_code ec;
            try {
                fs::path candidate = path_from_utf8(value);
                if (fs::is_directory(candidate, ec)) {
                    return candidate;
                }
            } catch (const std::exception&) {
                // 环境变量可能是 ANSI 编码；继续尝试系统临时目录。
            }
        }
    }
    std::error_code ec;
    fs::path system_temp = fs::temp_directory_path(ec);
    if (!ec) {
        return system_temp;
    }
    fs::path fallback = base_dir / ".tmp";
    ensure_directory(fallback);
    return fallback;
}

void ensure_directory(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        return;
    }
    if (!fs::create_directories(dir, ec) && ec) {
        throw Error("无法创建目录 " + path_to_utf8(dir) + ": " + ec.message());
    }
}

bool is_directory_writable(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    std::random_device random;
    fs::path probe = dir / (".~write_test_" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                            "_" + std::to_string(random()));
    {
        std::ofstream out(probe, std::ios::binary);
        if (!out) {
            return false;
        }
    }
    fs::remove(probe, ec);
    return true;
}

bool file_exists(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

uint64_t file_size_bytes(const fs::path& path) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

std::string extension_lower(const fs::path& path) {
    return lower_ascii(path_to_utf8(path.extension()));
}

std::string stem_utf8(const fs::path& path) {
    return path_to_utf8(path.stem());
}

std::vector<uint8_t> read_binary_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error("无法读取文件 " + path_to_utf8(path));
    }
    in.seekg(0, std::ios::end);
    std::streamoff length = in.tellg();
    if (length < 0) {
        throw Error("无法获取文件大小 " + path_to_utf8(path));
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    if (length > 0) {
        in.read(reinterpret_cast<char*>(data.data()), length);
        if (!in) {
            throw Error("读取文件失败 " + path_to_utf8(path));
        }
    }
    return data;
}

void write_binary_file(const fs::path& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw Error("无法写入文件 " + path_to_utf8(path));
    }
    if (size > 0) {
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!out) {
        throw Error("写入文件失败 " + path_to_utf8(path));
    }
}

void write_binary_file(const fs::path& path, const std::vector<uint8_t>& data) {
    write_binary_file(path, data.data(), data.size());
}

void write_text_file(const fs::path& path, const std::string& text) {
    write_binary_file(path, text.data(), text.size());
}

bool is_valid_utf8(const std::string& value) {
    size_t i = 0;
    while (i < value.size()) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch < 0x80) {
            ++i;
            continue;
        }
        int extra = 0;
        uint32_t cp = 0;
        if ((ch & 0xE0) == 0xC0) {
            extra = 1;
            cp = ch & 0x1F;
        } else if ((ch & 0xF0) == 0xE0) {
            extra = 2;
            cp = ch & 0x0F;
        } else if ((ch & 0xF8) == 0xF0) {
            extra = 3;
            cp = ch & 0x07;
        } else {
            return false;
        }
        if (i + static_cast<size_t>(extra) >= value.size()) {
            return false;
        }
        for (int k = 1; k <= extra; ++k) {
            unsigned char next = static_cast<unsigned char>(value[i + static_cast<size_t>(k)]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

static std::string repair_utf8(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else {
            out += "\xEF\xBF\xBD";
        }
    }
    return out;
}

std::string read_text_auto(const fs::path& path) {
    std::vector<uint8_t> data = read_binary_file(path);
    size_t offset = 0;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        offset = 3;
    }
    std::string text(reinterpret_cast<const char*>(data.data() + offset), data.size() - offset);
    if (is_valid_utf8(text)) {
        return text;
    }
#ifdef _WIN32
    int wide_length = MultiByteToWideChar(936, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide_length > 0) {
        std::wstring wide(static_cast<size_t>(wide_length), L'\0');
        MultiByteToWideChar(936, 0, text.data(), static_cast<int>(text.size()), wide.data(), wide_length);
        std::string converted = to_utf8(wide);
        if (is_valid_utf8(converted)) {
            return converted;
        }
    }
#endif
    return repair_utf8(text);
}

std::string OutputAllocator::key_for(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::string key = path_to_utf8(absolute.lexically_normal());
#ifdef _WIN32
    key = lower_ascii(key);
#endif
    return key;
}

OutputAllocator& OutputAllocator::shared() {
    static OutputAllocator instance;
    return instance;
}

namespace {

bool create_placeholder(const fs::path& path, bool& exists) {
    exists = false;
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            exists = true;
            return false;
        }
        throw Error("无法创建输出占位文件 " + path_to_utf8(path));
    }
    CloseHandle(handle);
    return true;
#else
    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (fd < 0) {
        if (errno == EEXIST) {
            exists = true;
            return false;
        }
        throw Error("无法创建输出占位文件 " + path_to_utf8(path) + ": " + std::strerror(errno));
    }
    ::close(fd);
    return true;
#endif
}

}  // namespace

OutputAllocator::~OutputAllocator() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : claimed_) {
        std::error_code ec;
        if (fs::is_regular_file(item.second, ec) && fs::file_size(item.second, ec) == 0) {
            fs::remove(item.second, ec);
        }
    }
}

fs::path OutputAllocator::claim(const fs::path& dir, const std::string& stem, const std::string& ext) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto try_claim = [&](const fs::path& candidate) {
        std::string key = key_for(candidate);
        if (claimed_.count(key)) {
            return false;
        }
        bool exists = false;
        if (!create_placeholder(candidate, exists)) {
            return false;
        }
        claimed_[key] = candidate;
        return true;
    };

    fs::path candidate = path_join_utf8(dir, stem + ext);
    if (try_claim(candidate)) {
        return candidate;
    }
    for (int i = 1; i < 1000000; ++i) {
        candidate = path_join_utf8(dir, stem + "_" + std::to_string(i) + ext);
        if (try_claim(candidate)) {
            return candidate;
        }
    }
    throw Error("无法为输出文件分配唯一名称");
}

void OutputAllocator::release(const fs::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = key_for(path);
    auto it = claimed_.find(key);
    if (it != claimed_.end()) {
        remove_file_quiet(it->second);
        claimed_.erase(it);
    }
}

fs::path unique_output(const fs::path& dir, const std::string& stem, const std::string& ext) {
    return OutputAllocator::shared().claim(dir, stem, ext);
}

fs::path path_join_utf8(const fs::path& dir, const std::string& name) {
    return dir / path_from_utf8(name);
}

fs::path append_suffix_utf8(const fs::path& path, const std::string& suffix) {
    return path_from_utf8(path_to_utf8(path) + suffix);
}

void replace_file(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw Error("无法写入输出文件 " + path_to_utf8(to) + ": " + ec.message());
        }
        fs::remove(from, ec);
    }
}

void remove_file_quiet(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

fs::path find_executable(const std::string& name, const std::vector<fs::path>& candidates) {
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (!candidate.empty() && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        return {};
    }
    std::string path_text = path_env;
#ifdef _WIN32
    const char separator = ';';
    std::vector<std::string> names = {name};
    if (!ends_with_ci(name, ".exe")) {
        names.push_back(name + ".exe");
    }
#else
    const char separator = ':';
    std::vector<std::string> names = {name};
#endif
    for (const std::string& dir : split_strings(path_text, separator)) {
        if (dir.empty()) {
            continue;
        }
        fs::path dir_path;
        try {
            dir_path = path_from_utf8(dir);
        } catch (const std::exception&) {
            continue;
        }
        for (const std::string& exe_name : names) {
            fs::path candidate = dir_path / exe_name;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
    return {};
}

TempDir::TempDir(const fs::path& root, const std::string& prefix) {
    ensure_directory(root);
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    for (int attempt = 0; attempt < 32; ++attempt) {
        fs::path candidate = root / (prefix + std::to_string(now) + "_" + std::to_string(rd()));
        std::error_code ec;
        if (fs::create_directories(candidate, ec) && !ec) {
            path_ = candidate;
            return;
        }
    }
    throw Error("无法创建临时目录");
}

TempDir::~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
}

}  // namespace fc
