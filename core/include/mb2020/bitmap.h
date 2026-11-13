// SPDX-License-Identifier: GPL-2.0-or-later
// Простые растровые изображения: 1 бит (печать) и 8 бит серый / RGB (скан, ввод).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mb2020 {

// 1 бит на пиксель, MSB первым, строка выровнена на байт. 1 = чёрный (как в JBIG).
struct Bitmap1 {
    int width = 0;
    int height = 0;
    int stride = 0;  // байт на строку
    std::vector<uint8_t> bits;

    Bitmap1() = default;
    Bitmap1(int w, int h) : width(w), height(h), stride((w + 7) / 8), bits(size_t(stride) * h, 0) {}

    bool get(int x, int y) const { return (bits[size_t(y) * stride + (x >> 3)] >> (7 - (x & 7))) & 1; }
    void set(int x, int y, bool black) {
        uint8_t& b = bits[size_t(y) * stride + (x >> 3)];
        uint8_t m = uint8_t(0x80 >> (x & 7));
        b = black ? uint8_t(b | m) : uint8_t(b & ~m);
    }
    const uint8_t* row(int y) const { return bits.data() + size_t(y) * stride; }
    uint8_t* row(int y) { return bits.data() + size_t(y) * stride; }
    // Вырезать полосу строк [y0, y0+lines)
    Bitmap1 band(int y0, int lines) const;
    // Заливка прямоугольника (для тестовых страниц)
    void fill_rect(int x0, int y0, int x1, int y1, bool black = true);
};

// 8 бит на канал, channels = 1 (серый) или 3 (RGB). 0 = чёрный, 255 = белый.
struct Image8 {
    int width = 0;
    int height = 0;
    int channels = 1;
    std::vector<uint8_t> data;

    Image8() = default;
    Image8(int w, int h, int ch) : width(w), height(h), channels(ch), data(size_t(w) * h * ch, 255) {}
};

}  // namespace mb2020
