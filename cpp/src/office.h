#pragma once

#include "common.h"

#include <string>

namespace fc {

/* Office/文本 → 指定格式；filter 为 LibreOffice --convert-to 的过滤串。 */
void office_export_job(const Job& job, EventSink& sink, const std::string& filter,
                       const std::string& out_ext);

/* Office → 图片：先转 PDF，再逐页渲染。 */
void office_to_images(const Job& job, EventSink& sink);

fs::path find_soffice(const Job& job);

/* 是否找得到 LibreOffice；找不到时（例如手机端）走内置的只读实现。 */
bool has_soffice(const Job& job);

}  // namespace fc