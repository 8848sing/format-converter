#include "pdf_writer.h"

#include "fs_utils.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fc {

int PdfBuilder::reserve_object() {
    objects_.push_back(std::string());
    return static_cast<int>(objects_.size());
}

int PdfBuilder::add_object(std::string body) {
    objects_.push_back(std::move(body));
    return static_cast<int>(objects_.size());
}

void PdfBuilder::set_object(int id, std::string body) {
    if (id <= 0 || static_cast<size_t>(id) > objects_.size()) {
        throw Error("PDF 对象编号越界");
    }
    objects_[static_cast<size_t>(id - 1)] = std::move(body);
}

void PdfBuilder::save(const fs::path& path) {
    std::string data;
    data.reserve(1024 * 64);
    data += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(objects_.size() + 1, 0);
    for (size_t i = 0; i < objects_.size(); ++i) {
        offsets[i + 1] = data.size();
        data += std::to_string(i + 1);
        data += " 0 obj\n";
        data += objects_[i];
        data += "\nendobj\n";
    }
    size_t xref_offset = data.size();
    data += "xref\n0 " + std::to_string(objects_.size() + 1) + "\n";
    data += "0000000000 65535 f \n";
    for (size_t i = 1; i <= objects_.size(); ++i) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%010zu 00000 n \n", offsets[i]);
        data += buffer;
    }
    data += "trailer\n<< /Size " + std::to_string(objects_.size() + 1) +
            " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_offset) + "\n%%EOF\n";

    fs::path part = append_suffix_utf8(path, ".part");
    try {
        write_binary_file(part, data.data(), data.size());
        replace_file(part, path);
    } catch (...) {
        remove_file_quiet(part);
        throw;
    }
}

void write_images_pdf(const std::vector<Image>& pages, const fs::path& out, int dpi) {
    if (pages.empty()) {
        throw Error("没有可写入 PDF 的图片");
    }
    if (dpi <= 0) {
        dpi = 150;
    }
    PdfBuilder builder;
    builder.add_object("<< /Type /Catalog /Pages 2 0 R >>");
    int pages_id = builder.add_object("");
    std::vector<int> page_ids;
    page_ids.reserve(pages.size());

    for (size_t i = 0; i < pages.size(); ++i) {
        const Image& image = pages[i];
        if (image.empty()) {
            throw Error("PDF 页面图片为空");
        }
        std::vector<uint8_t> jpeg = encode_jpeg(image, 92);
        if (jpeg.empty()) {
            throw Error("PDF 内图片编码失败");
        }
        std::ostringstream image_object;
        image_object << "<< /Type /XObject /Subtype /Image /Width " << image.width
                     << " /Height " << image.height
                     << " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length "
                     << jpeg.size() << " >>\nstream\n";
        std::string image_body = image_object.str();
        image_body.append(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
        image_body += "\nendstream";
        int image_id = builder.add_object(std::move(image_body));

        double width_pt = static_cast<double>(image.width) * 72.0 / dpi;
        double height_pt = static_cast<double>(image.height) * 72.0 / dpi;
        std::ostringstream content;
        content << std::fixed << std::setprecision(3) << "q\n" << width_pt << " 0 0 " << height_pt
                << " 0 0 cm\n/Im0 Do\nQ\n";
        std::string content_text = content.str();
        int content_id = builder.add_object(
            "<< /Length " + std::to_string(content_text.size()) + " >>\nstream\n" + content_text +
            "endstream");

        std::ostringstream page;
        page << "<< /Type /Page /Parent " << pages_id << " 0 R /MediaBox [0 0 " << std::fixed
             << std::setprecision(3) << width_pt << " " << height_pt
             << "] /Resources << /XObject << /Im0 " << image_id
             << " 0 R >> >> /Contents " << content_id << " 0 R >>";
        page_ids.push_back(builder.add_object(page.str()));
    }

    std::ostringstream pages_object;
    pages_object << "<< /Type /Pages /Kids [";
    for (size_t i = 0; i < page_ids.size(); ++i) {
        if (i) {
            pages_object << " ";
        }
        pages_object << page_ids[i] << " 0 R";
    }
    pages_object << "] /Count " << page_ids.size() << " >>";
    builder.set_object(pages_id, pages_object.str());
    builder.save(out);
}

}  // namespace fc