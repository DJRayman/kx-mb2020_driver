// SPDX-License-Identifier: GPL-2.0-or-later
#include "mb2020/raster.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "mb2020/gdi_print.h"

#include "stb_image.h"
#include "stb_image_write.h"

namespace mb2020 {

Image8 to_gray(const Image8& img) {
    if (img.channels == 1) return img;
    Image8 g(img.width, img.height, 1);
    const size_t n = size_t(img.width) * img.height;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.data[i * img.channels];
        g.data[i] = uint8_t((299 * p[0] + 587 * p[1] + 114 * p[2] + 500) / 1000);
    }
    return g;
}

namespace {

// Масштабирование серого изображения усреднением по площади (вниз) / билинейно (вверх).
std::vector<float> resample(const Image8& g, int dw, int dh) {
    std::vector<float> out(size_t(dw) * dh);
    const double sx = double(g.width) / dw, sy = double(g.height) / dh;
    if (sx >= 1.0 && sy >= 1.0) {
        // интегральное изображение
        const int W = g.width, H = g.height;
        std::vector<double> I(size_t(W + 1) * (H + 1), 0.0);
        for (int y = 0; y < H; ++y) {
            double row = 0;
            for (int x = 0; x < W; ++x) {
                row += g.data[size_t(y) * W + x];
                I[size_t(y + 1) * (W + 1) + x + 1] = I[size_t(y) * (W + 1) + x + 1] + row;
            }
        }
        auto at = [&](int x, int y) { return I[size_t(y) * (W + 1) + x]; };
        for (int y = 0; y < dh; ++y) {
            int y0 = int(y * sy), y1 = std::max(y0 + 1, std::min(H, int((y + 1) * sy)));
            for (int x = 0; x < dw; ++x) {
                int x0 = int(x * sx), x1 = std::max(x0 + 1, std::min(W, int((x + 1) * sx)));
                double s = at(x1, y1) - at(x0, y1) - at(x1, y0) + at(x0, y0);
                out[size_t(y) * dw + x] = float(s / ((x1 - x0) * (y1 - y0)));
            }
        }
    } else {
        for (int y = 0; y < dh; ++y) {
            double fy = std::max(0.0, (y + 0.5) * sy - 0.5);
            int y0 = std::min(int(fy), g.height - 1), y1 = std::min(y0 + 1, g.height - 1);
            double ty = fy - y0;
            for (int x = 0; x < dw; ++x) {
                double fx = std::max(0.0, (x + 0.5) * sx - 0.5);
                int x0 = std::min(int(fx), g.width - 1), x1 = std::min(x0 + 1, g.width - 1);
                double tx = fx - x0;
                auto p = [&](int xx, int yy) { return double(g.data[size_t(yy) * g.width + xx]); };
                double v = (p(x0, y0) * (1 - tx) + p(x1, y0) * tx) * (1 - ty) + (p(x0, y1) * (1 - tx) + p(x1, y1) * tx) * ty;
                out[size_t(y) * dw + x] = float(v);
            }
        }
    }
    return out;
}

bool looks_bilevel(const Image8& g) {
    size_t mid = 0, n = g.data.size();
    for (uint8_t v : g.data)
        if (v > 40 && v < 215) ++mid;
    return n == 0 || mid * 100 < n * 3;  // меньше 3% полутонов — текст/чертёж
}

}  // namespace

Bitmap1 render_page(const Image8& src, const RasterOptions& opt) {
    Image8 g = to_gray(src);
    if (g.width <= 0 || g.height <= 0) throw std::invalid_argument("render_page: пустое изображение");
    const int PW = kA4Width600, PH = kA4Height600;

    int dw, dh;
    if (opt.fit) {
        double k = std::min(double(PW) / g.width, double(PH) / g.height);
        dw = std::max(1, int(std::lround(g.width * k)));
        dh = std::max(1, int(std::lround(g.height * k)));
    } else {
        double k = 600.0 / (opt.src_dpi > 0 ? opt.src_dpi : 600.0);
        dw = std::min(PW, std::max(1, int(std::lround(g.width * k))));
        dh = std::min(PH, std::max(1, int(std::lround(g.height * k))));
    }
    std::vector<float> img = resample(g, dw, dh);

    Dither mode = opt.dither;
    if (mode == Dither::Auto) mode = looks_bilevel(g) ? Dither::Threshold : Dither::FloydSteinberg;

    Bitmap1 page(PW, PH);
    const int ox = (PW - dw) / 2, oy = opt.fit ? (PH - dh) / 2 : 0;
    if (mode == Dither::Threshold) {
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
                if (img[size_t(y) * dw + x] < opt.threshold) page.set(ox + x, oy + y, true);
    } else {
        // Floyd–Steinberg, змейкой
        for (int y = 0; y < dh; ++y) {
            const bool l2r = (y % 2) == 0;
            for (int i = 0; i < dw; ++i) {
                const int x = l2r ? i : dw - 1 - i;
                float& v = img[size_t(y) * dw + x];
                const float nv = v < 128 ? 0.f : 255.f;
                const float e = v - nv;
                if (nv == 0.f) page.set(ox + x, oy + y, true);
                const int dx = l2r ? 1 : -1;
                if (x + dx >= 0 && x + dx < dw) img[size_t(y) * dw + x + dx] += e * 7 / 16;
                if (y + 1 < dh) {
                    if (x - dx >= 0 && x - dx < dw) img[size_t(y + 1) * dw + x - dx] += e * 3 / 16;
                    img[size_t(y + 1) * dw + x] += e * 5 / 16;
                    if (x + dx >= 0 && x + dx < dw) img[size_t(y + 1) * dw + x + dx] += e * 1 / 16;
                }
            }
        }
    }
    return page;
}

Image8 load_image(const std::string& path, int want_channels) {
    int w, h, n;
    unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, want_channels);
    if (!p) throw std::runtime_error("не удалось открыть изображение " + path + ": " + stbi_failure_reason());
    Image8 img(w, h, want_channels);
    std::copy(p, p + size_t(w) * h * want_channels, img.data.begin());
    stbi_image_free(p);
    return img;
}

namespace {
bool ends_with(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); ++i)
        if (std::tolower((unsigned char)s[s.size() - suf.size() + i]) != suf[i]) return false;
    return true;
}
}  // namespace

void save_image(const std::string& path, const Image8& img, int jpeg_quality) {
    int ok;
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg"))
        ok = stbi_write_jpg(path.c_str(), img.width, img.height, img.channels, img.data.data(), jpeg_quality);
    else if (ends_with(path, ".png")) {
        stbi_write_png_compression_level = 6;
        ok = stbi_write_png(path.c_str(), img.width, img.height, img.channels, img.data.data(), img.width * img.channels);
    } else
        throw std::runtime_error("неизвестный формат файла (нужно .png или .jpg): " + path);
    if (!ok) throw std::runtime_error("не удалось записать " + path);
}

void save_bitmap1_png(const std::string& path, const Bitmap1& bm) {
    Image8 g(bm.width, bm.height, 1);
    for (int y = 0; y < bm.height; ++y)
        for (int x = 0; x < bm.width; ++x) g.data[size_t(y) * bm.width + x] = bm.get(x, y) ? 0 : 255;
    save_image(path, g);
}

}  // namespace mb2020
