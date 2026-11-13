// SPDX-License-Identifier: GPL-2.0-or-later
// Обёртка над jbig-kit (Markus Kuhn, GPL) для полос печати Panasonic GDI.
#include "mb2020/jbig_band.h"

#include <stdexcept>
#include <string>

extern "C" {
#include "jbig.h"
}

namespace mb2020 {

namespace {
void collect(unsigned char* start, size_t len, void* file) {
    auto* out = static_cast<std::vector<uint8_t>*>(file);
    out->insert(out->end(), start, start + len);
}
}  // namespace

std::vector<uint8_t> jbig_encode_band(const Bitmap1& band, bool panasonic_pad) {
    if (band.width <= 0 || band.height <= 0) throw std::invalid_argument("jbig: пустая полоса");
    std::vector<uint8_t> out;
    // jbig-kit не меняет входной растр, но принимает неконстантный указатель.
    std::vector<uint8_t> copy = band.bits;
    unsigned char* planes[1] = {copy.data()};
    jbg_enc_state s;
    jbg_enc_init(&s, (unsigned long)band.width, (unsigned long)band.height, 1, planes, collect, &out);
    jbg_enc_layers(&s, 0);
    jbg_enc_options(&s, 0, JBG_LRLTWO | JBG_TPBON, (unsigned long)band.height, 0, 0);
    jbg_enc_out(&s);
    jbg_enc_free(&s);

    if (out.size() < 22 || out[out.size() - 2] != 0xff || out[out.size() - 1] != 0x02)
        throw std::runtime_error("jbig: неожиданный конец BIE (нет FF 02)");
    if (panasonic_pad) out.insert(out.end() - 2, uint8_t(0x00));
    return out;
}

Bitmap1 jbig_decode(const uint8_t* bie, size_t len) {
    jbg_dec_state s;
    jbg_dec_init(&s);
    jbg_dec_maxsize(&s, 20000, 20000);
    size_t used = 0;
    int r = jbg_dec_in(&s, const_cast<unsigned char*>(bie), len, &used);
    if (r != JBG_EOK && r != JBG_EOK_INTR) {
        std::string msg = std::string("jbig: ошибка декодирования: ") + jbg_strerror(r, JBG_EN);
        jbg_dec_free(&s);
        throw std::runtime_error(msg);
    }
    Bitmap1 bm((int)jbg_dec_getwidth(&s), (int)jbg_dec_getheight(&s));
    const unsigned char* img = jbg_dec_getimage(&s, 0);
    std::copy(img, img + bm.bits.size(), bm.bits.begin());
    jbg_dec_free(&s);
    return bm;
}

}  // namespace mb2020
