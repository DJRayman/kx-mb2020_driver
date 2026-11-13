// SPDX-License-Identifier: GPL-2.0-or-later
// Подготовка страницы к печати: масштабирование картинки на A4 и перевод в 1 бит.
#pragma once
#include <string>

#include "mb2020/bitmap.h"

namespace mb2020 {

enum class Dither { Auto, Threshold, FloydSteinberg };

struct RasterOptions {
    Dither dither = Dither::Auto;
    int threshold = 128;   // для Threshold
    bool fit = true;       // вписать в печатаемую область с сохранением пропорций
    double src_dpi = 0;    // если fit = false: dpi исходной картинки (0 = 600)
};

// Серое изображение -> страница A4 600 dpi (kA4Width600 x kA4Height600).
Bitmap1 render_page(const Image8& gray, const RasterOptions& opt);

// RGB/серое -> серое (яркость).
Image8 to_gray(const Image8& img);

// Загрузка PNG/JPEG/BMP/GIF/TGA/PNM (stb_image). Бросает std::runtime_error.
Image8 load_image(const std::string& path, int want_channels);

// Сохранение: .png или .jpg (по расширению). Бросает std::runtime_error.
void save_image(const std::string& path, const Image8& img, int jpeg_quality = 90);
void save_bitmap1_png(const std::string& path, const Bitmap1& bm);

}  // namespace mb2020
