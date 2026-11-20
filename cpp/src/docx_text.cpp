#include "docx_text.h"

#include "fs_utils.h"

#include "miniz.h"
#include "tinyxml2.h"

#include <string>
#include <vector>

namespace fc {

namespace {

std::string local_name(const char* raw) {
    if (!raw) {
        return {};
    }
    const std::string name(raw);
    const size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

void walk_body(const tinyxml2::XMLElement* element, std::string& out) {
    for (const tinyxml2::XMLElement* child = element->FirstChildElement(); child != nullptr;
         child = child->NextSiblingElement()) {
        const std::string local = local_name(child->Name());
        if (local == "t") {
            if (const char* text = child->GetText()) {
                out += text;
            }
        } else if (local == "tab") {
            out += '\t';
        } else if (local == "br") {
            out += '\n';
        } else if (local == "cr") {
            out += '\n';
        } else {
            walk_body(child, out);
        }
        if (local == "p" || local == "tr") {
            out += '\n';
        } else if (local == "tc") {
            out += '\t';
        }
    }
}

bool read_zip_entry(const fs::path& path, const char* entry_name, std::string& content,
                    std::string& error) {
    mz_zip_archive archive{};
    const std::string file = path_to_utf8(path);
    if (!mz_zip_reader_init_file(&archive, file.c_str(), 0)) {
        error = "无法打开 docx（zip 解析失败）";
        return false;
    }
    const int index = mz_zip_reader_locate_file(&archive, entry_name, nullptr, 0);
    if (index < 0) {
        mz_zip_reader_end(&archive);
        error = std::string("docx 内缺少 ") + entry_name;
        return false;
    }
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &size, 0);
    if (!data || size == 0) {
        if (data) {
            mz_free(data);
        }
        mz_zip_reader_end(&archive);
        error = "docx 正文为空或无法解压";
        return false;
    }
    content.assign(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_reader_end(&archive);
    return true;
}

}  // namespace

bool extract_docx_text(const fs::path& path, std::string& text, std::string& error) {
    std::string xml;
    if (!read_zip_entry(path, "word/document.xml", xml, error)) {
        return false;
    }
    tinyxml2::XMLDocument document;
    if (document.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS) {
        error = "docx 正文 XML 解析失败";
        return false;
    }
    const tinyxml2::XMLElement* root = document.RootElement();
    if (!root) {
        error = "docx 正文 XML 为空";
        return false;
    }
    text.clear();
    walk_body(root, text);
    // 压缩连续空行，去掉行尾空白
    std::string cleaned;
    cleaned.reserve(text.size());
    int blank_lines = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            size_t end = i;
            while (end > line_start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                                        text[end - 1] == '\r')) {
                --end;
            }
            const std::string line = text.substr(line_start, end - line_start);
            if (line.empty()) {
                if (blank_lines == 0) {
                    cleaned += '\n';
                }
                ++blank_lines;
            } else {
                blank_lines = 0;
                cleaned += line;
                cleaned += '\n';
            }
            line_start = i + 1;
        }
    }
    while (!cleaned.empty() && cleaned.back() == '\n') {
        cleaned.pop_back();
    }
    text = cleaned;
    if (text.empty()) {
        error = "文档里没有可提取的文字";
        return false;
    }
    return true;
}

void docx_to_text_job(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        try {
            const std::string ext = extension_lower(input);
            if (ext == ".doc") {
                throw Error("Android 端只能提取 .docx 文本，请先在电脑上另存为 .docx");
            }
            std::string text;
            std::string error;
            if (!extract_docx_text(input, text, error)) {
                throw Error(error);
            }
            const fs::path out = unique_output(job.out_dir, stem_utf8(input), ".txt");
            std::string utf8 = text;
            if (utf8.empty() || utf8.back() != '\n') {
                utf8 += '\n';
            }
            write_binary_file(out, reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
            sink.file_ok(path_to_utf8(input), {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(path_to_utf8(input), e.what());
        }
    }
}

}  // namespace fc