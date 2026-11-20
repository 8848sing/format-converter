#include "tiff_decode.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>

namespace fc {

namespace {

struct Reader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool little = true;

    uint16_t u16(size_t offset) const {
        if (offset + 2 > size) {
            return 0;
        }
        return little ? static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8))
                      : static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
    }

    uint32_t u32(size_t offset) const {
        if (offset + 4 > size) {
            return 0;
        }
        if (little) {
            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<uint32_t>(data[offset + 3]) << 24);
        }
        return (static_cast<uint32_t>(data[offset]) << 24) |
               (static_cast<uint32_t>(data[offset + 1]) << 16) |
               (static_cast<uint32_t>(data[offset + 2]) << 8) |
               static_cast<uint32_t>(data[offset + 3]);
    }

    uint32_t value_at(size_t offset, uint16_t type) const {
        return type == 3 ? u16(offset) : u32(offset);
    }
};

struct Tags {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bits_per_sample = 8;
    uint32_t compression = 1;
    uint32_t photometric = 2;
    uint32_t samples_per_pixel = 1;
    uint32_t rows_per_strip = 0;
    uint32_t predictor = 1;
    uint32_t planar = 1;
    uint32_t extra_samples = 0;
    std::vector<uint32_t> strip_offsets;
    std::vector<uint32_t> strip_counts;
    std::vector<uint32_t> color_map;
};

bool read_values(const Reader& r, uint32_t offset, uint16_t type, uint32_t count,
                 uint32_t inline_bytes, std::vector<uint32_t>& out) {
    const size_t item_size = type == 3 ? 2 : 4;
    const size_t needed = static_cast<size_t>(item_size) * count;
    const size_t base = needed > inline_bytes ? offset : offset;  // offset 已指向数据或内联值
    if (needed > r.size || base + needed > r.size) {
        return false;
    }
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back(r.value_at(base + static_cast<size_t>(i) * item_size, type));
    }
    return true;
}

/* TIFF 的 LZW：9 位起，MSB 优先，ClearCode=256，EOI=257。 */
bool lzw_decode(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size,
                std::string& error) {
    constexpr int kClear = 256;
    constexpr int kEoi = 257;
    constexpr int kMaxCodes = 4096;

    std::vector<std::vector<uint8_t>> dict(kMaxCodes);
    auto reset_dict = [&]() {
        for (int i = 0; i < 256; ++i) {
            dict[i].assign(1, static_cast<uint8_t>(i));
        }
        dict[kClear].clear();
        dict[kEoi].clear();
    };
    reset_dict();

    int code_size = 9;
    int next_code = 258;
    std::vector<uint8_t> previous;
    size_t out_pos = 0;
    uint32_t bit_buffer = 0;
    int bit_count = 0;
    size_t in_pos = 0;

    while (true) {
        while (bit_count < code_size) {
            if (in_pos >= src_size) {
                break;
            }
            bit_buffer = (bit_buffer << 8) | src[in_pos++];
            bit_count += 8;
        }
        if (bit_count < code_size) {
            break;  // 数据结束（没有显式 EOI 也接受）
        }
        const int code = static_cast<int>((bit_buffer >> (bit_count - code_size)) &
                                          ((1u << code_size) - 1));
        bit_count -= code_size;

        if (code == kEoi) {
            break;
        }
        if (code == kClear) {
            reset_dict();
            code_size = 9;
            next_code = 258;
            previous.clear();
            continue;
        }
        // code == next_code 是合法的 KwKwK 情况（字典刚满一格，串还没真正存在）
        if (code > next_code || code >= kMaxCodes) {
            error = "TIFF 的 LZW 压缩流损坏，无法解码";
            return false;
        }

        std::vector<uint8_t> entry;
        if (code < next_code && !dict[code].empty()) {
            entry = dict[code];
        } else if (!previous.empty()) {
            entry = previous;
            entry.push_back(previous.front());
        } else {
            error = "TIFF LZW 数据损坏";
            return false;
        }

        if (out_pos + entry.size() > dst_size) {
            break;  // 多出来的补齐字节忽略
        }
        std::memcpy(dst + out_pos, entry.data(), entry.size());
        out_pos += entry.size();

        if (!previous.empty() && next_code < kMaxCodes) {
            std::vector<uint8_t> combined = previous;
            combined.push_back(entry.front());
            dict[next_code] = std::move(combined);
            ++next_code;
            // libtiff 的规则：下一个可用码到达 (1<<nbits)-1 时，码长加一。
            if (next_code == (1 << code_size) - 1 && code_size < 12) {
                ++code_size;
            }
        }
        previous = std::move(entry);
    }

    if (out_pos < dst_size) {
        std::memset(dst + out_pos, 0, dst_size - out_pos);
    }
    return true;
}

bool packbits_decode(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < src_size && out_pos < dst_size) {
        const int8_t control = static_cast<int8_t>(src[in_pos++]);
        if (control >= 0) {
            const size_t count = static_cast<size_t>(control) + 1;
            for (size_t i = 0; i < count && in_pos < src_size && out_pos < dst_size; ++i) {
                dst[out_pos++] = src[in_pos++];
            }
        } else if (control != -128) {
            const size_t count = static_cast<size_t>(1 - control);
            if (in_pos >= src_size) {
                break;
            }
            const uint8_t value = src[in_pos++];
            for (size_t i = 0; i < count && out_pos < dst_size; ++i) {
                dst[out_pos++] = value;
            }
        }
    }
    if (out_pos < dst_size) {
        std::memset(dst + out_pos, 0, dst_size - out_pos);
    }
    return true;
}

bool deflate_decode(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size) {
    mz_ulong out_len = static_cast<mz_ulong>(dst_size);
    const int status = mz_uncompress(dst, &out_len, src, static_cast<mz_ulong>(src_size));
    if (status != MZ_OK) {
        return false;
    }
    if (out_len < dst_size) {
        std::memset(dst + out_len, 0, dst_size - out_len);
    }
    return true;
}

void apply_predictor(std::vector<uint8_t>& raw, uint32_t width, uint32_t samples,
                     uint32_t rows) {
    const size_t row_bytes = static_cast<size_t>(width) * samples;
    for (uint32_t y = 0; y < rows; ++y) {
        uint8_t* row = raw.data() + static_cast<size_t>(y) * row_bytes;
        for (size_t i = samples; i < row_bytes; ++i) {
            row[i] = static_cast<uint8_t>(row[i] + row[i - samples]);
        }
    }
}

}  // namespace

bool decode_tiff(const std::vector<uint8_t>& bytes, int& width, int& height,
                 std::vector<uint8_t>& rgba, std::string& error) {
    if (bytes.size() < 8) {
        error = "TIFF 文件过短";
        return false;
    }
    Reader reader{bytes.data(), bytes.size(), true};
    if (bytes[0] == 'I' && bytes[1] == 'I') {
        reader.little = true;
    } else if (bytes[0] == 'M' && bytes[1] == 'M') {
        reader.little = false;
    } else {
        error = "不是 TIFF 文件";
        return false;
    }
    if (reader.u16(2) != 42) {
        error = "不支持的 TIFF 版本（BigTIFF？）";
        return false;
    }

    Tags tags;
    const uint32_t ifd_offset = reader.u32(4);
    if (ifd_offset + 2 > bytes.size()) {
        error = "TIFF 目录偏移越界";
        return false;
    }
    const uint32_t entry_count = reader.u16(ifd_offset);
    std::vector<uint32_t> values;

    for (uint32_t i = 0; i < entry_count; ++i) {
        const size_t entry = ifd_offset + 2 + static_cast<size_t>(i) * 12;
        if (entry + 12 > bytes.size()) {
            break;
        }
        const uint16_t tag = reader.u16(entry);
        const uint16_t type = reader.u16(entry + 2);
        const uint32_t count = reader.u32(entry + 4);
        if (type != 3 && type != 4) {
            continue;
        }
        const size_t item_size = type == 3 ? 2 : 4;
        const size_t needed = item_size * count;
        const size_t data_pos = needed <= 4 ? entry + 8 : reader.u32(entry + 8);
        if (data_pos + needed > bytes.size() || count > 1000000) {
            continue;
        }
        values.clear();
        values.reserve(count);
        for (uint32_t k = 0; k < count; ++k) {
            values.push_back(reader.value_at(data_pos + static_cast<size_t>(k) * item_size, type));
        }
        switch (tag) {
            case 256: tags.width = values.empty() ? 0 : values[0]; break;
            case 257: tags.height = values.empty() ? 0 : values[0]; break;
            case 258: tags.bits_per_sample = values.empty() ? 8 : values[0]; break;
            case 259: tags.compression = values.empty() ? 1 : values[0]; break;
            case 262: tags.photometric = values.empty() ? 2 : values[0]; break;
            case 273: tags.strip_offsets = values; break;
            case 277: tags.samples_per_pixel = values.empty() ? 1 : values[0]; break;
            case 278: tags.rows_per_strip = values.empty() ? 0 : values[0]; break;
            case 279: tags.strip_counts = values; break;
            case 284: tags.planar = values.empty() ? 1 : values[0]; break;
            case 317: tags.predictor = values.empty() ? 1 : values[0]; break;
            case 320: tags.color_map = values; break;
            case 338: tags.extra_samples = values.empty() ? 0 : values[0]; break;
            default: break;
        }
    }

    if (tags.width == 0 || tags.height == 0) {
        error = "TIFF 缺少尺寸信息";
        return false;
    }
    if (tags.width > 60000 || tags.height > 60000 ||
        static_cast<uint64_t>(tags.width) * tags.height > 400000000ULL) {
        error = "TIFF 尺寸过大";
        return false;
    }
    if (tags.bits_per_sample != 8) {
        error = "暂不支持 " + std::to_string(tags.bits_per_sample) + " 位 TIFF";
        return false;
    }
    if (tags.planar != 1) {
        error = "暂不支持分平面（PlanarConfiguration=2）的 TIFF";
        return false;
    }
    if (tags.samples_per_pixel != 1 && tags.samples_per_pixel != 3 && tags.samples_per_pixel != 4) {
        error = "暂不支持的 TIFF 颜色通道数";
        return false;
    }
    if (tags.strip_offsets.empty()) {
        error = "暂不支持分块(tiles)的 TIFF";
        return false;
    }

    const uint32_t samples = tags.samples_per_pixel;
    const uint32_t rows_per_strip = tags.rows_per_strip == 0 ? tags.height : tags.rows_per_strip;
    const size_t row_bytes = static_cast<size_t>(tags.width) * samples;
    std::vector<uint8_t> raw(row_bytes * tags.height, 0);

    for (size_t strip = 0; strip < tags.strip_offsets.size(); ++strip) {
        const uint32_t offset = tags.strip_offsets[strip];
        const uint32_t count = strip < tags.strip_counts.size() ? tags.strip_counts[strip]
                                                                : static_cast<uint32_t>(bytes.size());
        if (offset >= bytes.size() || count == 0) {
            continue;
        }
        const size_t available = std::min<size_t>(count, bytes.size() - offset);
        const uint32_t first_row = static_cast<uint32_t>(strip) * rows_per_strip;
        if (first_row >= tags.height) {
            break;
        }
        const uint32_t rows = std::min(rows_per_strip, tags.height - first_row);
        const size_t target = static_cast<size_t>(rows) * row_bytes;
        uint8_t* dst = raw.data() + static_cast<size_t>(first_row) * row_bytes;

        if (tags.compression == 1) {
            std::memcpy(dst, bytes.data() + offset, std::min(target, available));
        } else if (tags.compression == 5) {
            if (!lzw_decode(bytes.data() + offset, available, dst, target, error)) {
                return false;
            }
        } else if (tags.compression == 32773) {
            packbits_decode(bytes.data() + offset, available, dst, target);
        } else if (tags.compression == 8 || tags.compression == 32946) {
            if (!deflate_decode(bytes.data() + offset, available, dst, target)) {
                error = "TIFF Deflate 解压失败";
                return false;
            }
        } else {
            error = "暂不支持这种 TIFF 压缩方式（" + std::to_string(tags.compression) + "）";
            return false;
        }
    }

    if (tags.predictor == 2) {
        apply_predictor(raw, tags.width, samples, tags.height);
    } else if (tags.predictor != 1) {
        error = "暂不支持这种 TIFF 预测器（" + std::to_string(tags.predictor) + "）";
        return false;
    }

    width = static_cast<int>(tags.width);
    height = static_cast<int>(tags.height);
    rgba.assign(static_cast<size_t>(width) * height * 4, 255);

    const bool has_alpha = samples == 4;
    for (size_t i = 0, pixels = static_cast<size_t>(width) * height; i < pixels; ++i) {
        const uint8_t* src = raw.data() + i * samples;
        uint8_t* dst = rgba.data() + i * 4;
        if (tags.photometric == 2 || tags.photometric == 3) {
            if (tags.photometric == 3) {
                // 调色板：每通道 width 项，16 位值取高字节
                const size_t map_size = tags.width;
                if (tags.color_map.size() >= map_size * 3) {
                    const uint8_t index = src[0];
                    const size_t at = static_cast<size_t>(index) < map_size ? index : 0;
                    dst[0] = static_cast<uint8_t>(tags.color_map[at] >> 8);
                    dst[1] = static_cast<uint8_t>(tags.color_map[map_size + at] >> 8);
                    dst[2] = static_cast<uint8_t>(tags.color_map[map_size * 2 + at] >> 8);
                } else {
                    dst[0] = dst[1] = dst[2] = src[0];
                }
            } else {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
            dst[3] = has_alpha ? src[3] : 255;
        } else {
            // 灰度：WhiteIsZero 需要取反
            const uint8_t gray = tags.photometric == 0 ? static_cast<uint8_t>(255 - src[0]) : src[0];
            dst[0] = dst[1] = dst[2] = gray;
            dst[3] = has_alpha ? src[1] : 255;
        }
    }
    return true;
}

}  // namespace fc