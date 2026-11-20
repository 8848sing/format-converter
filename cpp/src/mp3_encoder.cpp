#include "mp3_encoder.h"

#include "fs_utils.h"

extern "C" {
#include "layer3.h"
}

#include <cstdio>
#include <cstring>
#include <fstream>

namespace fc {

namespace {

bool write_bytes(std::ofstream& out, const unsigned char* data, int count, std::string& error) {
    if (count <= 0 || data == nullptr) {
        return true;
    }
    out.write(reinterpret_cast<const char*>(data), count);
    if (!out) {
        error = "写入 MP3 失败（磁盘空间不足？）";
        return false;
    }
    return true;
}

}  // namespace

bool encode_pcm_to_mp3(const std::vector<int16_t>& pcm, int sample_rate, int channels,
                       int bitrate_kbps, const fs::path& out_path, std::string& error) {
    if (channels != 1 && channels != 2) {
        error = "MP3 编码只支持单声道或双声道";
        return false;
    }
    if (shine_check_config(sample_rate, bitrate_kbps) < 0) {
        error = "MP3 编码不支持 " + std::to_string(sample_rate) + " Hz / " +
                std::to_string(bitrate_kbps) + " kbps（支持 32 / 44.1 / 48 kHz）";
        return false;
    }

    shine_config_t config;
    std::memset(&config, 0, sizeof(config));
    config.wave.channels = (channels == 2) ? PCM_STEREO : PCM_MONO;
    config.wave.samplerate = sample_rate;
    shine_set_config_mpeg_defaults(&config.mpeg);
    config.mpeg.bitr = bitrate_kbps;
    config.mpeg.mode = (channels == 2) ? STEREO : MONO;

    shine_t encoder = shine_initialise(&config);
    if (!encoder) {
        error = "MP3 编码器初始化失败";
        return false;
    }

    const int samples_per_pass = shine_samples_per_pass(encoder);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        shine_close(encoder);
        error = "无法创建 MP3 文件";
        return false;
    }

    const size_t total_frames = pcm.size() / static_cast<size_t>(channels);
    size_t frame = 0;
    bool ok = true;
    while (frame < total_frames) {
        const int frames = static_cast<int>(
                std::min<size_t>(static_cast<size_t>(samples_per_pass), total_frames - frame));
        // shine 期望每次拿到「一帧 1152 个采样」；最后一帧补零。
        std::vector<int16_t> block(static_cast<size_t>(samples_per_pass) * channels, 0);
        std::memcpy(block.data(), pcm.data() + frame * channels,
                    static_cast<size_t>(frames) * channels * sizeof(int16_t));
        int written = 0;
        unsigned char* data = shine_encode_buffer_interleaved(encoder, block.data(), &written);
        if (!write_bytes(out, data, written, error)) {
            ok = false;
            break;
        }
        frame += static_cast<size_t>(frames);
    }
    if (ok) {
        int written = 0;
        unsigned char* data = shine_flush(encoder, &written);
        ok = write_bytes(out, data, written, error);
    }

    out.flush();
    out.close();
    shine_close(encoder);
    if (!ok) {
        remove_file_quiet(out_path);
    }
    return ok;
}

bool encode_pcm_file_to_mp3(const fs::path& pcm_path, int sample_rate, int channels,
                            int bitrate_kbps, const fs::path& out_path, std::string& error) {
    std::vector<uint8_t> bytes;
    try {
        bytes = read_binary_file(pcm_path);
    } catch (const std::exception& e) {
        error = std::string("读取 PCM 数据失败：") + e.what();
        return false;
    }
    if (bytes.size() < sizeof(int16_t) || bytes.size() % sizeof(int16_t) != 0) {
        error = "PCM 数据长度无效";
        return false;
    }
    std::vector<int16_t> pcm(bytes.size() / sizeof(int16_t));
    std::memcpy(pcm.data(), bytes.data(), bytes.size());
    return encode_pcm_to_mp3(pcm, sample_rate, channels, bitrate_kbps, out_path, error);
}

}  // namespace fc