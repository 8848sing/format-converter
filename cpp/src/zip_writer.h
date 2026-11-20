#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace fc {

class ZipWriter {
public:
    ZipWriter();
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    void add(const std::string& name, const std::string& data);
    void add(const std::string& name, const std::vector<uint8_t>& data);
    void save(const fs::path& path);

private:
    void* archive_ = nullptr;
    bool closed_ = false;
};

class ZipReader {
public:
    explicit ZipReader(const fs::path& path);
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool contains(const std::string& name) const;
    std::string read_text(const std::string& name) const;
    std::vector<uint8_t> read_binary(const std::string& name) const;
    std::vector<std::string> names() const;

private:
    void* archive_ = nullptr;
    std::vector<uint8_t> data_;
};

std::string xml_escape(const std::string& value);

}  // namespace fc