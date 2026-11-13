// SPDX-License-Identifier: GPL-2.0-or-later
// Минимальная кроссплатформенная обёртка над сокетами (POSIX / Winsock).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mb2020 {
namespace net {

// Вызвать один раз до работы с сетью (на Windows — WSAStartup).
void init();

struct Endpoint {
    std::array<uint8_t, 4> ip{};  // IPv4
    uint16_t port = 0;
    std::string to_string() const;
};
// "192.168.1.50" или имя хоста. Бросает std::runtime_error.
Endpoint resolve(const std::string& host, uint16_t port);

class Socket {
public:
    Socket() = default;
    explicit Socket(intptr_t fd) : fd_(fd) {}
    ~Socket();
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Socket& operator=(Socket&& o) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    bool valid() const { return fd_ != -1; }
    intptr_t fd() const { return fd_; }
    void close();

private:
    intptr_t fd_ = -1;
};

// UDP-сокет на эфемерном порту (или заданном).
class Udp {
public:
    explicit Udp(uint16_t local_port = 0);
    void send_to(const Endpoint& to, const std::vector<uint8_t>& data);
    // Ждёт датаграмму до timeout_ms. false — таймаут.
    bool recv_from(std::vector<uint8_t>& data, Endpoint& from, int timeout_ms);
    uint16_t local_port() const;

private:
    Socket s_;
};

class TcpStream {
public:
    explicit TcpStream(Socket s) : s_(std::move(s)) {}
    static TcpStream connect(const Endpoint& to, int timeout_ms);
    void write_all(const uint8_t* p, size_t n);
    void write_all(const std::vector<uint8_t>& v) { write_all(v.data(), v.size()); }
    // Читает до n байт; 0 = соединение закрыто. Бросает при таймауте/ошибке.
    size_t read_some(uint8_t* p, size_t n, int timeout_ms);
    void shutdown_send();
    Endpoint peer() const;

private:
    Socket s_;
};

class TcpListener {
public:
    // Бросает std::runtime_error с понятным текстом, если порт занят.
    explicit TcpListener(uint16_t port, bool loopback_only = false);
    // false — таймаут.
    bool accept(std::unique_ptr<TcpStream>& out, int timeout_ms);
    uint16_t local_port() const;

private:
    Socket s_;
};

// IPv4-адрес этого ПК, через который идёт маршрут до dest.
std::array<uint8_t, 4> local_ip_towards(const Endpoint& dest);
// MAC интерфейса с данным IPv4 (нули, если узнать не удалось).
std::array<uint8_t, 6> mac_for_ip(const std::array<uint8_t, 4>& ip);
std::string hostname();

}  // namespace net
}  // namespace mb2020
