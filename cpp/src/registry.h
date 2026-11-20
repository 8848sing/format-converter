#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace fc {

extern const char* const CAT_IMG;
extern const char* const CAT_PDF;
extern const char* const CAT_WORD;
extern const char* const CAT_EXCEL;
extern const char* const CAT_PPT;
extern const char* const CAT_TEXT;
extern const char* const CAT_MEDIA;
extern const char* const CAT_DATA;

extern const std::vector<std::string> IMAGE_EXTS;
extern const std::vector<std::string> PDF_EXTS;
extern const std::vector<std::string> WORD_EXTS;
extern const std::vector<std::string> EXCEL_EXTS;
extern const std::vector<std::string> PPT_EXTS;
extern const std::vector<std::string> TEXT_EXTS;
extern const std::vector<std::string> MEDIA_EXTS;
extern const std::vector<std::string> DATA_EXTS;
extern const std::vector<std::string> TABLE_DATA_EXTS;

const std::vector<ConversionSpec>& conversions();
const ConversionSpec* find_conversion(const std::string& id);
std::vector<std::string> categories_src();
std::vector<std::string> dsts_for(const std::string& src);
std::vector<const ConversionSpec*> convs_for(const std::string& src, const std::string& dst);
bool pair_exists(const std::string& src, const std::string& dst);
std::string registry_json();

}  // namespace fc