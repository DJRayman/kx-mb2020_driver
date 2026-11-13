// SPDX-License-Identifier: GPL-2.0-or-later
// Формат задания печати Panasonic KX-MB2020 GDI: UEL + PJL + ESC-команды + полосы JBIG.
// Описание: docs/protocol-print.md
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "mb2020/bitmap.h"

namespace mb2020 {

// Печатаемая область A4 при 600 dpi (как у драйвера Panasonic: поля ~4,23 мм).
constexpr int kA4Width600 = 4760;
constexpr int kA4Height600 = 6814;
constexpr int kBandLines = 512;
constexpr int kRawPrintPort = 9100;

struct PrintJobInfo {
    std::string name = "document";
    std::string owner = "user";
    std::string host = "PC";
    std::string pc_ip = "0.0.0.0";
    std::string time;        // "yyyy/MM/dd HH:mm:ss"; пусто = текущее время
    int copies = 1;          // проверено только 1
    bool economode = false;  // проверено только OFF
};

// Собирает задание для одной страницы A4 600 dpi (страница должна быть kA4Width600 x kA4Height600).
// Многостраничные задания в протоколе ещё не исследованы: для нескольких страниц
// вызывайте функцию по разу на страницу и отправляйте задания подряд.
std::vector<uint8_t> build_gdi_job(const Bitmap1& page, const PrintJobInfo& info);

struct GdiBand {
    size_t offset = 0;   // смещение ESC*b0W в потоке
    uint32_t flags = 0;  // 0x10000 = первая, 0x1 = последняя
    int lines = 0;
    std::vector<uint8_t> bie;
};

// Разбор задания: полосы и размер страницы (из ESC*r<H>T / ESC*r<W>S).
struct GdiJob {
    int width = 0;
    int height = 0;
    std::vector<GdiBand> bands;
};
GdiJob parse_gdi_job(const std::vector<uint8_t>& stream);

// Раскодировать все полосы в страницу.
Bitmap1 decode_gdi_page(const GdiJob& job);

std::string current_pjl_time();

}  // namespace mb2020
