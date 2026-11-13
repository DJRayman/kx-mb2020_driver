// SPDX-License-Identifier: GPL-2.0-or-later
#include "mb2020/pdf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <type_traits>

#include "mb2020/gdi_print.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace mb2020 {

namespace {

// Минимальный набор функций PDFium (fpdfview.h), вызываем через указатели.
typedef void* FPDF_DOCUMENT;
typedef void* FPDF_PAGE;
typedef void* FPDF_BITMAP;
const int kFlagAnnot = 0x01, kFlagPrinting = 0x800, kNoSmoothText = 0x1000, kNoSmoothPath = 0x4000;
const int kBGRx = 3;

struct Api {
    void (*InitLibrary)() = nullptr;
    FPDF_DOCUMENT (*LoadMemDocument)(const void*, int, const char*) = nullptr;
    void (*CloseDocument)(FPDF_DOCUMENT) = nullptr;
    int (*GetPageCount)(FPDF_DOCUMENT) = nullptr;
    FPDF_PAGE (*LoadPage)(FPDF_DOCUMENT, int) = nullptr;
    void (*ClosePage)(FPDF_PAGE) = nullptr;
    float (*GetPageWidthF)(FPDF_PAGE) = nullptr;
    float (*GetPageHeightF)(FPDF_PAGE) = nullptr;
    unsigned long (*GetLastError)() = nullptr;
    FPDF_BITMAP (*BitmapCreateEx)(int, int, int, void*, int) = nullptr;
    int (*BitmapFillRect)(FPDF_BITMAP, int, int, int, int, unsigned long) = nullptr;
    void (*BitmapDestroy)(FPDF_BITMAP) = nullptr;
    void (*RenderPageBitmap)(FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int, int) = nullptr;
    bool ready = false;
};
Api g_api;
std::mutex g_mutex;  // PDFium не потокобезопасен

void* open_lib(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return (void*)LoadLibraryW(w.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void* sym(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}
std::string exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    size_t s = w.find_last_of(L"\\/");
    w = s == std::wstring::npos ? L"." : w.substr(0, s);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string r(m > 0 ? m - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &r[0], m, nullptr, nullptr);
    return r;
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    std::string p(buf, size_t(n));
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "." : p.substr(0, s);
#endif
}

const char* pdf_error(unsigned long e) {
    switch (e) {
        case 2: return "файл не найден или не открывается";
        case 3: return "файл повреждён или это не PDF";
        case 4: return "PDF защищён паролем";
        case 5: return "неподдерживаемая защита PDF";
        case 6: return "страница не найдена или повреждена";
        default: return "неизвестная ошибка PDFium";
    }
}

}  // namespace

void PdfDocument::load_library(const std::string& lib_path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_api.ready) return;
#ifdef _WIN32
    const char* name = "pdfium.dll";
    const char sep = '\\';
#else
    const char* name = "libpdfium.so";
    const char sep = '/';
#endif
    std::vector<std::string> tries;
    if (!lib_path.empty()) tries.push_back(lib_path);
    tries.push_back(exe_dir() + sep + name);
    tries.push_back(name);
    void* lib = nullptr;
    for (const auto& t : tries)
        if ((lib = open_lib(t))) break;
    if (!lib)
        throw std::runtime_error(std::string("не найдена библиотека PDFium (") + name +
                                 "): положите её рядом с mbtool");
    Api a;
    bool ok = true;
    auto get = [&](auto& fn, const char* s) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(sym(lib, s));
        if (!fn) ok = false;
    };
    get(a.InitLibrary, "FPDF_InitLibrary");
    get(a.LoadMemDocument, "FPDF_LoadMemDocument");
    get(a.CloseDocument, "FPDF_CloseDocument");
    get(a.GetPageCount, "FPDF_GetPageCount");
    get(a.LoadPage, "FPDF_LoadPage");
    get(a.ClosePage, "FPDF_ClosePage");
    get(a.GetPageWidthF, "FPDF_GetPageWidthF");
    get(a.GetPageHeightF, "FPDF_GetPageHeightF");
    get(a.GetLastError, "FPDF_GetLastError");
    get(a.BitmapCreateEx, "FPDFBitmap_CreateEx");
    get(a.BitmapFillRect, "FPDFBitmap_FillRect");
    get(a.BitmapDestroy, "FPDFBitmap_Destroy");
    get(a.RenderPageBitmap, "FPDF_RenderPageBitmap");
    if (!ok) throw std::runtime_error("библиотека PDFium слишком старая или не та (нет нужных функций)");
    a.InitLibrary();
    a.ready = true;
    g_api = a;
}

PdfDocument::PdfDocument(std::vector<uint8_t> data, const std::string& password) : data_(std::move(data)) {
    load_library();
    std::lock_guard<std::mutex> lk(g_mutex);
    doc_ = g_api.LoadMemDocument(data_.data(), int(data_.size()), password.empty() ? nullptr : password.c_str());
    if (!doc_) throw std::runtime_error(std::string("PDF не открылся: ") + pdf_error(g_api.GetLastError()));
}

PdfDocument::~PdfDocument() {
    if (doc_) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_api.CloseDocument(doc_);
    }
}

int PdfDocument::page_count() const {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_api.GetPageCount(doc_);
}

void PdfDocument::page_size_pt(int index, double& w, double& h) const {
    std::lock_guard<std::mutex> lk(g_mutex);
    FPDF_PAGE p = g_api.LoadPage(doc_, index);
    if (!p) throw std::runtime_error("страница " + std::to_string(index + 1) + ": " + pdf_error(g_api.GetLastError()));
    w = g_api.GetPageWidthF(p);
    h = g_api.GetPageHeightF(p);
    g_api.ClosePage(p);
}

Image8 PdfDocument::render_a4_600(int index, const PdfRenderOptions& opt) const {
    std::lock_guard<std::mutex> lk(g_mutex);
    FPDF_PAGE page = g_api.LoadPage(doc_, index);
    if (!page) throw std::runtime_error("страница " + std::to_string(index + 1) + ": " + pdf_error(g_api.GetLastError()));

    const int PW = kA4Width600, PH = kA4Height600;
    const double A4W = 210.0 / 25.4 * 600, A4H = 297.0 / 25.4 * 600;  // 4960.6 x 7015.7 пикс
    double w = g_api.GetPageWidthF(page) / 72.0 * 600, h = g_api.GetPageHeightF(page) / 72.0 * 600;
    int rotate = 0;
    if (opt.auto_rotate && w > h * 1.01) {
        rotate = 1;  // 90° по часовой
        std::swap(w, h);
    }
    bool actual = opt.scale == PdfScale::Actual ||
                  (opt.scale == PdfScale::Auto && w <= A4W * 1.01 && h <= A4H * 1.01);
    int rw, rh, x0, y0;
    if (actual) {
        // Страница по центру листа A4; печатаемая область начинается в ~100 пикс от края листа.
        rw = int(std::lround(w));
        rh = int(std::lround(h));
        x0 = int(std::lround((A4W - w) / 2 - (A4W - PW) / 2));
        y0 = int(std::lround((A4H - h) / 2 - (A4H - PH) / 2));
    } else {
        double k = std::min(PW / w, PH / h);
        rw = int(std::lround(w * k));
        rh = int(std::lround(h * k));
        x0 = (PW - rw) / 2;
        y0 = (PH - rh) / 2;
    }

    Image8 out(PW, PH, 1);
    const int kBand = 1024;
    std::vector<uint8_t> buf(size_t(PW) * 4 * kBand);
    for (int by = 0; by < PH; by += kBand) {
        const int bh = std::min(kBand, PH - by);
        FPDF_BITMAP bmp = g_api.BitmapCreateEx(PW, bh, kBGRx, buf.data(), PW * 4);
        if (!bmp) {
            g_api.ClosePage(page);
            throw std::runtime_error("PDFium: не хватает памяти для растра");
        }
        g_api.BitmapFillRect(bmp, 0, 0, PW, bh, 0xFFFFFFFFul);
        g_api.RenderPageBitmap(bmp, page, x0, y0 - by, rw, rh, rotate,
                               kFlagAnnot | kFlagPrinting | kNoSmoothText | kNoSmoothPath);
        g_api.BitmapDestroy(bmp);
        for (int y = 0; y < bh; ++y) {
            const uint8_t* s = buf.data() + size_t(y) * PW * 4;
            uint8_t* d = out.data.data() + size_t(by + y) * PW;
            for (int x = 0; x < PW; ++x, s += 4) d[x] = uint8_t((114 * s[0] + 587 * s[1] + 299 * s[2] + 500) / 1000);
        }
    }
    g_api.ClosePage(page);
    return out;
}

}  // namespace mb2020
