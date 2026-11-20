#include "media.h"

#include "fs_utils.h"
#include "process_util.h"

#include <algorithm>
#include <sstream>

namespace fc {

namespace {

std::vector<std::string> codec_args(const std::string& target, bool hw_h264) {
    if (target == "mp3") {
        return {"-vn", "-c:a", "libmp3lame", "-q:a", "2"};
    }
    if (target == "wav") {
        return {"-vn", "-c:a", "pcm_s16le"};
    }
    if (target == "flac") {
        return {"-vn", "-c:a", "flac", "-compression_level", "8"};
    }
    if (target == "m4a" || target == "aac") {
        return {"-vn", "-c:a", "aac", "-b:a", "192k"};
    }
    if (target == "ogg") {
        return {"-vn", "-c:a", "libvorbis", "-q:a", "5"};
    }
    if (target == "opus") {
        return {"-vn", "-c:a", "libopus", "-b:a", "128k"};
    }
    if (target == "wma") {
        return {"-vn", "-c:a", "wmav2", "-b:a", "192k"};
    }
    if (target == "mp4" || target == "m4v" || target == "mkv" || target == "mov") {
        std::vector<std::string> args;
        if (hw_h264) {
            // GPU 硬件编码：1080p30 实测从 6.4 s 降到 2.7 s，同画质的 SSIM 也高于
            // 旧的 libopenh264 3500k。-cq 28 是画质/体积的平衡点；maxrate 兜住
            // 极端素材，避免输出文件比源文件还大。
            args = {"-c:v", "h264_nvenc", "-preset", "p5", "-tune", "hq",
                    "-rc", "vbr", "-cq", "28", "-b:v", "0",
                    "-maxrate", "16M", "-bufsize", "32M"};
        } else {
            args = {"-c:v", "libopenh264", "-b:v", "3500k"};
        }
        args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k"});
        if (target != "mkv") {
            args.insert(args.end(), {"-movflags", "+faststart"});
        }
        return args;
    }
    if (target == "webm") {
        return {"-c:v", "libvpx-vp9", "-crf", "32", "-b:v", "0", "-c:a", "libopus", "-b:a", "128k"};
    }
    if (target == "avi") {
        return {"-c:v", "mpeg4", "-q:v", "4", "-c:a", "libmp3lame", "-q:a", "4"};
    }
    if (target == "wmv") {
        return {"-c:v", "wmv2", "-b:v", "3000k", "-c:a", "wmav2", "-b:a", "192k"};
    }
    if (target == "gif") {
        return {"-an", "-vf",
                "fps=12,scale=960:-2:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=256[p];"
                "[s1][p]paletteuse=dither=sierra2_4a"};
    }
    throw Error("不支持的音视频目标格式：" + target);
}

bool is_h264_container(const std::string& target) {
    return target == "mp4" || target == "m4v" || target == "mkv" || target == "mov";
}

// 检查这一份 FFmpeg 是否自带某个编码器（例如显卡驱动提供的 h264_nvenc）。
bool supports_encoder(const fs::path& ffmpeg, const std::string& name) {
    ProcessResult probe = run_process(
        {path_to_utf8(ffmpeg), "-hide_banner", "-encoders"}, 20000);
    if (probe.exit_code != 0) {
        return false;
    }
    return probe.out.find(" " + name + " ") != std::string::npos;
}


std::string last_error_line(const ProcessResult& result) {
    std::string detail = trim(result.err.empty() ? result.out : result.err);
    if (detail.empty()) {
        return "FFmpeg 退出码 " + std::to_string(result.exit_code);
    }
    std::istringstream stream(detail);
    std::string line;
    std::string last;
    while (std::getline(stream, line)) {
        if (!trim(line).empty()) {
            last = trim(line);
        }
    }
    return last.empty() ? detail : last;
}

}  // namespace

fs::path find_ffmpeg(const Job& job) {
    fs::path base = job.base_dir;
    std::vector<fs::path> candidates;
    if (job.settings.contains("ffmpeg") && job.settings["ffmpeg"].is_string()) {
        std::string value = job.settings["ffmpeg"].get<std::string>();
        if (!value.empty()) {
            fs::path configured = path_from_utf8(value);
            if (configured.is_relative()) {
                configured = base / configured;
            }
            candidates.push_back(configured);
        }
    }
    candidates.push_back(base / "tools" / "FFmpeg" / "ffmpeg.exe");
    candidates.push_back(base / "tools" / "ffmpeg" / "ffmpeg.exe");
    candidates.push_back(base / "tools" / "ffmpeg.exe");
    candidates.push_back(base / "tools" / "ffmpeg");
    fs::path found = find_executable("ffmpeg", candidates);
    if (!found.empty()) {
        return found;
    }
    throw Error("未找到 FFmpeg。请确认 tools/FFmpeg/ffmpeg.exe 存在，或在 settings.json 中配置 ffmpeg 路径。");
}

void convert_media(const Job& job, EventSink& sink) {
    std::string target = lower_ascii(job.target_or("mp3"));
    static const char* audio[] = {"mp3", "wav", "flac", "m4a", "aac", "ogg", "opus", "wma"};
    static const char* video[] = {"mp4", "m4v", "mkv", "mov", "webm", "avi", "wmv", "gif"};
    bool supported = false;
    for (const char* value : audio) {
        supported = supported || target == value;
    }
    for (const char* value : video) {
        supported = supported || target == value;
    }
    if (!supported) {
        throw Error("不支持的音视频目标格式：" + target);
    }

    fs::path ffmpeg;
    try {
        ffmpeg = find_ffmpeg(job);
    } catch (const std::exception& e) {
        for (const fs::path& input : job.inputs) {
            sink.file_error(path_to_utf8(input), e.what());
        }
        return;
    }

    bool nvenc_ready = false;
    bool nvenc_checked = false;
    auto use_nvenc = [&]() {
        if (!nvenc_checked) {
            nvenc_ready = is_h264_container(target) && supports_encoder(ffmpeg, "h264_nvenc");
            nvenc_checked = true;
        }
        return nvenc_ready;
    };

    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        fs::path temp;
        try {
            fs::path out = unique_output(job.out_dir, stem_utf8(input), "." + target);
            temp = path_join_utf8(out.parent_path(),
                                  stem_utf8(out) + ".part" + path_to_utf8(out.extension()));
            auto build_args = [&](bool hw) {
                std::vector<std::string> args = {
                    path_to_utf8(ffmpeg), "-hide_banner", "-nostdin", "-y", "-i", input_text,
                };
                std::vector<std::string> codec = codec_args(target, hw);
                args.insert(args.end(), codec.begin(), codec.end());
                args.push_back(path_to_utf8(temp));
                return args;
            };
            bool hw = use_nvenc();
            ProcessResult result = run_process(build_args(hw), 7200 * 1000);
            if (result.exit_code != 0 && hw) {
                // 远程桌面、驱动版本不匹配或显卡被占用时 NVENC 会初始化失败，
                // 这里退回软件编码重跑一次，用户不会看到「明明别的视频能转」的失败。
                remove_file_quiet(temp);
                result = run_process(build_args(false), 7200 * 1000);
            }
            if (result.timed_out) {
                throw Error("转换超时（超过 2 小时）");
            }
            if (result.exit_code != 0) {
                throw Error("转换失败：" + last_error_line(result));
            }
            if (!file_exists(temp) || file_size_bytes(temp) == 0) {
                throw Error("FFmpeg 未生成输出文件");
            }
            replace_file(temp, out);
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            if (!temp.empty()) {
                remove_file_quiet(temp);
            }
            sink.file_error(input_text, e.what());
        }
    }
}

}  // namespace fc