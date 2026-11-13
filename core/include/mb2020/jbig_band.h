// SPDX-License-Identifier: GPL-2.0-or-later
// JBIG-1 (ITU-T T.82) для полос печати KX-MB2020 на основе jbig-kit.
#pragma once
#include <cstdint>
#include <vector>

#include "mb2020/bitmap.h"

namespace mb2020 {

// Кодирует одну полосу в BIE: BIH(20) + SDE + FF 02.
// Параметры как у драйвера Panasonic: одна полоса (L0 = высота), LRLTWO | TPBON, order = 0.
// panasonic_pad: дописать лишний 0x00 перед FF 02, как делает кодер Panasonic (байт-в-байт).
std::vector<uint8_t> jbig_encode_band(const Bitmap1& band, bool panasonic_pad = true);

// Декодирует BIE обратно в растр. Бросает std::runtime_error при ошибке.
Bitmap1 jbig_decode(const uint8_t* bie, size_t len);

}  // namespace mb2020
