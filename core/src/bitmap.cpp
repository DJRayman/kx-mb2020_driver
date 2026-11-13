// SPDX-License-Identifier: GPL-2.0-or-later
#include "mb2020/bitmap.h"

#include <algorithm>
#include <cstring>

namespace mb2020 {

Bitmap1 Bitmap1::band(int y0, int lines) const {
    Bitmap1 b(width, lines);
    std::memcpy(b.bits.data(), bits.data() + size_t(y0) * stride, size_t(lines) * stride);
    return b;
}

void Bitmap1::fill_rect(int x0, int y0, int x1, int y1, bool black) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(width, x1);
    y1 = std::min(height, y1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) set(x, y, black);
}

}  // namespace mb2020
