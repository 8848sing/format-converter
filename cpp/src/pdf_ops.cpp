#include "pdf_ops.h"

#include "docx.h"
#include "fs_utils.h"
#include "pdfium_api.h"
#include "pptx.h"
#include "xlsx.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

#if FC_HAVE_PDFIUM

namespace fc {

namespace {

using pdfium::api;

void require_pdfium() {
    if (!api().ready) {
        throw Error("PDF 功能不可用：" + pdfium::load_error());
    }
}

class DocumentHandle {
public:
    explicit DocumentHandle(FPDF_DOCUMENT document) : document_(document) {}
    ~DocumentHandle() {
        if (document_) {
            api().CloseDocument(document_);
        }
    }
    DocumentHandle(const DocumentHandle&) = delete;
    DocumentHandle& operator=(const DocumentHandle&) = delete;
    FPDF_DOCUMENT get() const { return document_; }

private:
    FPDF_DOCUMENT document_ = nullptr;
};

class PageHandle {
public:
    explicit PageHandle(FPDF_PAGE page) : page_(page) {}
    ~PageHandle() {
        if (page_) {
            api().ClosePage(page_);
        }
    }
    PageHandle(const PageHandle&) = delete;
    PageHandle& operator=(const PageHandle&) = delete;
    FPDF_PAGE get() const { return page_; }

private:
    FPDF_PAGE page_ = nullptr;
};

class TextPageHandle {
public:
    explicit TextPageHandle(FPDF_TEXTPAGE page) : page_(page) {}
    ~TextPageHandle() {
        if (page_) {
            api().TextClosePage(page_);
        }
    }
    TextPageHandle(const TextPageHandle&) = delete;
    TextPageHandle& operator=(const TextPageHandle&) = delete;
    FPDF_TEXTPAGE get() const { return page_; }

private:
    FPDF_TEXTPAGE page_ = nullptr;
};

DocumentHandle open_document(const fs::path& path) {
    require_pdfium();
    FPDF_DOCUMENT document = api().LoadDocument(path_to_utf8(path).c_str(), nullptr);
    if (!document) {
        throw Error("无法打开 PDF " + path_to_utf8(path) + ": " + pdfium::last_error_text());
    }
    return DocumentHandle(document);
}

int document_page_count(FPDF_DOCUMENT document) {
    int count = api().GetPageCount(document);
    if (count < 0) {
        throw Error("无法读取 PDF 页数");
    }
    return count;
}

std::string page_text(FPDF_PAGE page) {
    FPDF_TEXTPAGE text_page = api().TextLoadPage(page);
    if (!text_page) {
        return {};
    }
    TextPageHandle handle(text_page);
    int count = api().TextCountChars(text_page);
    if (count <= 0) {
        return {};
    }
    std::vector<unsigned short> buffer(static_cast<size_t>(count) + 1, 0);
    int written = api().TextGetText(text_page, 0, count, buffer.data());
    if (written <= 0) {
        return {};
    }
    size_t units = static_cast<size_t>(written);
    if (units > 0 && buffer[units - 1] == 0) {
        --units;
    }
    buffer.resize(units);
    std::string text = utf16le_to_utf8(buffer);
    text = replace_all(text, "\r\n", "\n");
    text = replace_all(text, "\r", "\n");
    return text;
}

std::vector<std::string> document_texts(FPDF_DOCUMENT document) {
    std::vector<std::string> pages;
    int count = document_page_count(document);
    pages.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        FPDF_PAGE page = api().LoadPage(document, i);
        if (!page) {
            pages.emplace_back();
            continue;
        }
        PageHandle handle(page);
        pages.push_back(page_text(page));
    }
    return pages;
}

std::vector<Image> render_document(FPDF_DOCUMENT document, int dpi) {
    if (dpi <= 0) {
        dpi = 150;
    }
    std::vector<Image> pages;
    int count = document_page_count(document);
    pages.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        FPDF_PAGE page = api().LoadPage(document, i);
        if (!page) {
            throw Error("PDF 第 " + std::to_string(i + 1) + " 页加载失败");
        }
        PageHandle handle(page);
        float width_pt = api().GetPageWidthF(page);
        float height_pt = api().GetPageHeightF(page);
        if (width_pt <= 0 || height_pt <= 0) {
            throw Error("PDF 第 " + std::to_string(i + 1) + " 页尺寸无效");
        }
        double scale = static_cast<double>(dpi) / 72.0;
        int width = std::max(1, static_cast<int>(std::ceil(width_pt * scale)));
        int height = std::max(1, static_cast<int>(std::ceil(height_pt * scale)));
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4, 0);
        FPDF_BITMAP bitmap = api().BitmapCreateEx(width, height, FPDFBitmap_BGRA, bgra.data(), width * 4);
        if (!bitmap) {
            throw Error("PDF 渲染缓冲区创建失败");
        }
        api().BitmapFillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
        api().RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT);
        api().BitmapDestroy(bitmap);

        Image image;
        image.width = width;
        image.height = height;
        image.rgba.resize(static_cast<size_t>(width) * height * 4);
        for (size_t p = 0; p < image.rgba.size(); p += 4) {
            image.rgba[p] = bgra[p + 2];
            image.rgba[p + 1] = bgra[p + 1];
            image.rgba[p + 2] = bgra[p];
            image.rgba[p + 3] = 255;
        }
        pages.push_back(std::move(image));
    }
    return pages;
}

struct FileWriteBridge : FPDF_FILEWRITE {
    std::ofstream* stream = nullptr;
};

int write_block(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* bridge = static_cast<FileWriteBridge*>(self);
    if (!bridge->stream || !bridge->stream->good()) {
        return 0;
    }
    bridge->stream->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return bridge->stream->good() ? 1 : 0;
}

void save_document(FPDF_DOCUMENT document, const fs::path& out) {
    fs::path part = append_suffix_utf8(out, ".part");
    std::ofstream stream(part, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw Error("无法创建 PDF 输出文件 " + path_to_utf8(out));
    }
    FileWriteBridge bridge{};
    bridge.version = 1;
    bridge.WriteBlock = &write_block;
    bridge.stream = &stream;
    FPDF_BOOL ok = api().SaveAsCopy(document, &bridge, FPDF_NO_INCREMENTAL);
    stream.flush();
    bool stream_ok = stream.good();
    stream.close();
    if (!ok || !stream_ok) {
        remove_file_quiet(part);
        throw Error("PDF 写入失败");
    }
    try {
        replace_file(part, out);
    } catch (...) {
        remove_file_quiet(part);
        throw;
    }
}

std::string joined_text(const std::vector<std::string>& pages) {
    std::string text;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (i) {
            text += "\n";
        }
        text += pages[i];
    }
    return text;
}

int parse_rotation(const std::string& target) {
    int angle = 90;
    try {
        angle = std::stoi(target.empty() ? "90" : target);
    } catch (...) {
        throw Error("旋转角度只支持 90 / 180 / 270");
    }
    if (angle != 90 && angle != 180 && angle != 270) {
        throw Error("旋转角度只支持 90 / 180 / 270");
    }
    return angle;
}

}  // namespace

void pdf_merge(const Job& job, EventSink& sink) {
    if (job.inputs.empty()) {
        throw Error("没有可合并的 PDF");
    }
    std::string first = path_to_utf8(job.inputs.front());
    try {
        std::lock_guard<std::mutex> lock(pdfium::mutex());
        require_pdfium();
        FPDF_DOCUMENT dest = api().CreateNewDocument();
        if (!dest) {
            throw Error("无法创建 PDF 文档");
        }
        DocumentHandle dest_handle(dest);
        for (const fs::path& input : job.inputs) {
            DocumentHandle source = open_document(input);
            if (!api().ImportPages(dest, source.get(), nullptr, 0)) {
                throw Error("合并 PDF 失败: " + path_to_utf8(input));
            }
        }
        std::string stem = stem_utf8(job.inputs.front()) +
                           (job.inputs.size() > 1 ? "-合并" : "-副本");
        fs::path out = unique_output(job.out_dir, stem, ".pdf");
        save_document(dest, out);
        sink.file_ok(first, {path_to_utf8(out)});
    } catch (const std::exception& e) {
        sink.file_error(first, e.what());
    }
}

void pdf_split(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle source = open_document(input);
            int count = document_page_count(source.get());
            std::vector<std::string> outputs;
            for (int i = 0; i < count; ++i) {
                FPDF_DOCUMENT dest = api().CreateNewDocument();
                if (!dest) {
                    throw Error("无法创建拆分 PDF");
                }
                DocumentHandle dest_handle(dest);
                std::string range = std::to_string(i + 1);
                if (!api().ImportPages(dest, source.get(), range.c_str(), 0)) {
                    throw Error("拆分第 " + std::to_string(i + 1) + " 页失败");
                }
                fs::path out = unique_output(job.out_dir,
                                             stem_utf8(input) + "-第" + std::to_string(i + 1) + "页",
                                             ".pdf");
                save_document(dest, out);
                outputs.push_back(path_to_utf8(out));
            }
            sink.file_ok(input_text, outputs);
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void pdf_rotate(const Job& job, EventSink& sink) {
    int angle = parse_rotation(job.target);
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle document = open_document(input);
            int count = document_page_count(document.get());
            for (int i = 0; i < count; ++i) {
                FPDF_PAGE page = api().LoadPage(document.get(), i);
                if (!page) {
                    throw Error("加载第 " + std::to_string(i + 1) + " 页失败");
                }
                PageHandle handle(page);
                int current = api().PageGetRotation(page);
                api().PageSetRotation(page, (current + angle / 90) % 4);
            }
            fs::path out = unique_output(job.out_dir,
                                         stem_utf8(input) + "-旋转" + std::to_string(angle), ".pdf");
            save_document(document.get(), out);
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void pdf_compress(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle document = open_document(input);
            fs::path out = unique_output(job.out_dir, stem_utf8(input) + "-压缩", ".pdf");
            save_document(document.get(), out);
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void pdf_to_images(const Job& job, EventSink& sink) {
    std::string target = normalize_image_format(job.target_or("png"));
    if (target != "jpeg" && target != "png" && target != "webp" && target != "bmp" &&
        target != "tiff" && target != "gif" && target != "tga" && target != "ppm" &&
        target != "pgm" && target != "ico" && target != "heif" && target != "avif") {
        throw Error("PDF 转图片不支持目标格式: " + target);
    }
    int dpi = job.option_int("dpi", 200);
    std::string suffix = image_suffix_for_format(target);
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle document = open_document(input);
            std::vector<Image> pages = render_document(document.get(), dpi);
            std::vector<std::string> outputs;
            for (size_t i = 0; i < pages.size(); ++i) {
                fs::path out = unique_output(job.out_dir,
                                             stem_utf8(input) + "-第" + std::to_string(i + 1) + "页",
                                             suffix);
                save_image(pages[i], out, target);
                outputs.push_back(path_to_utf8(out));
            }
            sink.file_ok(input_text, outputs);
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void pdf_to_text(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle document = open_document(input);
            std::string text = joined_text(document_texts(document.get()));
            if (trim(text).empty()) {
                throw Error("未提取到文本（可能是扫描件）");
            }
            fs::path out = unique_output(job.out_dir, stem_utf8(input), ".txt");
            write_text_file(out, text);
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void pdf_to_word(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle document = open_document(input);
            std::vector<std::string> pages = document_texts(document.get());
            std::vector<DocxParagraph> paragraphs;
            for (size_t i = 0; i < pages.size(); ++i) {
                bool first_line = true;
                std::istringstream stream(pages[i]);
                std::string line;
                while (std::getline(stream, line)) {
                    DocxParagraph paragraph;
                    paragraph.text = line;
                    paragraph.page_break_before = first_line && i > 0;
                    paragraphs.push_back(std::move(paragraph));
                    first_line = false;
                }
                if (first_line) {
                    DocxParagraph paragraph;
                    paragraph.page_break_before = i > 0;
                    paragraphs.push_back(std::move(paragraph));
                }
            }
            fs::path out = unique_output(job.out_dir, stem_utf8(input), ".docx");
            write_docx(paragraphs, out, stem_utf8(input));
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void pdf_to_excel(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle document = open_document(input);
            std::vector<std::string> pages = document_texts(document.get());
            std::vector<XlsxSheet> sheets;
            bool any = false;
            for (size_t i = 0; i < pages.size(); ++i) {
                XlsxSheet sheet;
                sheet.name = "第" + std::to_string(i + 1) + "页";
                std::istringstream stream(pages[i]);
                std::string line;
                while (std::getline(stream, line)) {
                    sheet.rows.push_back({line});
                    any = any || !trim(line).empty();
                }
                if (sheet.rows.empty()) {
                    sheet.rows.push_back({""});
                }
                sheets.push_back(std::move(sheet));
            }
            if (!any) {
                throw Error("未提取到任何表格或文本");
            }
            fs::path out = unique_output(job.out_dir, stem_utf8(input), ".xlsx");
            write_xlsx(sheets, out);
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void pdf_to_ppt(const Job& job, EventSink& sink) {
    int dpi = job.option_int("dpi", 150);
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::lock_guard<std::mutex> lock(pdfium::mutex());
            require_pdfium();
            DocumentHandle document = open_document(input);
            std::vector<Image> pages = render_document(document.get(), dpi);
            if (pages.empty()) {
                throw Error("PDF 没有可转换的页面");
            }
            fs::path out = unique_output(job.out_dir, stem_utf8(input), ".pptx");
            write_image_pptx(pages, out);
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

}  // namespace fc

#else

namespace fc {

void pdf_merge(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_split(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_rotate(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_compress(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_to_images(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_to_text(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_to_word(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_to_excel(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }
void pdf_to_ppt(const Job&, EventSink& sink) { sink.file_error("", "当前构建未启用 PDF 支持"); }

}  // namespace fc

#endif