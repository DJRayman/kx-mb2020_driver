// SPDX-License-Identifier: GPL-2.0-or-later
// Рендер PDF через PDFium (BSD/Apache). Библиотека подгружается во время работы:
// pdfium.dll (Windows) / libpdfium.so (Linux, Android) рядом с программой или по пути.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mb2020/bitmap.h"

namespace mb2020 {

enum class PdfScale {
    Auto,    // натуральная величина, если страница не больше A4; иначе вписать в печатаемую область
    Fit,     // всегда вписать в печатаемую область (ничего не обрежется у краёв)
    Actual,  // всегда натуральная величина (что вне печатаемой области — обрежется)
};

struct PdfRenderOptions {
    PdfScale scale = PdfScale::Auto;
    bool auto_rotate = true;  // альбомные страницы поворачивать на 90°
};

class PdfDocument {
public:
    // Загружает PDFium. lib_path пусто = искать рядом с программой, затем в системе.
    // Бросает std::runtime_error, если библиотеку не найти.
    static void load_library(const std::string& lib_path = "");

    // Открывает PDF из памяти. Бросает std::runtime_error (повреждён, нужен пароль…).
    explicit PdfDocument(std::vector<uint8_t> data, const std::string& password = "");
    ~PdfDocument();
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    int page_count() const;
    // Размер страницы в пунктах (1/72 дюйма).
    void page_size_pt(int index, double& w, double& h) const;
    // Рендер страницы в серое изображение печатаемой области A4 при 600 dpi (kA4Width600 x kA4Height600).
    Image8 render_a4_600(int index, const PdfRenderOptions& opt) const;

private:
    std::vector<uint8_t> data_;
    void* doc_ = nullptr;
};

}  // namespace mb2020
