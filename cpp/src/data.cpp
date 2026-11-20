#include "data.h"

#include "fs_utils.h"
#include "xlsx.h"
#include "zip_writer.h"

#include <tinyxml2.h>
#if FC_HAVE_YAML
#include <yaml-cpp/yaml.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace fc {

namespace {

using ojson = nlohmann::ordered_json;

std::string json_dump(const ojson& value) {
    return value.dump(2, ' ', false, ojson::error_handler_t::replace);
}

std::string xml_name(const std::string& value) {
    if (value.empty()) {
        return "item";
    }
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
                     (i > 0 && ((ch >= '0' && ch <= '9') || ch == '-' || ch == '.'));
        out.push_back(valid ? static_cast<char>(ch) : '_');
    }
    return out;
}

ojson xml_value(const tinyxml2::XMLElement* element) {
    ojson result = ojson::object();
    for (const tinyxml2::XMLAttribute* attr = element->FirstAttribute(); attr;
         attr = attr->Next()) {
        result["@" + std::string(attr->Name())] = attr->Value() ? attr->Value() : "";
    }
    const char* text = element->GetText();
    std::string trimmed = text ? trim(text) : "";
    bool has_children = element->FirstChildElement() != nullptr;
    if (!has_children) {
        if (result.empty()) {
            return trimmed;
        }
        if (!trimmed.empty()) {
            result["#text"] = trimmed;
        }
        return result;
    }
    for (const tinyxml2::XMLElement* child = element->FirstChildElement(); child;
         child = child->NextSiblingElement()) {
        std::string key = child->Name() ? child->Name() : "item";
        ojson value = xml_value(child);
        if (!result.contains(key)) {
            result[key] = value;
        } else if (result[key].is_array()) {
            result[key].push_back(value);
        } else {
            ojson array = ojson::array();
            array.push_back(result[key]);
            array.push_back(value);
            result[key] = array;
        }
    }
    if (!trimmed.empty()) {
        result["#text"] = trimmed;
    }
    return result;
}

ojson load_structured(const fs::path& path) {
    std::string ext = extension_lower(path);
    std::string text = read_text_auto(path);
    try {
        if (ext == ".json") {
            return ojson::parse(text);
        }
#if FC_HAVE_YAML
        if (ext == ".yaml" || ext == ".yml") {
            YAML::Node node = YAML::Load(text);
            std::function<ojson(const YAML::Node&)> convert = [&](const YAML::Node& value) -> ojson {
                if (!value || value.IsNull()) {
                    return nullptr;
                }
                if (value.IsSequence()) {
                    ojson array = ojson::array();
                    for (const YAML::Node& item : value) {
                        array.push_back(convert(item));
                    }
                    return array;
                }
                if (value.IsMap()) {
                    ojson object = ojson::object();
                    for (auto it = value.begin(); it != value.end(); ++it) {
                        std::string key = it->first.as<std::string>();
                        object[key] = convert(it->second);
                    }
                    return object;
                }
                std::string scalar = value.Scalar();
                std::string tag = value.Tag();
                if (tag == "tag:yaml.org,2002:str") {
                    return scalar;
                }
                std::string lowered = lower_ascii(scalar);
                if (lowered == "null" || lowered == "~") {
                    return nullptr;
                }
                if (lowered == "true" || lowered == "yes" || lowered == "on") {
                    return true;
                }
                if (lowered == "false" || lowered == "no" || lowered == "off") {
                    return false;
                }
                char* end = nullptr;
                errno = 0;
                long long integer = std::strtoll(scalar.c_str(), &end, 0);
                if (end && *end == '\0' && errno == 0 && !scalar.empty()) {
                    return integer;
                }
                errno = 0;
                double number = std::strtod(scalar.c_str(), &end);
                if (end && *end == '\0' && errno == 0 && !scalar.empty()) {
                    return number;
                }
                return scalar;
            };
            return convert(node);
        }
#else
        if (ext == ".yaml" || ext == ".yml") {
            throw Error("当前构建未启用 YAML 支持");
        }
#endif
        if (ext == ".xml") {
            tinyxml2::XMLDocument document;
            if (document.Parse(text.c_str()) != tinyxml2::XML_SUCCESS) {
                throw Error("XML 解析失败");
            }
            const tinyxml2::XMLElement* root = document.RootElement();
            if (!root) {
                throw Error("XML 缺少根元素");
            }
            ojson result = ojson::object();
            result[root->Name() ? root->Name() : "root"] = xml_value(root);
            return result;
        }
    } catch (const Error&) {
        throw;
    } catch (const std::exception& e) {
        throw Error(std::string("解析 ") + path_to_utf8(path) + " 失败: " + e.what());
    }
    throw Error("不支持的结构化数据格式：" + ext);
}

void append_xml(std::string& out, const std::string& key, const ojson& value, int indent) {
    std::string pad(static_cast<size_t>(indent), ' ');
    if (value.is_array()) {
        for (const auto& item : value) {
            append_xml(out, key, item, indent);
        }
        return;
    }
    std::string name = xml_name(key);
    if (value.is_null()) {
        out += pad + "<" + name + "/>\n";
        return;
    }
    if (!value.is_object()) {
        out += pad + "<" + name + ">" + xml_escape(value.is_string() ? value.get<std::string>()
                                                                   : value.dump()) +
               "</" + name + ">\n";
        return;
    }

    std::string attributes;
    std::string text;
    std::vector<std::pair<std::string, ojson>> children;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& child_key = it.key();
        if (child_key.rfind("@", 0) == 0) {
            attributes += " " + xml_name(child_key.substr(1)) + "=\"" +
                          xml_escape(it.value().is_string() ? it.value().get<std::string>()
                                                            : it.value().dump()) +
                          "\"";
        } else if (child_key == "#text") {
            text = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        } else {
            children.emplace_back(child_key, it.value());
        }
    }

    if (children.empty()) {
        if (text.empty() && attributes.empty()) {
            out += pad + "<" + name + "/>\n";
        } else if (text.empty()) {
            out += pad + "<" + name + attributes + "/>\n";
        } else {
            out += pad + "<" + name + attributes + ">" + xml_escape(text) + "</" + name + ">\n";
        }
        return;
    }
    out += pad + "<" + name + attributes + ">\n";
    if (!text.empty()) {
        out += std::string(static_cast<size_t>(indent + 2), ' ') + xml_escape(text) + "\n";
    }
    for (const auto& child : children) {
        append_xml(out, child.first, child.second, indent + 2);
    }
    out += pad + "</" + name + ">\n";
}

std::string to_xml(const ojson& data) {
    std::string root_name = "root";
    ojson root_value = data;
    if (data.is_object() && data.size() == 1) {
        auto it = data.begin();
        root_name = it.key();
        root_value = it.value();
    }
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    if (root_value.is_object()) {
        out += "<" + xml_name(root_name) + ">\n";
        for (auto it = root_value.begin(); it != root_value.end(); ++it) {
            append_xml(out, it.key(), it.value(), 2);
        }
        out += "</" + xml_name(root_name) + ">\n";
    } else if (root_value.is_array()) {
        out += "<" + xml_name(root_name) + ">\n";
        for (const auto& item : root_value) {
            append_xml(out, "item", item, 2);
        }
        out += "</" + xml_name(root_name) + ">\n";
    } else if (root_value.is_null()) {
        out += "<" + xml_name(root_name) + "/>\n";
    } else {
        out += "<" + xml_name(root_name) + ">" +
               xml_escape(root_value.is_string() ? root_value.get<std::string>()
                                                 : root_value.dump()) +
               "</" + xml_name(root_name) + ">\n";
    }
    return out;
}

#if FC_HAVE_YAML
YAML::Node to_yaml(const ojson& value) {
    if (value.is_null()) {
        return YAML::Node(YAML::NodeType::Null);
    }
    if (value.is_boolean()) {
        return YAML::Node(value.get<bool>());
    }
    if (value.is_number_integer()) {
        return YAML::Node(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return YAML::Node(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        return YAML::Node(value.get<double>());
    }
    if (value.is_string()) {
        return YAML::Node(value.get<std::string>());
    }
    if (value.is_array()) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (const auto& item : value) {
            node.push_back(to_yaml(item));
        }
        return node;
    }
    YAML::Node node(YAML::NodeType::Map);
    for (auto it = value.begin(); it != value.end(); ++it) {
        node[it.key()] = to_yaml(it.value());
    }
    return node;
}
#endif

ojson flatten_record(const ojson& record, const std::string& prefix = "") {
    if (!record.is_object()) {
        ojson row = ojson::object();
        row["value"] = record;
        return row;
    }
    ojson row = ojson::object();
    for (auto it = record.begin(); it != record.end(); ++it) {
        std::string name = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it.value().is_object()) {
            ojson nested = flatten_record(it.value(), name);
            for (auto nested_it = nested.begin(); nested_it != nested.end(); ++nested_it) {
                row[nested_it.key()] = nested_it.value();
            }
        } else if (it.value().is_array()) {
            row[name] = it.value().dump();
        } else {
            row[name] = it.value();
        }
    }
    return row;
}

std::vector<ojson> tabular_rows(const ojson& data) {
    std::vector<ojson> rows;
    if (data.is_array()) {
        for (const auto& item : data) {
            rows.push_back(flatten_record(item));
        }
        return rows;
    }
    if (data.is_object()) {
        std::vector<std::string> list_keys;
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (it.value().is_array()) {
                list_keys.push_back(it.key());
            }
        }
        if (data.size() == 1 && !list_keys.empty()) {
            for (const auto& item : data[list_keys.front()]) {
                rows.push_back(flatten_record(item));
            }
            return rows;
        }
        bool all_arrays = !data.empty();
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (!it.value().is_array()) {
                all_arrays = false;
                break;
            }
        }
        if (all_arrays) {
            size_t length = 0;
            for (auto it = data.begin(); it != data.end(); ++it) {
                length = std::max(length, it.value().size());
            }
            for (size_t i = 0; i < length; ++i) {
                ojson row = ojson::object();
                for (auto it = data.begin(); it != data.end(); ++it) {
                    row[it.key()] = i < it.value().size() ? it.value()[i] : ojson(nullptr);
                }
                rows.push_back(std::move(row));
            }
            return rows;
        }
        rows.push_back(flatten_record(data));
        return rows;
    }
    ojson row = ojson::object();
    row["value"] = data;
    rows.push_back(std::move(row));
    return rows;
}

std::string cell_text(const ojson& value) {
    if (value.is_null()) {
        return {};
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return value.dump();
}

std::vector<std::string> headers_for(const std::vector<ojson>& rows) {
    std::vector<std::string> headers;
    for (const ojson& row : rows) {
        for (auto it = row.begin(); it != row.end(); ++it) {
            if (std::find(headers.begin(), headers.end(), it.key()) == headers.end()) {
                headers.push_back(it.key());
            }
        }
    }
    if (headers.empty()) {
        headers.push_back("value");
    }
    return headers;
}

bool write_data_xlsx(const ojson& data, const fs::path& path) {
    std::vector<ojson> rows = tabular_rows(data);
    std::vector<std::string> headers = headers_for(rows);
    XlsxSheet sheet;
    sheet.name = "数据";
    std::vector<std::string> header_row;
    header_row.reserve(headers.size());
    for (const std::string& header : headers) {
        header_row.push_back(header);
    }
    sheet.rows.push_back(std::move(header_row));
    for (const ojson& row : rows) {
        std::vector<std::string> values;
        values.reserve(headers.size());
        for (const std::string& header : headers) {
            values.push_back(row.contains(header) ? cell_text(row[header]) : std::string());
        }
        sheet.rows.push_back(std::move(values));
    }
    write_xlsx({sheet}, path);
    return true;
}

std::vector<std::vector<std::string>> parse_csv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool in_quotes = false;
    size_t i = text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' && text[2] == '\xBF' ? 3 : 0;
    for (; i < text.size(); ++i) {
        char ch = text[i];
        if (in_quotes) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                field.push_back(ch);
            }
        } else if (ch == '"') {
            in_quotes = true;
        } else if (ch == ',') {
            row.push_back(field);
            field.clear();
        } else if (ch == '\n') {
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
        } else if (ch == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
        } else {
            field.push_back(ch);
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

}  // namespace

void convert_structured(const Job& job, EventSink& sink) {
    std::string target = lower_ascii(job.target_or("json"));
    std::string suffix;
    if (target == "json") {
        suffix = ".json";
    } else if (target == "yaml" || target == "yml") {
        suffix = target == "yml" ? ".yml" : ".yaml";
    } else if (target == "xml") {
        suffix = ".xml";
    } else if (target == "xlsx") {
        suffix = ".xlsx";
    } else {
        throw Error("不支持的数据目标格式：" + target);
    }
#if !FC_HAVE_YAML
    if (target == "yaml" || target == "yml") {
        throw Error("当前构建未启用 YAML 支持");
    }
#endif

    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            ojson data = load_structured(input);
            fs::path out = unique_output(job.out_dir, stem_utf8(input), suffix);
            if (target == "json") {
                write_text_file(out, json_dump(data) + "\n");
            } else if (target == "yaml" || target == "yml") {
#if FC_HAVE_YAML
                std::string text = YAML::Dump(to_yaml(data));
                write_text_file(out, text + "\n");
#else
                throw Error("当前构建未启用 YAML 支持");
#endif
            } else if (target == "xml") {
                write_text_file(out, to_xml(data));
            } else {
                write_data_xlsx(data, out);
            }
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

void convert_table_to_json(const Job& job, EventSink& sink) {
    for (const fs::path& input : job.inputs) {
        std::string input_text = path_to_utf8(input);
        try {
            std::vector<std::vector<std::string>> rows;
            if (extension_lower(input) == ".csv") {
                rows = parse_csv(read_text_auto(input));
            } else {
                rows = read_xlsx_first_sheet(input);
            }
            ojson result = ojson::array();
            if (rows.empty()) {
                // 空表输出空数组。
            } else {
                std::vector<std::string> headers = rows.front();
                if (headers.empty()) {
                    headers.push_back("value");
                }
                for (size_t i = 1; i < rows.size(); ++i) {
                    ojson item = ojson::object();
                    for (size_t c = 0; c < headers.size(); ++c) {
                        std::string header = headers[c].empty() ? "列" + std::to_string(c + 1) : headers[c];
                        item[header] = c < rows[i].size() ? ojson(rows[i][c]) : ojson(nullptr);
                    }
                    result.push_back(std::move(item));
                }
            }
            fs::path out = unique_output(job.out_dir, stem_utf8(input), ".json");
            write_text_file(out, json_dump(result) + "\n");
            sink.file_ok(input_text, {path_to_utf8(out)});
        } catch (const std::exception& e) {
            sink.file_error(input_text, e.what());
        }
    }
}

}  // namespace fc