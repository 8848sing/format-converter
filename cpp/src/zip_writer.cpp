#include "zip_writer.h"

#include "fs_utils.h"

#include <miniz.h>

#include <cstring>

namespace fc {

ZipWriter::ZipWriter() {
    auto* archive = new mz_zip_archive();
    std::memset(archive, 0, sizeof(*archive));
    if (!mz_zip_writer_init_heap(archive, 0, 0)) {
        delete archive;
        throw Error("无法初始化 ZIP 写入器");
    }
    archive_ = archive;
}

ZipWriter::~ZipWriter() {
    if (archive_) {
        auto* archive = static_cast<mz_zip_archive*>(archive_);
        if (!closed_) {
            mz_zip_writer_end(archive);
        }
        delete archive;
    }
}

void ZipWriter::add(const std::string& name, const std::string& data) {
    add(name, std::vector<uint8_t>(data.begin(), data.end()));
}

void ZipWriter::add(const std::string& name, const std::vector<uint8_t>& data) {
    auto* archive = static_cast<mz_zip_archive*>(archive_);
    if (!archive || closed_) {
        throw Error("ZIP 写入器已关闭");
    }
    if (!mz_zip_writer_add_mem(archive, name.c_str(), data.data(), data.size(),
                               MZ_DEFAULT_COMPRESSION)) {
        throw Error("写入 ZIP 条目失败: " + name);
    }
}

void ZipWriter::save(const fs::path& path) {
    auto* archive = static_cast<mz_zip_archive*>(archive_);
    if (!archive || closed_) {
        throw Error("ZIP 写入器已关闭");
    }
    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(archive, &buffer, &size)) {
        throw Error("ZIP 打包失败");
    }
    mz_zip_writer_end(archive);
    closed_ = true;
    try {
        write_binary_file(path, buffer, size);
    } catch (...) {
        mz_free(buffer);
        throw;
    }
    mz_free(buffer);
}

ZipReader::ZipReader(const fs::path& path) {
    data_ = read_binary_file(path);
    auto* archive = new mz_zip_archive();
    std::memset(archive, 0, sizeof(*archive));
    if (!mz_zip_reader_init_mem(archive, data_.data(), data_.size(), 0)) {
        delete archive;
        throw Error("无法读取 ZIP 文件 " + path_to_utf8(path));
    }
    archive_ = archive;
}

ZipReader::~ZipReader() {
    if (archive_) {
        auto* archive = static_cast<mz_zip_archive*>(archive_);
        mz_zip_reader_end(archive);
        delete archive;
    }
}

bool ZipReader::contains(const std::string& name) const {
    auto* archive = static_cast<mz_zip_archive*>(archive_);
    return archive && mz_zip_reader_locate_file(archive, name.c_str(), nullptr, 0) >= 0;
}

std::vector<uint8_t> ZipReader::read_binary(const std::string& name) const {
    auto* archive = static_cast<mz_zip_archive*>(archive_);
    if (!archive) {
        throw Error("ZIP 读取器已关闭");
    }
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(archive, name.c_str(), &size, 0);
    if (!data) {
        throw Error("ZIP 中缺少条目: " + name);
    }
    std::vector<uint8_t> result(static_cast<uint8_t*>(data),
                                static_cast<uint8_t*>(data) + size);
    mz_free(data);
    return result;
}

std::string ZipReader::read_text(const std::string& name) const {
    std::vector<uint8_t> data = read_binary(name);
    return std::string(data.begin(), data.end());
}

std::vector<std::string> ZipReader::names() const {
    std::vector<std::string> result;
    auto* archive = static_cast<mz_zip_archive*>(archive_);
    if (!archive) {
        return result;
    }
    mz_uint count = mz_zip_reader_get_num_files(archive);
    result.reserve(count);
    for (mz_uint i = 0; i < count; ++i) {
        char name[1024];
        if (mz_zip_reader_get_filename(archive, i, name, sizeof(name)) > 0) {
            result.emplace_back(name);
        }
    }
    return result;
}

std::string xml_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (char ch : value) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
                    continue;
                }
                out.push_back(ch);
        }
    }
    return out;
}

}  // namespace fc