#include "docx.h"

#include "fs_utils.h"
#include "zip_writer.h"

#include <sstream>

namespace fc {

namespace {

std::string document_xml(const std::vector<DocxParagraph>& paragraphs, const std::string& title) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>";
    (void)title;
    for (const DocxParagraph& paragraph : paragraphs) {
        xml += "<w:p>";
        if (paragraph.page_break_before) {
            xml += "<w:pPr><w:pageBreakBefore/></w:pPr>";
        }
        xml += "<w:r><w:t xml:space=\"preserve\">" + xml_escape(paragraph.text) +
               "</w:t></w:r></w:p>";
    }
    if (paragraphs.empty()) {
        xml += "<w:p/>";
    }
    xml += "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
           "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\"/>"
           "</w:sectPr></w:body></w:document>";
    return xml;
}

const char* kContentTypes =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "</Types>";

const char* kRootRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
    "Target=\"word/document.xml\"/>"
    "</Relationships>";

const char* kDocumentRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
    "Target=\"styles.xml\"/>"
    "</Relationships>";

const char* kStyles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:eastAsia=\"Microsoft YaHei\"/>"
    "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
    "<w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
    "</w:styles>";

}  // namespace

void write_docx(const std::vector<DocxParagraph>& paragraphs, const fs::path& path,
                const std::string& title) {
    ZipWriter zip;
    zip.add("[Content_Types].xml", kContentTypes);
    zip.add("_rels/.rels", kRootRels);
    zip.add("word/document.xml", document_xml(paragraphs, title));
    zip.add("word/_rels/document.xml.rels", kDocumentRels);
    zip.add("word/styles.xml", kStyles);
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