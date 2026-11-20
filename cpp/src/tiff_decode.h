#pragma once

/*
 * 内置 TIFF 解码器（基线子集）。
 *
 * Android 系统没有 TIFF 解码器，桌面端虽然有 WIC，但换个环境就不一定可用；
 * 这里自己实现常见的那部分，保证 .tif/.tiff 在两端都能读。
 *
 * 支持范围：
 *  - 基线 TIFF，单 IFD，未分块（strips）
 *  - 压缩：无压缩 / LZW / PackBits / Deflate(zip)
 *  - 8 位样本；灰度 / RGB / 调色板
 *  - 水平差分 Predictor=2
 * 不支持：分块(tiles)、16 位、CMYK、多页、浮点样本。
 */

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

/* 解码成功返回 true 并填充 RGBA8（逐行从上到下）；失败时 error 为可读原因。 */
bool decode_tiff(const std::vector<uint8_t>& bytes, int& width, int& height,
                 std::vector<uint8_t>& rgba, std::string& error);

}  // namespace fc