#include "xlsx.h"

#include "zip_writer.h"

#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

namespace fc {

namespace {

std::string column_name(int index) {
    std::string name;
    int value = index + 1;
    while (value > 0) {
        int remainder = (value - 1) % 26;
        name.push_back(static_cast<char>('A' + remainder));
        value = (value - 1) / 26;
    }
    std::reverse(name.begin(), name.end());
    return name;
}

std::string sanitize_sheet_name(const std::string& name, size_t index) {
    std::string result = name.empty() ? "Sheet" + std::to_string(index + 1) : name;
    for (char& ch : result) {
        if (ch == '[' || ch == ']' || ch == ':' || ch == '*' || ch == '?' || ch == '/' || ch == '\\') {
            ch = '_';
        }
    }
    if (result.size() > 31) {
        result.resize(31);
    }
    return result;
}

std::string cell_xml(const std::string& reference, const std::string& value) {
    if (value.empty()) {
        return "<c r=\"" + reference + "\"/>";
    }
    return "<c r=\"" + reference + "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">" +
           xml_escape(value) + "</t></is></c>";
}

std::string workbook_xml(const std::vector<XlsxSheet>& sheets) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets>";
    for (size_t i = 0; i < sheets.size(); ++i) {
        xml += "<sheet name=\"" + xml_escape(sanitize_sheet_name(sheets[i].name, i)) +
               "\" sheetId=\"" + std::to_string(i + 1) + "\" r:id=\"rId" +
               std::to_string(i + 1) + "\"/>";
    }
    xml += "</sheets></workbook>";
    return xml;
}

std::string workbook_rels(const std::vector<XlsxSheet>& sheets) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (size_t i = 0; i < sheets.size(); ++i) {
        xml += "<Relationship Id=\"rId" + std::to_string(i + 1) +
               "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
               "Target=\"worksheets/sheet" + std::to_string(i + 1) + ".xml\"/>";
    }
    xml += "<Relationship Id=\"rId" + std::to_string(sheets.size() + 1) +
           "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
           "Target=\"styles.xml\"/>";
    xml += "</Relationships>";
    return xml;
}

std::string sheet_xml(const XlsxSheet& sheet) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetData>";
    for (size_t r = 0; r < sheet.rows.size(); ++r) {
        xml += "<row r=\"" + std::to_string(r + 1) + "\">";
        for (size_t c = 0; c < sheet.rows[r].size(); ++c) {
            const std::string& value = sheet.rows[r][c];
            if (value.empty()) {
                continue;
            }
            std::string reference = column_name(static_cast<int>(c)) + std::to_string(r + 1);
            xml += cell_xml(reference, value);
        }
        xml += "</row>";
    }
    xml += "</sheetData></worksheet>";
    return xml;
}

std::string content_types(size_t sheet_count) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>";
    for (size_t i = 0; i < sheet_count; ++i) {
        xml += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1) +
               ".xml\" "
               "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    }
    xml += "</Types>";
    return xml;
}

const char* kStylesXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
    "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
    "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
    "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
    "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
    "</styleSheet>";

std::string rels_root() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Id=\"rId1\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
           "Target=\"xl/workbook.xml\"/>"
           "</Relationships>";
}

std::string shared_string_text(const tinyxml2::XMLElement* si) {
    std::string result;
    for (const tinyxml2::XMLElement* t = si->FirstChildElement("t"); t;
         t = t->NextSiblingElement("t")) {
        if (t->GetText()) {
            result += t->GetText();
        }
    }
    return result;
}

std::string relationship_target(const std::string& workbook_xml_text,
                                const std::string& rels_text) {
    tinyxml2::XMLDocument workbook;
    if (workbook.Parse(workbook_xml_text.c_str()) != tinyxml2::XML_SUCCESS) {
        return {};
    }
    const tinyxml2::XMLElement* sheet = workbook.FirstChildElement("workbook");
    if (!sheet) {
        return {};
    }
    const tinyxml2::XMLElement* sheets = sheet->FirstChildElement("sheets");
    if (!sheets) {
        return {};
    }
    const tinyxml2::XMLElement* first = sheets->FirstChildElement("sheet");
    if (!first) {
        return {};
    }
    const char* rid = first->Attribute("r:id");
    if (!rid) {
        rid = first->Attribute("id");
    }
    if (!rid) {
        return {};
    }

    tinyxml2::XMLDocument rels;
    if (rels.Parse(rels_text.c_str()) != tinyxml2::XML_SUCCESS) {
        return {};
    }
    const tinyxml2::XMLElement* root = rels.FirstChildElement("Relationships");
    if (!root) {
        return {};
    }
    for (const tinyxml2::XMLElement* rel = root->FirstChildElement("Relationship"); rel;
         rel = rel->NextSiblingElement("Relationship")) {
        const char* id = rel->Attribute("Id");
        if (id && std::string(id) == rid) {
            const char* target = rel->Attribute("Target");
            return target ? std::string(target) : std::string();
        }
    }
    return {};
}

std::string normalize_zip_path(const std::string& target) {
    if (target.rfind("/", 0) == 0) {
        return target.substr(1);
    }
    if (target.rfind("xl/", 0) == 0) {
        return target;
    }
    return "xl/" + target;
}

void parse_sheet(const std::string& xml, const std::vector<std::string>& shared,
                 std::vector<std::vector<std::string>>& rows) {
    tinyxml2::XMLDocument document;
    if (document.Parse(xml.c_str()) != tinyxml2::XML_SUCCESS) {
        throw Error("XLSX 工作表 XML 解析失败");
    }
    const tinyxml2::XMLElement* worksheet = document.FirstChildElement("worksheet");
    if (!worksheet) {
        return;
    }
    const tinyxml2::XMLElement* sheet_data = worksheet->FirstChildElement("sheetData");
    if (!sheet_data) {
        return;
    }
    for (const tinyxml2::XMLElement* row = sheet_data->FirstChildElement("row"); row;
         row = row->NextSiblingElement("row")) {
        int row_index = 0;
        if (row->Attribute("r")) {
            row_index = std::max(0, std::atoi(row->Attribute("r")) - 1);
        } else {
            row_index = static_cast<int>(rows.size());
        }
        if (static_cast<size_t>(row_index) >= rows.size()) {
            rows.resize(static_cast<size_t>(row_index) + 1);
        }
        auto& output_row = rows[static_cast<size_t>(row_index)];
        for (const tinyxml2::XMLElement* cell = row->FirstChildElement("c"); cell;
             cell = cell->NextSiblingElement("c")) {
            const char* reference = cell->Attribute("r");
            int column = 0;
            if (reference) {
                for (const char* p = reference; *p && std::isalpha(static_cast<unsigned char>(*p)); ++p) {
                    column = column * 26 + (std::toupper(static_cast<unsigned char>(*p)) - 'A' + 1);
                }
                column = std::max(0, column - 1);
            } else {
                column = static_cast<int>(output_row.size());
            }
            if (static_cast<size_t>(column) >= output_row.size()) {
                output_row.resize(static_cast<size_t>(column) + 1);
            }
            std::string value;
            const char* type = cell->Attribute("t");
            if (type && std::string(type) == "inlineStr") {
                const tinyxml2::XMLElement* is = cell->FirstChildElement("is");
                if (is) {
                    value = shared_string_text(is);
                }
            } else {
                const tinyxml2::XMLElement* v = cell->FirstChildElement("v");
                if (v && v->GetText()) {
                    if (type && std::string(type) == "s") {
                        int index = std::atoi(v->GetText());
                        if (index >= 0 && static_cast<size_t>(index) < shared.size()) {
                            value = shared[static_cast<size_t>(index)];
                        }
                    } else if (type && std::string(type) == "b") {
                        value = std::string(v->GetText()) == "1" ? "TRUE" : "FALSE";
                    } else {
                        value = v->GetText();
                    }
                }
            }
            output_row[static_cast<size_t>(column)] = value;
        }
    }
}

}  // namespace

void write_xlsx(const std::vector<XlsxSheet>& sheets, const fs::path& path) {
    if (sheets.empty()) {
        throw Error("没有可写入的表格数据");
    }
    ZipWriter zip;
    zip.add("[Content_Types].xml", content_types(sheets.size()));
    zip.add("_rels/.rels", rels_root());
    zip.add("xl/workbook.xml", workbook_xml(sheets));
    zip.add("xl/_rels/workbook.xml.rels", workbook_rels(sheets));
    zip.add("xl/styles.xml", kStylesXml);
    for (size_t i = 0; i < sheets.size(); ++i) {
        zip.add("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", sheet_xml(sheets[i]));
    }
    zip.save(path);
}

std::vector<std::vector<std::string>> read_xlsx_first_sheet(const fs::path& path) {
    ZipReader zip(path);
    std::string workbook = zip.read_text("xl/workbook.xml");
    std::string rels = zip.read_text("xl/_rels/workbook.xml.rels");
    std::string target = relationship_target(workbook, rels);
    if (target.empty()) {
        target = "worksheets/sheet1.xml";
    }
    std::string sheet_path = normalize_zip_path(target);

    std::vector<std::string> shared;
    if (zip.contains("xl/sharedStrings.xml")) {
        tinyxml2::XMLDocument document;
        std::string xml = zip.read_text("xl/sharedStrings.xml");
        if (document.Parse(xml.c_str()) == tinyxml2::XML_SUCCESS) {
            const tinyxml2::XMLElement* root = document.FirstChildElement("sst");
            if (root) {
                for (const tinyxml2::XMLElement* si = root->FirstChildElement("si"); si;
                     si = si->NextSiblingElement("si")) {
                    shared.push_back(shared_string_text(si));
                }
            }
        }
    }

    std::vector<std::vector<std::string>> rows;
    parse_sheet(zip.read_text(sheet_path), shared, rows);
    while (!rows.empty() && rows.back().empty()) {
        rows.pop_back();
    }
    return rows;
}

}  // namespace fc