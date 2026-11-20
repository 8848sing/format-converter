#pragma once

#include "common.h"
#include "image.h"

#include <string>
#include <vector>

namespace fc {

/* 把若干张图片写成多页 PDF（每页一张图）；dpi 决定页面物理尺寸。 */
void write_images_pdf(const std::vector<Image>& pages, const fs::path& out, int dpi = 150);

/* 最小 PDF 对象写入器，供内部复用。 */
class PdfBuilder {
public:
    int add_object(std::string body);
    void set_object(int id, std::string body);
    int reserve_object();
    void save(const fs::path& path);

private:
    std::vector<std::string> objects_;  // 1-based；空字符串表示未设置
};

}  // namespace fc