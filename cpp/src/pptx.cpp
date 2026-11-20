#include "pptx.h"

#include "fs_utils.h"
#include "pptx_templates.h"
#include "zip_writer.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace fc {

namespace {

constexpr int64_t kSlideWidthEmu = 12192000;
constexpr int64_t kSlideHeightEmu = 6858000;

std::string content_types(size_t slide_count) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
        "<Override PartName=\"/ppt/presentation.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
        "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
        "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
        "<Override PartName=\"/ppt/theme/theme1.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
        "<Override PartName=\"/ppt/presProps.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presProps+xml\"/>"
        "<Override PartName=\"/ppt/viewProps.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.viewProps+xml\"/>"
        "<Override PartName=\"/ppt/tableStyles.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.tableStyles+xml\"/>";
    for (size_t i = 0; i < slide_count; ++i) {
        xml += "<Override PartName=\"/ppt/slides/slide" + std::to_string(i + 1) +
               ".xml\" "
               "ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>";
    }
    xml += "</Types>";
    return xml;
}

std::string root_rels() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Id=\"rId1\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
           "Target=\"ppt/presentation.xml\"/>"
           "</Relationships>";
}

std::string presentation_xml(size_t slide_count) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
        "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>"
        "<p:sldIdLst>";
    for (size_t i = 0; i < slide_count; ++i) {
        xml += "<p:sldId id=\"" + std::to_string(256 + i) + "\" r:id=\"rId" +
               std::to_string(6 + i) + "\"/>";
    }
    xml += "</p:sldIdLst>";
    xml += "<p:sldSz cx=\"" + std::to_string(kSlideWidthEmu) + "\" cy=\"" +
           std::to_string(kSlideHeightEmu) + "\" type=\"screen16x9\"/>";
    xml += "<p:notesSz cx=\"6858000\" cy=\"9144000\"/>";
    xml += "</p:presentation>";
    return xml;
}

std::string presentation_rels(size_t slide_count) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" "
        "Target=\"slideMasters/slideMaster1.xml\"/>"
        "<Relationship Id=\"rId2\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/presProps\" "
        "Target=\"presProps.xml\"/>"
        "<Relationship Id=\"rId3\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/viewProps\" "
        "Target=\"viewProps.xml\"/>"
        "<Relationship Id=\"rId4\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" "
        "Target=\"theme/theme1.xml\"/>"
        "<Relationship Id=\"rId5\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/tableStyles\" "
        "Target=\"tableStyles.xml\"/>";
    for (size_t i = 0; i < slide_count; ++i) {
        xml += "<Relationship Id=\"rId" + std::to_string(6 + i) +
               "\" "
               "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" "
               "Target=\"slides/slide" + std::to_string(i + 1) + ".xml\"/>";
    }
    xml += "</Relationships>";
    return xml;
}

std::string slide_xml(size_t index, const Image& image) {
    double scale = std::min(static_cast<double>(kSlideWidthEmu) / image.width,
                            static_cast<double>(kSlideHeightEmu) / image.height);
    int64_t width = static_cast<int64_t>(std::llround(image.width * scale));
    int64_t height = static_cast<int64_t>(std::llround(image.height * scale));
    int64_t x = (kSlideWidthEmu - width) / 2;
    int64_t y = (kSlideHeightEmu - height) / 2;

    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/>"
        "</p:nvGrpSpPr><p:grpSpPr/><p:pic><p:nvPicPr><p:cNvPr id=\"2\" name=\"Picture " +
        std::to_string(index + 1) +
        "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
        "<p:blipFill><a:blip r:embed=\"rId2\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
        "<p:spPr><a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) +
        "\"/><a:ext cx=\"" + std::to_string(width) + "\" cy=\"" + std::to_string(height) +
        "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>"
        "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
    return xml;
}

std::string slide_rels(size_t index) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Id=\"rId1\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" "
           "Target=\"../slideLayouts/slideLayout1.xml\"/>"
           "<Relationship Id=\"rId2\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
           "Target=\"../media/image" +
           std::to_string(index + 1) + ".png\"/></Relationships>";
}

}  // namespace

void write_image_pptx(const std::vector<Image>& pages, const fs::path& path) {
    if (pages.empty()) {
        throw Error("没有可写入 PPT 的页面");
    }
    ZipWriter zip;
    zip.add("[Content_Types].xml", content_types(pages.size()));
    zip.add("_rels/.rels", root_rels());
    zip.add("ppt/presentation.xml", presentation_xml(pages.size()));
    zip.add("ppt/_rels/presentation.xml.rels", presentation_rels(pages.size()));
    zip.add("ppt/presProps.xml", pptx_templates::kPresProps);
    zip.add("ppt/viewProps.xml", pptx_templates::kViewProps);
    zip.add("ppt/tableStyles.xml", pptx_templates::kTableStyles);
    zip.add("ppt/theme/theme1.xml", pptx_templates::kTheme);
    zip.add("ppt/slideMasters/slideMaster1.xml", pptx_templates::kSlideMaster);
    zip.add("ppt/slideMasters/_rels/slideMaster1.xml.rels", pptx_templates::kSlideMasterRels);
    zip.add("ppt/slideLayouts/slideLayout1.xml", pptx_templates::kSlideLayout);
    zip.add("ppt/slideLayouts/_rels/slideLayout1.xml.rels", pptx_templates::kSlideLayoutRels);

    for (size_t i = 0; i < pages.size(); ++i) {
        std::vector<uint8_t> png = encode_png(pages[i]);
        if (png.empty()) {
            throw Error("PPT 页面图片编码失败");
        }
        zip.add("ppt/media/image" + std::to_string(i + 1) + ".png", png);
        zip.add("ppt/slides/slide" + std::to_string(i + 1) + ".xml", slide_xml(i, pages[i]));
        zip.add("ppt/slides/_rels/slide" + std::to_string(i + 1) + ".xml.rels", slide_rels(i));
    }

    fs::path part = append_suffix_utf8(path, ".part");
    try {
        zip.save(part);
        replace_file(part, path);
    } catch (...) {
        remove_file_quiet(part);
        throw;
    }
}

}  // namespace fc