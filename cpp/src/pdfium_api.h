#pragma once

#include "common.h"

#include <mutex>

#if FC_HAVE_PDFIUM

#include "fpdfview.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include "fpdf_text.h"

namespace fc::pdfium {

/*
 * Windows 使用运行时 LoadLibrary 加载 pdfium.dll，避免 MinGW 链接 MSVC 导入库；
 * Android/Linux 直接链接 libpdfium。
 */
struct Api {
    decltype(&FPDF_InitLibrary) InitLibrary = nullptr;
    decltype(&FPDF_DestroyLibrary) DestroyLibrary = nullptr;
    decltype(&FPDF_GetLastError) GetLastError = nullptr;

    decltype(&FPDF_LoadDocument) LoadDocument = nullptr;
    decltype(&FPDF_CloseDocument) CloseDocument = nullptr;
    decltype(&FPDF_GetPageCount) GetPageCount = nullptr;
    decltype(&FPDF_LoadPage) LoadPage = nullptr;
    decltype(&FPDF_ClosePage) ClosePage = nullptr;
    decltype(&FPDF_GetPageWidthF) GetPageWidthF = nullptr;
    decltype(&FPDF_GetPageHeightF) GetPageHeightF = nullptr;
    decltype(&FPDFPage_GetRotation) PageGetRotation = nullptr;
    decltype(&FPDFPage_SetRotation) PageSetRotation = nullptr;

    decltype(&FPDF_RenderPageBitmap) RenderPageBitmap = nullptr;
    decltype(&FPDFBitmap_CreateEx) BitmapCreateEx = nullptr;
    decltype(&FPDFBitmap_Destroy) BitmapDestroy = nullptr;
    decltype(&FPDFBitmap_FillRect) BitmapFillRect = nullptr;
    decltype(&FPDFBitmap_GetBuffer) BitmapGetBuffer = nullptr;
    decltype(&FPDFBitmap_GetStride) BitmapGetStride = nullptr;

    decltype(&FPDFText_LoadPage) TextLoadPage = nullptr;
    decltype(&FPDFText_ClosePage) TextClosePage = nullptr;
    decltype(&FPDFText_CountChars) TextCountChars = nullptr;
    decltype(&FPDFText_GetText) TextGetText = nullptr;
    decltype(&FPDFText_GetUnicode) TextGetUnicode = nullptr;
    decltype(&FPDFText_GetCharBox) TextGetCharBox = nullptr;
    decltype(&FPDFText_GetFontSize) TextGetFontSize = nullptr;

    decltype(&FPDF_CreateNewDocument) CreateNewDocument = nullptr;
    decltype(&FPDF_ImportPages) ImportPages = nullptr;
    decltype(&FPDF_SaveAsCopy) SaveAsCopy = nullptr;

    bool ready = false;
    std::string error;
};

Api& api();
std::mutex& mutex();

std::string last_error_text();
std::string load_error();

}  // namespace fc::pdfium

#endif  // FC_HAVE_PDFIUM