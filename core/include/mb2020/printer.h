// SPDX-License-Identifier: GPL-2.0-or-later
// Отправка готового задания на МФУ (RAW TCP 9100).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mb2020 {

// Отправляет поток на host:port одним соединением и ждёт, пока МФУ закроет соединение.
// Бросает std::runtime_error.
void send_raw(const std::string& host, const std::vector<uint8_t>& job, uint16_t port = 9100, int timeout_ms = 30000);

}  // namespace mb2020
