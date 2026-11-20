#include "image.h"

#include "fs_utils.h"
#include "media.h"
#include "pdf_writer.h"
#include "process_util.h"
#include "tiff_decode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_ONLY_PNM
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "gif.h"

#if FC_HAVE_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#endif

namespace fc {

namespace {

constexpr int kPdfDpi = 150;

uint16_t read_u16(const uint8_t* data, bool little) {
    if (little) {
        return static_cast<uint16_t>(data[0] | (data[1] << 8));
    }
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t read_u32(const uint8_t* data, bool little) {
    if (little) {
        return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    }
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint16_t exif_orientation(const std::vector<uint8_t>& data) {
    if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return 1;
    }
    size_t pos = 2;
    while (pos + 4 <= data.size()) {
        if (data[pos] != 0xFF) {
            ++pos;
            continue;
        }
        uint8_t marker = data[pos + 1];
        pos += 2;
        if (marker == 0xD8 || marker == 0xD9) {
            continue;
        }
        if (pos + 2 > data.size()) {
            break;
        }
        uint16_t length = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        if (length < 2 || pos + length > data.size()) {
            break;
        }
        if (marker == 0xE1 && length >= 8 &&
            std::memcmp(data.data() + pos + 2, "Exif\0\0", 6) == 0) {
            size_t tiff = pos + 8;
            if (tiff + 8 > data.size()) {
                return 1;
            }
            bool little = data[tiff] == 'I' && data[tiff + 1] == 'I';
            if (!little && !(data[tiff] == 'M' && data[tiff + 1] == 'M')) {
                return 1;
            }
            uint32_t ifd_offset = read_u32(data.data() + tiff + 4, little);
            size_t ifd = tiff + ifd_offset;
            if (ifd + 2 > data.size()) {
                return 1;
            }
            uint16_t count = read_u16(data.data() + ifd, little);
            for (uint16_t i = 0; i < count; ++i) {
                size_t entry = ifd + 2 + static_cast<size_t>(i) * 12;
                if (entry + 12 > data.size()) {
                    break;
                }
                uint16_t tag = read_u16(data.data() + entry, little);
                if (tag == 0x0112) {
                    uint16_t type = read_u16(data.data() + entry + 2, little);
                    uint32_t value_count = read_u32(data.data() + entry + 4, little);
                    if (type == 3 && value_count >= 1) {
                        uint16_t value = read_u16(data.data() + entry + 8, little);
                        if (value >= 1 && value <= 8) {
                            return value;
                        }
                    }
                }
            }
        }
        pos += length;
    }
    return 1;
}

Image apply_orientation(const Image& source, uint16_t orientation) {
    if (orientation <= 1 || source.empty()) {
        return source;
    }
    Image out;
    const int w = source.width;
    const int h = source.height;
    if (orientation == 2 || orientation == 3 || orientation == 4) {
        out.width = w;
        out.height = h;
    } else {
        out.width = h;
        out.height = w;
    }
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            int sx = x;
            int sy = y;
            switch (orientation) {
                case 2: sx = w - 1 - x; sy = y; break;
                case 3: sx = w - 1 - x; sy = h - 1 - y; break;
                case 4: sx = x; sy = h - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = h - 1 - x; break;
                case 7: sx = w - 1 - y; sy = h - 1 - x; break;
                case 8: sx = w - 1 - y; sy = x; break;
                default: break;
            }
            size_t src = (static_cast<size_t>(sy) * w + sx) * 4;
            size_t dst = (static_cast<size_t>(y) * out.width + x) * 4;
            std::memcpy(out.rgba.data() + dst, source.rgba.data() + src, 4);
        }
    }
    return out;
}

bool load_stb(const std::vector<uint8_t>& bytes, Image& out, std::string& error) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                            &channels, 4);
    if (!pixels) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "图片解码失败";
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

#if FC_HAVE_WEBP
bool load_webp(const std::vector<uint8_t>& bytes, Image& out, std::string& error) {
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(bytes.data(), bytes.size(), &width, &height)) {
        error = "WebP 文件头无效";
        return false;
    }
    uint8_t* pixels = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height);
    if (!pixels) {
        error = "WebP 解码失败";
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    WebPFree(pixels);
    return true;
}

bool save_webp(const Image& image, const fs::path& path, int quality, std::string& error) {
    uint8_t* output = nullptr;
    size_t output_size = WebPEncodeRGBA(image.rgba.data(), image.width, image.height,
                                        static_cast<int>(image.stride()),
                                        static_cast<float>(quality), &output);
    if (!output || output_size == 0) {
        error = "WebP 编码失败";
        return false;
    }
    write_binary_file(path, output, output_size);
    WebPFree(output);
    return true;
}
#endif

void append_to_buffer(void* context, void* data, int size) {
    auto* buffer = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

std::vector<uint8_t> to_rgb(const Image& image) {
    std::vector<uint8_t> rgb(static_cast<size_t>(image.width) * image.height * 3);
    for (size_t i = 0, j = 0; i < image.rgba.size(); i += 4, j += 3) {
        rgb[j] = image.rgba[i];
        rgb[j + 1] = image.rgba[i + 1];
        rgb[j + 2] = image.rgba[i + 2];
    }
    return rgb;
}

Image resize_bilinear(const Image& source, int width, int height) {
    Image out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * height * 4);
    if (source.empty() || width <= 0 || height <= 0) {
        return out;
    }
    const float scale_x = static_cast<float>(source.width) / width;
    const float scale_y = static_cast<float>(source.height) / height;
    for (int y = 0; y < height; ++y) {
        float sy = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
        int y0 = static_cast<int>(std::floor(sy));
        float fy = sy - y0;
        if (y0 < 0) {
            y0 = 0;
            fy = 0;
        }
        int y1 = std::min(y0 + 1, source.height - 1);
        for (int x = 0; x < width; ++x) {
            float sx = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
            int x0 = static_cast<int>(std::floor(sx));
            float fx = sx - x0;
            if (x0 < 0) {
                x0 = 0;
                fx = 0;
            }
            int x1 = std::min(x0 + 1, source.width - 1);
            size_t p00 = (static_cast<size_t>(y0) * source.width + x0) * 4;
            size_t p10 = (static_cast<size_t>(y0) * source.width + x1) * 4;
            size_t p01 = (static_cast<size_t>(y1) * source.width + x0) * 4;
            size_t p11 = (static_cast<size_t>(y1) * source.width + x1) * 4;
            size_t dst = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                float top = source.rgba[p00 + c] * (1.0f - fx) + source.rgba[p10 + c] * fx;
                float bottom = source.rgba[p01 + c] * (1.0f - fx) + source.rgba[p11 + c] * fx;
                out.rgba[dst + c] = static_cast<uint8_t>(
                    std::clamp(top * (1.0f - fy) + bottom * fy, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

FILE* open_utf8(const fs::path& path, const wchar_t* mode) {
#ifdef _WIN32
    return _wfopen(path.wstring().c_str(), mode);
#else
    (void)mode;
    return std::fopen(path.u8string().c_str(), "wb");
#endif
}

bool write_gif_frames(const std::vector<Image>& frames, const fs::path& path, int delay_centiseconds,
                      std::string& error) {
    if (frames.empty()) {
        error = "没有可写入的 GIF 帧";
        return false;
    }
    int width = 0;
    int height = 0;
    for (const Image& frame : frames) {
        width = std::max(width, frame.width);
        height = std::max(height, frame.height);
    }
    if (width <= 0 || height <= 0) {
        error = "GIF 帧尺寸无效";
        return false;
    }
    FILE* file = open_utf8(path, L"wb");
    if (!file) {
        error = "无法创建 GIF 文件 " + path_to_utf8(path);
        return false;
    }
    GifWriter writer{};
    if (!GifBeginHandle(&writer, file, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                        static_cast<uint32_t>(std::max(0, delay_centiseconds)), 8, false)) {
        std::fclose(file);
        error = "GIF 初始化失败";
        return false;
    }
    std::vector<uint8_t> canvas(static_cast<size_t>(width) * height * 4, 0);
    for (const Image& frame : frames) {
        std::fill(canvas.begin(), canvas.end(), 0);
        int offset_x = (width - frame.width) / 2;
        int offset_y = (height - frame.height) / 2;
        for (int y = 0; y < frame.height; ++y) {
            size_t src = static_cast<size_t>(y) * frame.width * 4;
            size_t dst = (static_cast<size_t>(y + offset_y) * width + offset_x) * 4;
            std::memcpy(canvas.data() + dst, frame.rgba.data() + src,
                        static_cast<size_t>(frame.width) * 4);
        }
        if (!GifWriteFrame(&writer, canvas.data(), static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height),
                           static_cast<uint32_t>(std::max(0, delay_centiseconds)), 8, false)) {
            GifEnd(&writer);
            error = "GIF 帧写入失败";
            return false;
        }
    }
    if (!GifEnd(&writer)) {
        error = "GIF 文件收尾失败";
        return false;
    }
    return true;
}

bool write_ico(const Image& image, const fs::path& path, std::string& error) {
    const int sizes[] = {16, 32, 48, 64, 128, 256};
    std::vector<std::vector<uint8_t>> payloads;
    for (int size : sizes) {
        Image scaled = resize_to_fit(image, size, size);
        Image canvas = pad_center(scaled, size, size);
        std::vector<uint8_t> png = encode_png(canvas);
        if (png.empty()) {
            error = "ICO 内的 PNG 编码失败";
            return false;
        }
        payloads.push_back(std::move(png));
    }
    std::vector<uint8_t> header(6, 0);
    header[2] = 1;
    header[4] = static_cast<uint8_t>(payloads.size() & 0xFF);
    header[5] = static_cast<uint8_t>((payloads.size() >> 8) & 0xFF);
    uint32_t offset = static_cast<uint32_t>(6 + 16 * payloads.size());
    for (size_t i = 0; i < payloads.size(); ++i) {
        uint8_t entry[16] = {0};
        int size = sizes[i];
        entry[0] = static_cast<uint8_t>(size == 256 ? 0 : size);
        entry[1] = static_cast<uint8_t>(size == 256 ? 0 : size);
        entry[2] = 0;
        entry[3] = 0;
        entry[4] = 1;
        entry[5] = 0;
        entry[6] = 32;
        entry[7] = 0;
        entry[8] = static_cast<uint8_t>(payloads[i].size() & 0xFF);
        entry[9] = static_cast<uint8_t>((payloads[i].size() >> 8) & 0xFF);
        entry[10] = static_cast<uint8_t>((payloads[i].size() >> 16) & 0xFF);
        entry[11] = static_cast<uint8_t>((payloads[i].size() >> 24) & 0xFF);
        entry[12] = static_cast<uint8_t>(offset & 0xFF);
        entry[13] = static_cast<uint8_t>((offset >> 8) & 0xFF);
        entry[14] = static_cast<uint8_t>((offset >> 16) & 0xFF);
        entry[15] = static_cast<uint8_t>((offset >> 24) & 0xFF);
        header.insert(header.end(), entry, entry + 16);
        offset += static_cast<uint32_t>(payloads[i].size());
    }
    for (const auto& payload : payloads) {
        header.insert(header.end(), payload.begin(), payload.end());
    }
    write_binary_file(path, header);
    return true;
}

bool write_ppm(const Image& image, const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << "P6\n" << image.width << " " << image.height << "\n255\n";
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        out.put(static_cast<char>(image.rgba[i]));
        out.put(static_cast<char>(image.rgba[i + 1]));
        out.put(static_cast<char>(image.rgba[i + 2]));
    }
    return static_cast<bool>(out);
}

bool write_pgm(const Image& image, const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << "P5\n" << image.width << " " << image.height << "\n255\n";
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        int luma = (image.rgba[i] * 299 + image.rgba[i + 1] * 587 + image.rgba[i + 2] * 114) / 1000;
        out.put(static_cast<char>(luma));
    }
    return static_cast<bool>(out);
}

}  // namespace

bool is_image_extension(const std::string& ext) {
    static const char* values[] = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff", ".gif",
                                   ".ico", ".avif", ".heic", ".heif", ".ppm", ".pgm", ".tga"};
    for (const char* value : values) {
        if (iequals(ext, value)) {
            return true;
        }
    }
    return false;
}

std::string normalize_image_format(const std::string& target) {
    std::string format = lower_ascii(target);
    if (format == "jpg") {
        return "jpeg";
    }
    if (format == "tif") {
        return "tiff";
    }
    if (format == "heic") {
        return "heif";
    }
    return format;
}

std::string image_suffix_for_format(const std::string& format) {
    if (format == "jpeg") {
        return ".jpg";
    }
    if (format == "tiff") {
        return ".tif";
    }
    if (format == "heif") {
        return ".heic";
    }
    return "." + format;
}

Image load_image(const fs::path& path, bool apply_exif) {
    std::vector<uint8_t> bytes = read_binary_file(path);
    Image image;
    std::string error;
    std::string ext = extension_lower(path);

    bool loaded = false;
    bool special = ext == ".tif" || ext == ".tiff" || ext == ".ico" || ext == ".heic" ||
                   ext == ".heif" || ext == ".avif";
    if (special) {
        loaded = platform_decode_image(path, image, error);
        if (!loaded && (ext == ".tif" || ext == ".tiff")) {
            // Android 没有系统 TIFF 解码器；桌面 WIC 也可能缺，统一走内置解码器兜底。
            int tiff_width = 0;
            int tiff_height = 0;
            std::vector<uint8_t> tiff_pixels;
            if (decode_tiff(bytes, tiff_width, tiff_height, tiff_pixels, error)) {
                image.width = tiff_width;
                image.height = tiff_height;
                image.rgba = std::move(tiff_pixels);
                loaded = true;
            }
        }
        if (!loaded && (ext == ".tif" || ext == ".tiff" || ext == ".ico")) {
            // stb 不支持 TIFF，失败时会留下 "unknown image type" 这类英文提示，
            // 平台/内置解码器给出的原因更具体，优先保留它。
            const std::string platform_error = error;
            loaded = load_stb(bytes, image, error);
            if (!loaded && !platform_error.empty()) {
                error = platform_error;
            }
        }
    } else if (ext == ".webp") {
#if FC_HAVE_WEBP
        loaded = load_webp(bytes, image, error);
#else
        error = "未启用 WebP 支持";
#endif
    } else {
        loaded = load_stb(bytes, image, error);
        if (!loaded) {
            loaded = platform_decode_image(path, image, error);
        }
    }

    if (!loaded) {
        throw Error("无法读取图片 " + path_to_utf8(path) + ": " + error);
    }
    if (apply_exif) {
        image = apply_orientation(image, exif_orientation(bytes));
    }
    return image;
}

void flatten_alpha(Image& image, uint8_t r, uint8_t g, uint8_t b) {
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        int alpha = image.rgba[i + 3];
        if (alpha >= 255) {
            continue;
        }
        image.rgba[i] = static_cast<uint8_t>((image.rgba[i] * alpha + r * (255 - alpha)) / 255);
        image.rgba[i + 1] = static_cast<uint8_t>((image.rgba[i + 1] * alpha + g * (255 - alpha)) / 255);
        image.rgba[i + 2] = static_cast<uint8_t>((image.rgba[i + 2] * alpha + b * (255 - alpha)) / 255);
        image.rgba[i + 3] = 255;
    }
}

Image resize_exact(const Image& image, int width, int height) {
    return resize_bilinear(image, width, height);
}

Image resize_to_fit(const Image& image, int max_width, int max_height) {
    if (image.empty() || max_width <= 0 || max_height <= 0) {
        return Image();
    }
    if (image.width <= max_width && image.height <= max_height) {
        return image;
    }
    double scale = std::min(static_cast<double>(max_width) / image.width,
                            static_cast<double>(max_height) / image.height);
    int width = std::max(1, static_cast<int>(std::lround(image.width * scale)));
    int height = std::max(1, static_cast<int>(std::lround(image.height * scale)));
    return resize_bilinear(image, width, height);
}

Image pad_center(const Image& image, int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Image out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        out.rgba[i] = r;
        out.rgba[i + 1] = g;
        out.rgba[i + 2] = b;
        out.rgba[i + 3] = a;
    }
    int offset_x = (width - image.width) / 2;
    int offset_y = (height - image.height) / 2;
    for (int y = 0; y < image.height; ++y) {
        if (y + offset_y < 0 || y + offset_y >= height) {
            continue;
        }
        size_t src = static_cast<size_t>(y) * image.width * 4;
        size_t dst = (static_cast<size_t>(y + offset_y) * width + offset_x) * 4;
        std::memcpy(out.rgba.data() + dst, image.rgba.data() + src,
                    static_cast<size_t>(image.width) * 4);
    }
    return out;
}

std::vector<uint8_t> encode_jpeg(const Image& image, int quality) {
    std::vector<uint8_t> rgb = to_rgb(image);
    std::vector<uint8_t> output;
    if (!stbi_write_jpg_to_func(append_to_buffer, &output, image.width, image.height, 3, rgb.data(),
                                std::clamp(quality, 1, 100))) {
        return {};
    }
    return output;
}

std::vector<uint8_t> encode_png(const Image& image) {
    std::vector<uint8_t> output;
    if (!stbi_write_png_to_func(append_to_buffer, &output, image.width, image.height, 4,
                                image.rgba.data(), static_cast<int>(image.stride()))) {
        return {};
    }
    return output;
}

void save_image(const Image& source, const fs::path& path, const std::string& target, int quality) {
    std::string format = normalize_image_format(target);
    Image image = source;
    std::string error;

    auto write_temp = [&](const std::function<void(const fs::path&)>& writer) {
        fs::path part = append_suffix_utf8(path, ".part");
        try {
            writer(part);
            replace_file(part, path);
        } catch (...) {
            remove_file_quiet(part);
            throw;
        }
    };

    if (format == "jpeg") {
        flatten_alpha(image);
        std::vector<uint8_t> data = encode_jpeg(image, quality > 0 ? quality : 94);
        if (data.empty()) {
            throw Error("JPEG 编码失败");
        }
        write_temp([&](const fs::path& p) { write_binary_file(p, data); });
        return;
    }
    if (format == "png") {
        std::vector<uint8_t> data = encode_png(image);
        if (data.empty()) {
            throw Error("PNG 编码失败");
        }
        write_temp([&](const fs::path& p) { write_binary_file(p, data); });
        return;
    }
    if (format == "bmp") {
        flatten_alpha(image);
        std::vector<uint8_t> rgb = to_rgb(image);
        std::vector<uint8_t> data;
        if (!stbi_write_bmp_to_func(append_to_buffer, &data, image.width, image.height, 3,
                                    rgb.data())) {
            throw Error("BMP 编码失败");
        }
        write_temp([&](const fs::path& p) { write_binary_file(p, data); });
        return;
    }
    if (format == "tga") {
        std::vector<uint8_t> data;
        if (!stbi_write_tga_to_func(append_to_buffer, &data, image.width, image.height, 4,
                                    image.rgba.data())) {
            throw Error("TGA 编码失败");
        }
        write_temp([&](const fs::path& p) { write_binary_file(p, data); });
        return;
    }
    if (format == "ppm" || format == "pgm") {
        write_temp([&](const fs::path& p) {
            bool ok = format == "ppm" ? write_ppm(image, p) : write_pgm(image, p);
            if (!ok) {
                throw Error("PPM/PGM 写入失败");
            }
        });
        return;
    }
    if (format == "gif") {
        flatten_alpha(image);
        write_temp([&](const fs::path& p) {
            std::string gif_error;
            if (!write_gif_frames({image}, p, 10, gif_error)) {
                throw Error(gif_error);
            }
        });
        return;
    }
    if (format == "webp") {
#if FC_HAVE_WEBP
        write_temp([&](const fs::path& p) {
            std::string webp_error;
            if (!save_webp(image, p, quality > 0 ? quality : 93, webp_error)) {
                throw Error(webp_error);
            }
        });
        return;
#else
        throw Error("当前构建未启用 WebP 支持");
#endif
    }
    if (format == "ico") {
        image = resize_to_fit(image, 256, 256);
        write_temp([&](const fs::path& p) {
            std::string ico_error;
            if (!write_ico(image, p, ico_error)) {
                throw Error(ico_error);
            }
        });
        return;
    }
    if (format == "tiff" || format == "heif" || format == "avif") {
        if (format == "tiff") {
            flatten_alpha(image);
        }
        write_temp([&](const fs::path& p) {
            std::string platform_error;
            if (!platform_encode_image(image, p, format, platform_error)) {
                throw Error("当前平台无法输出 " + format + ": " + platform_error);
            }
        });
        return;
    }
    throw Error("不支持的图片目标格式: " + target);
}

namespace {

// AVIF 兜底：Windows 的 WIC 一般没有 AV1/AVIF 编码器，这里先落一个临时 PNG，
// 再用便携包里的 FFmpeg（libsvtav1）编码成 .avif。失败时 error 带具体原因。
bool encode_avif_with_ffmpeg(const Image& image, const fs::path& path, int quality,
                             const fs::path& ffmpeg, std::string& error) {
    std::error_code ec;
    fs::path temp = fs::temp_directory_path(ec);
    if (ec) {
        error = "无法定位系统临时目录";
        return false;
    }
    fs::path dir;
    for (int attempt = 0; attempt < 32 && dir.empty(); ++attempt) {
        fs::path candidate = temp / ("fc_avif_" + std::to_string(attempt) + "_" +
                                     std::to_string(static_cast<long long>(
                                         std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count())));
        std::error_code create_ec;
        if (fs::create_directory(candidate, create_ec) && !create_ec) {
            dir = candidate;
        }
    }
    if (dir.empty()) {
        error = "无法创建 AVIF 临时目录";
        return false;
    }
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } cleanup{dir};

    fs::path png = dir / "frame.png";
    try {
        save_image(image, png, "png", -1);
    } catch (const std::exception& e) {
        error = std::string("写临时 PNG 失败: ") + e.what();
        return false;
    }

    int clamped = quality > 0 ? std::min(100, std::max(10, quality)) : 90;
    // quality 越高 -> crf 越低：92 约等于 crf 17。
    int crf = std::clamp(63 - clamped * 5 / 10, 16, 52);
    fs::path staged = append_suffix_utf8(path, ".part");
    std::vector<std::string> args = {
        path_to_utf8(ffmpeg),
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        path_to_utf8(png),
        "-vf",
        "scale=trunc(iw/2)*2:trunc(ih/2)*2",
        "-c:v",
        "libsvtav1",
        "-crf",
        std::to_string(crf),
        "-preset",
        "8",
        "-pix_fmt",
        "yuv420p",
        // 输出名带 .part 后缀，FFmpeg 无法据此推断容器，显式指定 avif muxer。
        "-f",
        "avif",
        path_to_utf8(staged),
    };
    ProcessResult result = run_process(args, 180000);
    if (!file_exists(staged) || file_size_bytes(staged) == 0) {
        std::string detail = trim(result.err.empty() ? result.out : result.err);
        error = detail.empty() ? "FFmpeg 未产出 AVIF（可能缺少 libsvtav1 编码器）" : detail;
        remove_file_quiet(staged);
        return false;
    }
    try {
        replace_file(staged, path);
    } catch (const std::exception& e) {
        error = e.what();
        remove_file_quiet(staged);
        return false;
    }
    return true;
}

}  // namespace

void convert_image_format(const Job& job, EventSink& sink) {
    std::string target = lower_ascii(job.target_or("png"));
    static const char* allowed[] = {"jpg", "jpeg", "png", "webp", "bmp", "tif", "tiff",
                                    "gif", "ico", "avif", "heic", "heif", "ppm", "pgm", "tga"};
    bool ok = false;
    for (const char* value : allowed) {
        if (target == value) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        throw Error("不支持的图片目标格式：" + target);
    }
    std::string format = normalize_image_format(target);
    std::string suffix = image_suffix_for_format(format);
    int quality = job.option_int("quality", -1);
    bool ffmpeg_probe_done = false;
    fs::path ffmpeg;
    for (const fs::path& input : job.inputs) {
        try {
            Image image = load_image(input, true);
            fs::path out = unique_output(job.out_dir, stem_utf8(input), suffix);
            try {
                save_image(image, out, format, quality);
            } catch (const Error& save_error) {
                // Windows 的 WIC 一般没有 AV1/AVIF 编码器。AVIF 失败时改用
                // 便携包里的 FFmpeg（libsvtav1）再试一次，其它格式照常抛错。
                if (format != "avif") {
                    throw;
                }
                if (!ffmpeg_probe_done) {
                    ffmpeg_probe_done = true;
                    try {
                        ffmpeg = find_ffmpeg(job);
                    } catch (...) {
                        ffmpeg.clear();
                    }
                }
                std::string avif_error = save_error.what();
                if (ffmpeg.empty() ||
                    !encode_avif_with_ffmpeg(image, out, quality, ffmpeg, avif_error)) {
                    throw Error("AVIF 编码失败（系统无 AVIF 编码器，且 FFmpeg 兜底不可用）：" +
                                avif_error);
                }
            }
            sink.file_ok(path_to_utf8(input), {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(path_to_utf8(input), e.what());
        }
    }
}

void images_to_pdf(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        try {
            Image image = load_image(input, true);
            flatten_alpha(image);
            fs::path out = unique_output(job.out_dir, stem_utf8(input), ".pdf");
            std::vector<Image> pages;
            pages.push_back(std::move(image));
            write_images_pdf(pages, out, kPdfDpi);
            sink.file_ok(path_to_utf8(input), {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(path_to_utf8(input), e.what());
        }
    }
}

void merge_images_to_pdf(const Job& job, EventSink& sink) {
    if (job.inputs.empty()) {
        throw Error("没有可合并的图片");
    }
    std::string input = path_to_utf8(job.inputs.front());
    try {
        std::vector<Image> pages;
        pages.reserve(job.inputs.size());
        for (const fs::path& file : job.inputs) {
            Image image = load_image(file, true);
            flatten_alpha(image);
            pages.push_back(std::move(image));
        }
        std::string stem = stem_utf8(job.inputs.front()) +
                           (job.inputs.size() > 1 ? "-合并" : "");
        fs::path out = unique_output(job.out_dir, stem, ".pdf");
        write_images_pdf(pages, out, kPdfDpi);
        sink.file_ok(input, {path_to_utf8(out)});
    } catch (const std::exception& e) {
        sink.file_error(input, e.what());
    }
}

void images_to_animated_gif(const Job& job, EventSink& sink) {
    if (job.inputs.empty()) {
        throw Error("没有可合成 GIF 的图片");
    }
    std::string input = path_to_utf8(job.inputs.front());
    try {
        std::vector<Image> frames;
        frames.reserve(job.inputs.size());
        for (const fs::path& file : job.inputs) {
            Image image = load_image(file, true);
            image = resize_to_fit(image, 1280, 1280);
            flatten_alpha(image);
            frames.push_back(std::move(image));
        }
        int duration = job.option_int("duration_ms", 600);
        fs::path out = unique_output(job.out_dir, stem_utf8(job.inputs.front()) + "-动图", ".gif");
        std::string error;
        if (!write_gif_frames(frames, out, std::max(8, duration / 10), error)) {
            throw Error("合成 GIF 失败: " + error);
        }
        sink.file_ok(input, {path_to_utf8(out)});
    } catch (const std::exception& e) {
        sink.file_error(input, e.what());
    }
}

}  // namespace fc