#include "registry.h"

#include "data.h"
#include "docx_text.h"
#include "fs_utils.h"
#include "image.h"
#include "media.h"
#include "office.h"
#include "pdf_ops.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

namespace fc {

const char* const CAT_IMG = "图片";
const char* const CAT_PDF = "PDF";
const char* const CAT_WORD = "Word";
const char* const CAT_EXCEL = "Excel";
const char* const CAT_PPT = "PPT";
const char* const CAT_TEXT = "文本";
const char* const CAT_MEDIA = "音视频";
const char* const CAT_DATA = "数据";

const std::vector<std::string> IMAGE_EXTS = {
    ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff", ".gif",
    ".ico", ".avif", ".heic", ".heif", ".ppm", ".pgm", ".tga",
};
const std::vector<std::string> PDF_EXTS = {".pdf"};
const std::vector<std::string> WORD_EXTS = {".doc", ".docx", ".rtf", ".odt"};
const std::vector<std::string> EXCEL_EXTS = {".xls", ".xlsx", ".ods", ".csv"};
const std::vector<std::string> PPT_EXTS = {".ppt", ".pptx", ".odp"};
const std::vector<std::string> TEXT_EXTS = {".txt", ".md"};
const std::vector<std::string> MEDIA_EXTS = {
    ".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg", ".opus", ".wma", ".aiff", ".amr",
    ".mp4", ".m4v", ".mkv", ".mov", ".webm", ".avi", ".wmv", ".flv", ".ts", ".mts", ".m2ts", ".3gp",
};
const std::vector<std::string> DATA_EXTS = {".json", ".yaml", ".yml", ".xml"};
const std::vector<std::string> TABLE_DATA_EXTS = {".xlsx", ".csv"};

namespace {

const std::vector<std::string> IMG_TARGETS = {"png", "jpg", "jpeg", "webp", "avif", "heic",
                                              "ico", "tiff", "bmp", "gif", "tga", "ppm"};
const std::vector<std::string> AUDIO_TARGETS = {"mp3", "wav", "flac", "m4a", "aac", "ogg", "opus", "wma"};
const std::vector<std::string> VIDEO_TARGETS = {"mp4", "m4v", "mkv", "mov", "webm", "avi", "wmv", "gif"};

using ExportMap = std::map<std::string, std::pair<std::string, std::string>>;

const ExportMap& word_exports() {
    static const ExportMap map = {
        {"pdf", {"pdf", ".pdf"}},
        {"docx", {"docx:MS Word 2007 XML", ".docx"}},
        {"doc", {"doc:MS Word 97", ".doc"}},
        {"odt", {"odt:writer8", ".odt"}},
        {"rtf", {"rtf", ".rtf"}},
        {"txt", {"txt:Text (encoded):UTF8", ".txt"}},
    };
    return map;
}

const ExportMap& excel_exports() {
    static const ExportMap map = {
        {"pdf", {"pdf", ".pdf"}},
        {"xlsx", {"xlsx:Calc MS Excel 2007 XML", ".xlsx"}},
        {"xls", {"xls:MS Excel 97", ".xls"}},
        {"ods", {"ods:calc8", ".ods"}},
        {"csv", {"csv:Text - txt - csv (StarCalc)", ".csv"}},
    };
    return map;
}

const ExportMap& ppt_exports() {
    static const ExportMap map = {
        {"pdf", {"pdf", ".pdf"}},
        {"pptx", {"pptx:Impress MS PowerPoint 2007 XML", ".pptx"}},
        {"ppt", {"ppt:MS PowerPoint 97", ".ppt"}},
        {"odp", {"odp:impress8", ".odp"}},
    };
    return map;
}

const ExportMap& text_exports() {
    static const ExportMap map = {
        {"pdf", {"pdf", ".pdf"}},
        {"docx", {"docx:MS Word 2007 XML", ".docx"}},
        {"doc", {"doc:MS Word 97", ".doc"}},
        {"odt", {"odt:writer8", ".odt"}},
        {"rtf", {"rtf", ".rtf"}},
    };
    return map;
}

Handler export_handler(const std::string& filter, const std::string& out_ext) {
    return [filter, out_ext](const Job& job, EventSink& sink) {
        office_export_job(job, sink, filter, out_ext);
    };
}

Handler export_choice(const ExportMap& map, const std::string& fallback) {
    return [map, fallback](const Job& job, EventSink& sink) {
        std::string target = lower_ascii(job.target_or(fallback));
        auto it = map.find(target);
        if (it == map.end()) {
            it = map.find(fallback);
        }
        if (it == map.end()) {
            throw Error("不支持的目标格式：" + target);
        }
        office_export_job(job, sink, it->second.first, it->second.second);
    };
}

ConversionSpec make_spec(std::string id, std::string label, std::string src, std::string dst,
                         std::vector<std::string> extensions, Handler handler,
                         std::vector<std::string> accepts = {},
                         std::string accepts_label = {}, bool per_file = true,
                         bool batch_report = false, int workers = 1,
                         std::string description = {}) {
    ConversionSpec spec;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.src = std::move(src);
    spec.dst = std::move(dst);
    spec.extensions = std::move(extensions);
    spec.handler = std::move(handler);
    spec.accepts = std::move(accepts);
    spec.accepts_label = std::move(accepts_label);
    spec.per_file = per_file;
    spec.batch_report = batch_report;
    spec.workers = workers;
    spec.description = std::move(description);
    return spec;
}

const std::vector<ConversionSpec>& build_registry() {
    static const std::vector<ConversionSpec> registry = [] {
        std::vector<ConversionSpec> list;

        list.push_back(make_spec(
            "image_convert", "图片格式互转", CAT_IMG, CAT_IMG, IMAGE_EXTS, convert_image_format,
            IMG_TARGETS, "目标格式", true, false, 4,
            "支持 JPG / PNG / WebP / AVIF / HEIC / ICO / TIFF / GIF 等；JPEG 采用高质量编码。"));

        list.push_back(make_spec(
            "image_to_pdf", "图片 转 PDF（每张单独一个）", CAT_IMG, CAT_PDF, IMAGE_EXTS,
            images_to_pdf, {}, "", true, false, 4,
            "每张图生成一个同名 PDF。"));

        list.push_back(make_spec(
            "images_merge_pdf", "多张图片 合并为一个 PDF", CAT_IMG, CAT_PDF, IMAGE_EXTS,
            merge_images_to_pdf, {}, "", false, false, 1,
            "按文件列表顺序合并为一个多页 PDF。"));

        list.push_back(make_spec(
            "pdf_to_image", "PDF 转图片（每页一张）", CAT_PDF, CAT_IMG, PDF_EXTS, pdf_to_images,
            IMG_TARGETS, "目标格式", true, false, 1,
            "PDFium 渲染，默认 200 DPI。"));

        list.push_back(make_spec(
            "pdf_to_word", "PDF 转 Word（可编辑 docx）", CAT_PDF, CAT_WORD, PDF_EXTS, pdf_to_word,
            {}, "", true, false, 1,
            "提取文字层生成 docx；扫描件需要 OCR。"));

        list.push_back(make_spec(
            "pdf_to_excel", "PDF 转 Excel", CAT_PDF, CAT_EXCEL, PDF_EXTS, pdf_to_excel, {}, "",
            true, false, 1, "每页文字写入独立工作表。"));

        list.push_back(make_spec(
            "pdf_to_ppt", "PDF 转 PPT（整页图片版）", CAT_PDF, CAT_PPT, PDF_EXTS, pdf_to_ppt, {},
            "", true, false, 1, "每页渲染为全页图片插入幻灯片，文本不可编辑。"));

        list.push_back(make_spec(
            "pdf_to_text", "PDF 转 纯文本", CAT_PDF, CAT_TEXT, PDF_EXTS, pdf_to_text, {}, "", true,
            false, 1, "提取文字层，输出 UTF-8 .txt；扫描件无文字层时为空。"));

        list.push_back(make_spec(
            "word_to_pdf", "Word 转 PDF", CAT_WORD, CAT_PDF, WORD_EXTS,
            export_handler("pdf", ".pdf"), {}, "", false, true, 1,
            "内置 LibreOffice；整批共用进程启动。"));

        list.push_back(make_spec(
            "word_to_image", "Word 转图片（每页一张）", CAT_WORD, CAT_IMG, WORD_EXTS,
            office_to_images, IMG_TARGETS, "目标格式", true, false, 1,
            "先转 PDF 再逐页渲染，保留分页。"));

        list.push_back(make_spec(
            "word_convert", "Word 转 旧版/其他文档格式", CAT_WORD, CAT_WORD, WORD_EXTS,
            export_choice(word_exports(), "docx"), {"docx", "doc", "odt", "rtf"}, "目标格式",
            false, true, 1, "doc ↔ docx / odt / rtf 任意互转，内置 LibreOffice。"));

        list.push_back(make_spec(
            "word_to_text", "Word 转 纯文本", CAT_WORD, CAT_TEXT, WORD_EXTS,
            [](const Job& job, EventSink& sink) {
                // 默认策略：.docx 直接用内置提取（几十毫秒，不启动 LibreOffice）；
                // .doc / .rtf / .odt 这类没有内置解析器的格式才交给 soffice。
                // 想恢复「优先高保真导出」时，在 settings.json 写
                //   "word_text_engine": "libreoffice"
                // 无论哪种策略，LibreOffice 一个文件都没导出成功时都会回退内置提取。
                std::string engine = "builtin";
                if (job.settings.contains("word_text_engine") &&
                    job.settings["word_text_engine"].is_string()) {
                    engine = lower_ascii(job.settings["word_text_engine"].get<std::string>());
                }

                class ProbeSink : public EventSink {
                public:
                    explicit ProbeSink(EventSink& inner) : inner_(inner) {}
                    void file_ok(const std::string& input,
                                 const std::vector<std::string>& outputs,
                                 const std::string& message) override {
                        ++ok;
                        inner_.file_ok(input, outputs, message);
                    }
                    void file_error(const std::string& input,
                                    const std::string& error) override {
                        ++failed;
                        failures.emplace_back(input, error);
                    }
                    void message(const std::string& text) override {
                        inner_.message(text);
                    }
                    int ok = 0;
                    int failed = 0;
                    std::vector<std::pair<std::string, std::string>> failures;

                private:
                    EventSink& inner_;
                };

                if (engine == "libreoffice" && has_soffice(job)) {
                    ProbeSink probe(sink);
                    office_export_job(job, probe, "txt:Text (encoded):UTF8", ".txt");
                    if (probe.ok > 0 || probe.failed == 0) {
                        return;
                    }
                    Job retry = job;
                    retry.inputs.clear();
                    for (const fs::path& input : job.inputs) {
                        if (extension_lower(input) == ".docx") {
                            retry.inputs.push_back(input);
                        }
                    }
                    sink.message("LibreOffice 导出失败，已改用内置文本提取");
                    if (!retry.inputs.empty()) {
                        docx_to_text_job(retry, sink);
                    }
                    for (const auto& failure : probe.failures) {
                        if (extension_lower(path_from_utf8(failure.first)) != ".docx") {
                            sink.file_error(failure.first, failure.second);
                        }
                    }
                    return;
                }

                std::vector<fs::path> docx_inputs;
                std::vector<fs::path> other_inputs;
                for (const fs::path& input : job.inputs) {
                    if (extension_lower(input) == ".docx") {
                        docx_inputs.push_back(input);
                    } else {
                        other_inputs.push_back(input);
                    }
                }
                if (!docx_inputs.empty()) {
                    Job part = job;
                    part.inputs = docx_inputs;
                    docx_to_text_job(part, sink);
                }
                if (other_inputs.empty()) {
                    return;
                }
                if (!has_soffice(job)) {
                    for (const fs::path& input : other_inputs) {
                        sink.file_error(
                            path_to_utf8(input),
                            "内置提取只支持 .docx；请安装 LibreOffice，或先把文件另存为 .docx");
                    }
                    return;
                }
                Job part = job;
                part.inputs = other_inputs;
                ProbeSink probe(sink);
                office_export_job(part, probe, "txt:Text (encoded):UTF8", ".txt");
                for (const auto& failure : probe.failures) {
                    sink.file_error(failure.first, failure.second);
                }
            },
            {}, "", false, false, 1,
            "导出 UTF-8 文本：.docx 走内置提取（快），其它格式走 LibreOffice。"));

        list.push_back(make_spec(
            "excel_to_pdf", "Excel 转 PDF", CAT_EXCEL, CAT_PDF, EXCEL_EXTS,
            export_handler("pdf", ".pdf"), {}, "", false, true, 1,
            "内置 LibreOffice；整批共用进程启动。"));

        list.push_back(make_spec(
            "excel_to_image", "Excel 转图片（每页一张）", CAT_EXCEL, CAT_IMG, EXCEL_EXTS,
            office_to_images, IMG_TARGETS, "目标格式", true, false, 1,
            "先转 PDF 再逐页渲染。打印区域外的内容可能不被包含。"));

        list.push_back(make_spec(
            "excel_convert", "Excel 转 其他表格格式", CAT_EXCEL, CAT_EXCEL, EXCEL_EXTS,
            export_choice(excel_exports(), "xlsx"), {"xlsx", "xls", "ods", "csv"}, "目标格式",
            false, true, 1, "xls ↔ xlsx / ods / csv 任意互转，内置 LibreOffice。"));

        list.push_back(make_spec(
            "ppt_to_pdf", "PPT 转 PDF", CAT_PPT, CAT_PDF, PPT_EXTS, export_handler("pdf", ".pdf"),
            {}, "", false, true, 1, "内置 LibreOffice；整批共用进程启动。"));

        list.push_back(make_spec(
            "ppt_to_image", "PPT 转图片（每页一张）", CAT_PPT, CAT_IMG, PPT_EXTS, office_to_images,
            IMG_TARGETS, "目标格式", true, false, 1, "先转 PDF 再逐页渲染，每张幻灯片一页图。"));

        list.push_back(make_spec(
            "ppt_convert", "PPT 转 旧版/其他演示格式", CAT_PPT, CAT_PPT, PPT_EXTS,
            export_choice(ppt_exports(), "pptx"), {"pptx", "ppt", "odp"}, "目标格式", false, true,
            1, "ppt ↔ pptx / odp 任意互转，内置 LibreOffice。"));

        list.push_back(make_spec(
            "text_to_pdf", "文本 转 PDF", CAT_TEXT, CAT_PDF, TEXT_EXTS,
            export_handler("pdf", ".pdf"), {}, "", false, true, 1,
            "txt / md 直接排版输出 PDF，内置 LibreOffice。"));

        list.push_back(make_spec(
            "text_to_word", "文本 转 Word 文档", CAT_TEXT, CAT_WORD, TEXT_EXTS,
            export_choice(text_exports(), "docx"), {"docx", "doc", "odt", "rtf"}, "目标格式",
            false, true, 1, "txt / md 转为 Word 系列文档，内置 LibreOffice。"));

        list.push_back(make_spec(
            "images_to_gif", "多张图片 合成动图 GIF", CAT_IMG, CAT_IMG, IMAGE_EXTS,
            images_to_animated_gif, {}, "", false, false, 1,
            "按文件队列顺序合成循环 GIF，最长边限制为 1280 像素。"));

        list.push_back(make_spec(
            "pdf_merge", "PDF 合并（多个文件 → 一个）", CAT_PDF, CAT_PDF, PDF_EXTS, pdf_merge, {},
            "", false, false, 1, "按文件队列顺序合并全部 PDF。"));

        list.push_back(make_spec(
            "pdf_split", "PDF 拆分（一页一个文件）", CAT_PDF, CAT_PDF, PDF_EXTS, pdf_split, {},
            "", true, false, 1, "每个 PDF 的每一页导出为独立文件。"));

        list.push_back(make_spec(
            "pdf_rotate", "PDF 旋转页面", CAT_PDF, CAT_PDF, PDF_EXTS, pdf_rotate, {"90", "180", "270"},
            "旋转角度", true, false, 1, "顺时针旋转全部页面，不改变原始内容。"));

        list.push_back(make_spec(
            "pdf_compress", "PDF 压缩（内容流）", CAT_PDF, CAT_PDF, PDF_EXTS, pdf_compress, {},
            "", true, false, 1, "重写 PDF 结构并尽量压缩内容流；不会重新压缩已嵌入的高清图片。"));

        list.push_back(make_spec(
            "audio_convert", "音频转音频", CAT_MEDIA, CAT_MEDIA, {
                ".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg", ".opus", ".wma", ".aiff", ".amr"},
            convert_media, AUDIO_TARGETS, "目标格式", true, false, 2,
            "使用 FFmpeg 转 MP3 / WAV / FLAC / M4A / AAC / OGG / OPUS / WMA。"));

        std::vector<std::string> video_targets = VIDEO_TARGETS;
        video_targets.insert(video_targets.end(), AUDIO_TARGETS.begin(), AUDIO_TARGETS.end());
        list.push_back(make_spec(
            "video_convert", "视频转视频 / 提取音频", CAT_MEDIA, CAT_MEDIA, {
                ".mp4", ".m4v", ".mkv", ".mov", ".webm", ".avi", ".wmv", ".flv", ".ts", ".mts", ".m2ts", ".3gp"},
            convert_media, video_targets, "目标格式", true, false, 2,
            "支持 MP4 / MKV / MOV / WebM / AVI / WMV / GIF，也可选音频格式提取音轨。"));

        list.push_back(make_spec(
            "data_convert", "结构化数据互转", CAT_DATA, CAT_DATA, DATA_EXTS, convert_structured,
            {"json", "yaml", "xml", "xlsx"}, "目标格式", true, false, 4,
            "JSON / YAML / XML 互转，也可把结构化数据展开为 Excel 表格。"));

        list.push_back(make_spec(
            "data_to_excel", "结构化数据 转 Excel", CAT_DATA, CAT_EXCEL, DATA_EXTS,
            [](const Job& job, EventSink& sink) {
                Job copy = job;
                copy.target = "xlsx";
                convert_structured(copy, sink);
            },
            {"xlsx"}, "目标格式", true, false, 4,
            "自动展开对象字段，嵌套字段使用点号命名；数组转为多行。"));

        list.push_back(make_spec(
            "table_to_json", "Excel / CSV 转 JSON", CAT_EXCEL, CAT_DATA, TABLE_DATA_EXTS,
            convert_table_to_json, {}, "", true, false, 2,
            "读取当前工作表首行作为字段名，输出 UTF-8 JSON 数组。"));

        return list;
    }();
    return registry;
}

}  // namespace

const std::vector<ConversionSpec>& conversions() {
    return build_registry();
}

const ConversionSpec* find_conversion(const std::string& id) {
    for (const ConversionSpec& spec : conversions()) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<std::string> categories_src() {
    std::vector<std::string> result;
    for (const ConversionSpec& spec : conversions()) {
        if (std::find(result.begin(), result.end(), spec.src) == result.end()) {
            result.push_back(spec.src);
        }
    }
    return result;
}

std::vector<std::string> dsts_for(const std::string& src) {
    std::vector<std::string> result;
    for (const ConversionSpec& spec : conversions()) {
        if (spec.src == src && std::find(result.begin(), result.end(), spec.dst) == result.end()) {
            result.push_back(spec.dst);
        }
    }
    return result;
}

std::vector<const ConversionSpec*> convs_for(const std::string& src, const std::string& dst) {
    std::vector<const ConversionSpec*> result;
    for (const ConversionSpec& spec : conversions()) {
        if (spec.src == src && spec.dst == dst) {
            result.push_back(&spec);
        }
    }
    return result;
}

bool pair_exists(const std::string& src, const std::string& dst) {
    return !convs_for(src, dst).empty();
}

std::string registry_json() {
    static std::once_flag once;
    static std::string cached;
    std::call_once(once, [] {
        json array = json::array();
        for (const ConversionSpec& spec : conversions()) {
            json item;
            item["id"] = spec.id;
            item["label"] = spec.label;
            item["src"] = spec.src;
            item["dst"] = spec.dst;
            item["extensions"] = spec.extensions;
            item["accepts"] = spec.accepts;
            item["accepts_label"] = spec.accepts_label;
            item["per_file"] = spec.per_file;
            item["batch_report"] = spec.batch_report;
            item["workers"] = spec.workers;
            item["description"] = spec.description;
            array.push_back(std::move(item));
        }
        cached = array.dump(2, ' ', false, json::error_handler_t::replace);
    });
    return cached;
}

}  // namespace fc