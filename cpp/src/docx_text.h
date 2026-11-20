#pragma once

/*
 * 原生 Office 只读能力（不依赖 LibreOffice）。
 *
 * 手机上没有 soffice，但 .docx 本质是 zip + XML，用 miniz 解压 + tinyxml2 解析
 * 就能拿到文字；xlsx 的读取在 data.cpp 里已经实现。
 */

#include "common.h"

namespace fc {

/* 提取 .docx 的正文文本（段落换行、单元格用制表符分隔）。 */
bool extract_docx_text(const fs::path& path, std::string& text, std::string& error);

/* 任务入口：把一批 .docx 转成同名 .txt；老式 .doc 会给出明确提示。 */
void docx_to_text_job(const Job& job, EventSink& sink);

}  // namespace fc