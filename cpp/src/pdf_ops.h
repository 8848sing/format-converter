#pragma once

#include "common.h"

namespace fc {

void pdf_merge(const Job& job, EventSink& sink);
void pdf_split(const Job& job, EventSink& sink);
void pdf_rotate(const Job& job, EventSink& sink);
void pdf_compress(const Job& job, EventSink& sink);
void pdf_to_images(const Job& job, EventSink& sink);
void pdf_to_text(const Job& job, EventSink& sink);
void pdf_to_word(const Job& job, EventSink& sink);
void pdf_to_excel(const Job& job, EventSink& sink);
void pdf_to_ppt(const Job& job, EventSink& sink);

}  // namespace fc