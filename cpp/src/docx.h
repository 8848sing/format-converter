#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace fc {

struct DocxParagraph {
    std::string text;
    bool page_break_before = false;
};

void write_docx(const std::vector<DocxParagraph>& paragraphs, const fs::path& path,
                const std::string& title = "转换结果");

}  // namespace fc