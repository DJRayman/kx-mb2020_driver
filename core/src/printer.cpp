// SPDX-License-Identifier: GPL-2.0-or-later
#include "mb2020/printer.h"

#include <stdexcept>

#include "mb2020/net.h"

namespace mb2020 {

void send_raw(const std::string& host, const std::vector<uint8_t>& job, uint16_t port, int timeout_ms) {
    net::init();
    net::TcpStream s = net::TcpStream::connect(net::resolve(host, port), timeout_ms);
    s.write_all(job);
    s.shutdown_send();
    // МФУ ничего не отвечает и закрывает соединение после приёма; дождёмся этого.
    uint8_t buf[256];
    try {
        while (s.read_some(buf, sizeof buf, timeout_ms) > 0) {
        }
    } catch (const std::exception&) {
        // таймаут ожидания закрытия не считаем ошибкой: данные уже переданы
    }
}

}  // namespace mb2020
