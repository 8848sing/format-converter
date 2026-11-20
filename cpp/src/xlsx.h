#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace fc {

struct XlsxSheet {
    std::string name;
    std::vector<std::vector<std::string>> rows;
};

void write_xlsx(const std::vector<XlsxSheet>& sheets, const fs::path& path);
std::vector<std::vector<std::string>> read_xlsx_first_sheet(const fs::path& path);

}  // namespace fc