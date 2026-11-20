#pragma once

#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // RGBA8，逐行从上到下

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
    size_t stride() const { return static_cast<size_t>(width) * 4; }
};

bool is_image_extension(const std::string& ext);
std::string normalize_image_format(const std::string& target);
std::string image_suffix_for_format(const std::string& format);

Image load_image(const fs::path& path, bool apply_orientation = true);
void save_image(const Image& image, const fs::path& path, const std::string& format,
                int quality = -1);

void flatten_alpha(Image& image, uint8_t r = 255, uint8_t g = 255, uint8_t b = 255);
Image resize_to_fit(const Image& image, int max_width, int max_height);
Image resize_exact(const Image& image, int width, int height);
Image pad_center(const Image& image, int width, int height, uint8_t r = 0, uint8_t g = 0, uint8_t b = 0,
                 uint8_t a = 0);

std::vector<uint8_t> encode_jpeg(const Image& image, int quality = 94);
std::vector<uint8_t> encode_png(const Image& image);

/* 图片类转换实现；由 registry 绑定。 */
void convert_image_format(const Job& job, EventSink& sink);
void images_to_pdf(const Job& job, EventSink& sink);
void merge_images_to_pdf(const Job& job, EventSink& sink);
void images_to_animated_gif(const Job& job, EventSink& sink);

/* Windows 平台编解码（WIC）接口；其他平台返回 false。 */
bool platform_decode_image(const fs::path& path, Image& image, std::string& error);
bool platform_encode_image(const Image& image, const fs::path& path, const std::string& format,
                           std::string& error);

}  // namespace fc