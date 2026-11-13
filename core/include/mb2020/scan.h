// SPDX-License-Identifier: GPL-2.0-or-later
// Сетевое сканирование KX-MB2020: команды MFPP по UDP + приём картинки по LPR/LPD.
// Описание: docs/protocol-scan.md
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mb2020/bitmap.h"
#include "mb2020/net.h"

namespace mb2020 {

constexpr uint16_t kMfppPort = 3892;  // UDP на МФУ и TCP (LPD) на ПК

// ---------- MFPP ----------
struct MfppPacket {
    uint16_t seq = 0;
    uint8_t cls = 0;   // 0x31 обнаружение, 0x32 сканирование
    uint8_t code = 0;
    std::vector<uint8_t> body;
};
std::vector<uint8_t> mfpp_encode(const MfppPacket& p);
// false — не MFPP или обрезан.
bool mfpp_decode(const std::vector<uint8_t>& d, MfppPacket& out);

// Тело объявления ПК (31 03 / 31 05): IPv4 + имя (16, заглавные) + MAC + 00 0f.
std::vector<uint8_t> mfpp_announce_body(const std::array<uint8_t, 4>& ip, const std::string& pc_name,
                                        const std::array<uint8_t, 6>& mac);

struct ScanParams {
    int mode = 3;       // 3 = цвет (проверено); другие режимы не исследованы
    int dpi = 300;      // проверено только 300
    int width = 2480;   // A4 при 300 dpi
    int height = 3508;
};
std::vector<uint8_t> mfpp_scan_params_body(const ScanParams& p);

// ---------- данные скана ----------
struct ScanDataHeader {
    uint32_t mode = 0, dpi_x = 0, dpi_y = 0, width = 0, requested_height = 0;
};
constexpr size_t kScanHeaderSize = 128;
ScanDataHeader parse_scan_header(const std::vector<uint8_t>& data);
// Data-файл (заголовок 128 байт + строки RGB/серого) -> изображение.
Image8 decode_scan_data(const std::vector<uint8_t>& data);

// ---------- LPD ----------
struct LpdJob {
    std::string queue;
    std::string control;
    std::string data_name;
    std::vector<uint8_t> data;
};
using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;
// Обслуживает одно LPR-соединение («receive job»). Бросает при ошибке протокола/таймауте.
LpdJob lpd_receive(net::TcpStream& s, int timeout_ms, const ProgressFn& progress = nullptr);

// ---------- сканер ----------
struct ScanOptions {
    std::string device;                // IP МФУ
    uint16_t device_port = kMfppPort;  // для тестов
    uint16_t lpd_port = kMfppPort;     // для тестов
    bool lpd_loopback_only = false;    // для тестов
    std::string pc_name;               // пусто = имя ПК
    int reply_timeout_ms = 3000;       // ожидание 32 43
    int data_timeout_s = 180;          // ожидание картинки
    ScanParams params;
    std::function<void(const std::string&)> log;
    ProgressFn progress;
};

struct ScanResult {
    ScanDataHeader header;
    Image8 image;
    std::string job_id;  // напр. "f0185304:0005"
};

// Выполняет один скан со стекла. Бросает std::runtime_error с понятным текстом.
ScanResult scan_once(const ScanOptions& opt);

}  // namespace mb2020
