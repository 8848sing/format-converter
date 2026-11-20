#pragma once

/*
 * MP3 编码（shine，LGPL-2.0-only）。
 *
 * Android 系统只带 AAC/Opus 编码器，没有 MP3；桌面端一直用 FFmpeg 转 MP3。
 * 这里把 shine 编进来，让手机端也能输出 MP3。
 *
 * 限制（来自 shine 本身）：采样率 32 / 44.1 / 48 kHz，单声道或双声道，CBR。
 */

#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

/* 把交错的 16-bit PCM 编码成 MP3 文件。 */
bool encode_pcm_to_mp3(const std::vector<int16_t>& pcm, int sample_rate, int channels,
                       int bitrate_kbps, const fs::path& out_path, std::string& error);

/* 从 PCM 原始文件（16-bit little-endian，交错）编码成 MP3。 */
bool encode_pcm_file_to_mp3(const fs::path& pcm_path, int sample_rate, int channels,
                            int bitrate_kbps, const fs::path& out_path, std::string& error);

}  // namespace fc