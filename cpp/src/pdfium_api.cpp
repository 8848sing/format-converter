#include "pdfium_api.h"

#if FC_HAVE_PDFIUM

#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fc::pdfium {

namespace {

#ifdef _WIN32
HMODULE g_module = nullptr;
#endif

template <typename T>
bool bind_symbol(T& target, const char* name) {
#ifdef _WIN32
    target = reinterpret_cast<T>(GetProcAddress(g_module, name));
#else
    (void)name;
    target = nullptr;
#endif
    return target != nullptr;
}

#ifdef _WIN32
bool bind_all(Api& api) {
    bool ok = true;
    auto need = [&](auto& target, const char* name) { ok = bind_symbol(target, name) && ok; };

    need(api.InitLibrary, "FPDF_InitLibrary");
    need(api.DestroyLibrary, "FPDF_DestroyLibrary");
    need(api.GetLastError, "FPDF_GetLastError");
    need(api.LoadDocument, "FPDF_LoadDocument");
    need(api.CloseDocument, "FPDF_CloseDocument");
    need(api.GetPageCount, "FPDF_GetPageCount");
    need(api.LoadPage, "FPDF_LoadPage");
    need(api.ClosePage, "FPDF_ClosePage");
    need(api.GetPageWidthF, "FPDF_GetPageWidthF");
    need(api.GetPageHeightF, "FPDF_GetPageHeightF");
    need(api.PageGetRotation, "FPDFPage_GetRotation");
    need(api.PageSetRotation, "FPDFPage_SetRotation");
    need(api.RenderPageBitmap, "FPDF_RenderPageBitmap");
    need(api.BitmapCreateEx, "FPDFBitmap_CreateEx");
    need(api.BitmapDestroy, "FPDFBitmap_Destroy");
    need(api.BitmapFillRect, "FPDFBitmap_FillRect");
    need(api.BitmapGetBuffer, "FPDFBitmap_GetBuffer");
    need(api.BitmapGetStride, "FPDFBitmap_GetStride");
    need(api.TextLoadPage, "FPDFText_LoadPage");
    need(api.TextClosePage, "FPDFText_ClosePage");
    need(api.TextCountChars, "FPDFText_CountChars");
    need(api.TextGetText, "FPDFText_GetText");
    need(api.TextGetUnicode, "FPDFText_GetUnicode");
    need(api.TextGetCharBox, "FPDFText_GetCharBox");
    need(api.TextGetFontSize, "FPDFText_GetFontSize");
    need(api.CreateNewDocument, "FPDF_CreateNewDocument");
    need(api.ImportPages, "FPDF_ImportPages");
    need(api.SaveAsCopy, "FPDF_SaveAsCopy");
    return ok;
}
#else
void bind_all(Api& api) {
    api.InitLibrary = &::FPDF_InitLibrary;
    api.DestroyLibrary = &::FPDF_DestroyLibrary;
    api.GetLastError = &::FPDF_GetLastError;
    api.LoadDocument = &::FPDF_LoadDocument;
    api.CloseDocument = &::FPDF_CloseDocument;
    api.GetPageCount = &::FPDF_GetPageCount;
    api.LoadPage = &::FPDF_LoadPage;
    api.ClosePage = &::FPDF_ClosePage;
    api.GetPageWidthF = &::FPDF_GetPageWidthF;
    api.GetPageHeightF = &::FPDF_GetPageHeightF;
    api.PageGetRotation = &::FPDFPage_GetRotation;
    api.PageSetRotation = &::FPDFPage_SetRotation;
    api.RenderPageBitmap = &::FPDF_RenderPageBitmap;
    api.BitmapCreateEx = &::FPDFBitmap_CreateEx;
    api.BitmapDestroy = &::FPDFBitmap_Destroy;
    api.BitmapFillRect = &::FPDFBitmap_FillRect;
    api.BitmapGetBuffer = &::FPDFBitmap_GetBuffer;
    api.BitmapGetStride = &::FPDFBitmap_GetStride;
    api.TextLoadPage = &::FPDFText_LoadPage;
    api.TextClosePage = &::FPDFText_ClosePage;
    api.TextCountChars = &::FPDFText_CountChars;
    api.TextGetText = &::FPDFText_GetText;
    api.TextGetUnicode = &::FPDFText_GetUnicode;
    api.TextGetCharBox = &::FPDFText_GetCharBox;
    api.TextGetFontSize = &::FPDFText_GetFontSize;
    api.CreateNewDocument = &::FPDF_CreateNewDocument;
    api.ImportPages = &::FPDF_ImportPages;
    api.SaveAsCopy = &::FPDF_SaveAsCopy;
}
#endif

void load_api(Api& api) {
#ifdef _WIN32
    g_module = LoadLibraryW(L"pdfium.dll");
    if (!g_module) {
        const char* override_path = std::getenv("FC_PDFIUM_DLL");
        if (override_path && *override_path) {
            g_module = LoadLibraryW(to_wide(override_path).c_str());
        }
    }
    if (!g_module) {
        api.error = "未找到 pdfium.dll（应放在 fccli.exe 同目录）";
        return;
    }
    if (!bind_all(api)) {
        api.error = "pdfium.dll 缺少必要的导出函数，版本可能不匹配";
        return;
    }
#else
    bind_all(api);
#endif
    api.InitLibrary();
    api.ready = true;
}

}  // namespace

Api& api() {
    static Api instance;
    static std::once_flag once;
    std::call_once(once, [&]() { load_api(instance); });
    return instance;
}

std::mutex& mutex() {
    static std::mutex instance;
    return instance;
}

std::string last_error_text() {
    Api& a = api();
    if (!a.ready || !a.GetLastError) {
        return a.error;
    }
    switch (a.GetLastError()) {
        case 0: return "成功";
        case 1: return "未知错误";
        case 2: return "文件未找到或无法打开";
        case 3: return "不是有效的 PDF 文件";
        case 4: return "PDF 格式错误";
        case 5: return "PDF 已加密，需要密码";
        case 6: return "安全方案不支持";
        case 7: return "页面不存在";
        default: return "PDFium 错误码 " + std::to_string(a.GetLastError());
    }
}

std::string load_error() {
    return api().error;
}

}  // namespace fc::pdfium

#endif  // FC_HAVE_PDFIUM