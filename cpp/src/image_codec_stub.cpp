#include "image.h"

#ifndef _WIN32

namespace fc {

bool platform_decode_image(const fs::path& path, Image& image, std::string& error) {
    (void)path;
    (void)image;
    error = "当前平台没有可用的系统图片解码器";
    return false;
}

bool platform_encode_image(const Image& image, const fs::path& path, const std::string& format,
                           std::string& error) {
    (void)image;
    (void)path;
    (void)format;
    error = "当前平台没有可用的系统图片编码器";
    return false;
}

}  // namespace fc

#endif