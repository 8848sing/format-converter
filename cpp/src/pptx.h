#pragma once

#include "common.h"
#include "image.h"

#include <vector>

namespace fc {

/* 每页一张整页图片的 PPTX（16:9，13.333 x 7.5 英寸）。 */
void write_image_pptx(const std::vector<Image>& pages, const fs::path& path);

}  // namespace fc