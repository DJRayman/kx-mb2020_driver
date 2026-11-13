// SPDX-License-Identifier: GPL-2.0-or-later
#include "mb2020/net.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
typedef int socklen_t;
#define MB_CLOSESOCKET closesocket
static int last_error() { return WSAGetLastError(); }
static bool err_addrinuse(int e) { return e == WSAEADDRINUSE || e == WSAEACCES; }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#define MB_CLOSESOCKET ::close
static int last_error() { return errno; }
static bool err_addrinuse(int e) { return e == EADDRINUSE || e == EACCES; }
#endif

namespace mb2020 {
namespace net {

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + " (код " + std::to_string(last_error()) + ")");
}

sockaddr_in to_sa(const Endpoint& e) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    std::memcpy(&sa.sin_addr, e.ip.data(), 4);
    sa.sin_port = htons(e.port);
    return sa;
}

Endpoint from_sa(const sockaddr_in& sa) {
    Endpoint e;
    std::memcpy(e.ip.data(), &sa.sin_addr, 4);
    e.port = ntohs(sa.sin_port);
    return e;
}

// Ожидание готовности сокета: true — готов, false — таймаут.
bool wait_ready(intptr_t fd, bool for_write, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd = (SOCKET)fd;
    p.events = for_write ? POLLWRNORM : POLLRDNORM;
    int r = WSAPoll(&p, 1, timeout_ms);
#else
    pollfd p{};
    p.fd = (int)fd;
    p.events = for_write ? POLLOUT : POLLIN;
    int r;
    do r = ::poll(&p, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
#endif
    if (r < 0) fail("poll");
    return r > 0;
}

uint16_t sock_port(const Socket& s) {
    sockaddr_in sa{};
    socklen_t l = sizeof sa;
    if (getsockname((decltype(socket(0, 0, 0)))s.fd(), (sockaddr*)&sa, &l) != 0) fail("getsockname");
    return ntohs(sa.sin_port);
}

}  // namespace

void init() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) throw std::runtime_error("WSAStartup не удался");
        done = true;
    }
#endif
}

std::string Endpoint::to_string() const {
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]) + ":" + std::to_string(port);
}

Endpoint resolve(const std::string& host, uint16_t port) {
    Endpoint e;
    e.port = port;
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
        std::memcpy(e.ip.data(), &a, 4);
        return e;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        throw std::runtime_error("не удалось найти адрес " + host);
    std::memcpy(e.ip.data(), &((sockaddr_in*)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return e;
}

Socket::~Socket() { close(); }
Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}
void Socket::close() {
    if (fd_ != -1) {
        MB_CLOSESOCKET((decltype(socket(0, 0, 0)))fd_);
        fd_ = -1;
    }
}

// ---------- UDP ----------
Udp::Udp(uint16_t local_port) {
    auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((intptr_t)fd == -1) fail("socket(UDP)");
    s_ = Socket((intptr_t)fd);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(local_port);
    if (bind(fd, (sockaddr*)&sa, sizeof sa) != 0) fail("bind(UDP)");
}

void Udp::send_to(const Endpoint& to, const std::vector<uint8_t>& data) {
    sockaddr_in sa = to_sa(to);
    auto r = sendto((decltype(socket(0, 0, 0)))s_.fd(), (const char*)data.data(), (int)data.size(), 0, (sockaddr*)&sa, sizeof sa);
    if (r < 0) fail("sendto");
}

bool Udp::recv_from(std::vector<uint8_t>& data, Endpoint& from, int timeout_ms) {
    if (!wait_ready(s_.fd(), false, timeout_ms)) return false;
    data.resize(65536);
    sockaddr_in sa{};
    socklen_t l = sizeof sa;
    auto r = recvfrom((decltype(socket(0, 0, 0)))s_.fd(), (char*)data.data(), (int)data.size(), 0, (sockaddr*)&sa, &l);
    if (r < 0) {
#ifdef _WIN32
        // ICMP port unreachable приходит как WSAECONNRESET — считаем «ответа нет».
        if (WSAGetLastError() == WSAECONNRESET) return false;
#endif
        fail("recvfrom");
    }
    data.resize((size_t)r);
    from = from_sa(sa);
    return true;
}

uint16_t Udp::local_port() const { return sock_port(s_); }

// ---------- TCP ----------
TcpStream TcpStream::connect(const Endpoint& to, int timeout_ms) {
    auto fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((intptr_t)fd == -1) fail("socket(TCP)");
    Socket s((intptr_t)fd);
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
    sockaddr_in sa = to_sa(to);
    int r = ::connect(fd, (sockaddr*)&sa, sizeof sa);
    if (r != 0) {
#ifdef _WIN32
        bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        bool pending = errno == EINPROGRESS;
#endif
        if (!pending) fail("не удалось подключиться к " + to.to_string());
        if (!wait_ready((intptr_t)fd, true, timeout_ms)) throw std::runtime_error("таймаут подключения к " + to.to_string());
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &l);
        if (err != 0) throw std::runtime_error("не удалось подключиться к " + to.to_string() + " (код " + std::to_string(err) + ")");
    }
#ifdef _WIN32
    nb = 0;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    fcntl(fd, F_SETFL, fl);
#endif
    return TcpStream(std::move(s));
}

void TcpStream::write_all(const uint8_t* p, size_t n) {
    while (n > 0) {
        int chunk = (int)std::min<size_t>(n, 1 << 20);
        auto r = send((decltype(socket(0, 0, 0)))s_.fd(), (const char*)p, chunk, 0);
        if (r <= 0) fail("send");
        p += r;
        n -= (size_t)r;
    }
}

size_t TcpStream::read_some(uint8_t* p, size_t n, int timeout_ms) {
    if (!wait_ready(s_.fd(), false, timeout_ms)) throw std::runtime_error("таймаут чтения TCP");
    auto r = recv((decltype(socket(0, 0, 0)))s_.fd(), (char*)p, (int)std::min<size_t>(n, 1 << 20), 0);
    if (r < 0) fail("recv");
    return (size_t)r;
}

void TcpStream::shutdown_send() {
#ifdef _WIN32
    shutdown((SOCKET)s_.fd(), SD_SEND);
#else
    shutdown((int)s_.fd(), SHUT_WR);
#endif
}

Endpoint TcpStream::peer() const {
    sockaddr_in sa{};
    socklen_t l = sizeof sa;
    getpeername((decltype(socket(0, 0, 0)))s_.fd(), (sockaddr*)&sa, &l);
    return from_sa(sa);
}

TcpListener::TcpListener(uint16_t port, bool loopback_only) {
    auto fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((intptr_t)fd == -1) fail("socket(TCP)");
    s_ = Socket((intptr_t)fd);
#ifdef _WIN32
    BOOL excl = TRUE;  // не делить порт с другим процессом (например, службой Panasonic)
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
#else
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(fd, (sockaddr*)&sa, sizeof sa) != 0) {
        int e = last_error();
        if (err_addrinuse(e))
            throw std::runtime_error("TCP-порт " + std::to_string(port) +
                                     " занят другой программой (обычно служба «Panasonic Local Printer Service» / PCCMFLPD.exe)");
        fail("bind(TCP " + std::to_string(port) + ")");
    }
    if (listen(fd, 4) != 0) fail("listen");
}

bool TcpListener::accept(std::unique_ptr<TcpStream>& out, int timeout_ms) {
    if (!wait_ready(s_.fd(), false, timeout_ms)) return false;
    auto c = ::accept((decltype(socket(0, 0, 0)))s_.fd(), nullptr, nullptr);
    if ((intptr_t)c == -1) fail("accept");
    out.reset(new TcpStream(Socket((intptr_t)c)));
    return true;
}

uint16_t TcpListener::local_port() const { return sock_port(s_); }

// ---------- сведения о ПК ----------
std::array<uint8_t, 4> local_ip_towards(const Endpoint& dest) {
    auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((intptr_t)fd == -1) fail("socket");
    Socket s((intptr_t)fd);
    sockaddr_in sa = to_sa(dest);
    if (sa.sin_port == 0) sa.sin_port = htons(9);
    if (::connect(fd, (sockaddr*)&sa, sizeof sa) != 0) fail("нет маршрута до " + dest.to_string());
    sockaddr_in me{};
    socklen_t l = sizeof me;
    getsockname(fd, (sockaddr*)&me, &l);
    std::array<uint8_t, 4> ip{};
    std::memcpy(ip.data(), &me.sin_addr, 4);
    return ip;
}

std::array<uint8_t, 6> mac_for_ip(const std::array<uint8_t, 4>& ip) {
    std::array<uint8_t, 6> mac{};
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != NO_ERROR) return mac;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next)
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = (sockaddr_in*)u->Address.lpSockaddr;
            if (sa->sin_family == AF_INET && std::memcmp(&sa->sin_addr, ip.data(), 4) == 0 && a->PhysicalAddressLength == 6) {
                std::memcpy(mac.data(), a->PhysicalAddress, 6);
                return mac;
            }
        }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return mac;
    std::string name;
    for (auto* i = ifs; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
            std::memcmp(&((sockaddr_in*)i->ifa_addr)->sin_addr, ip.data(), 4) == 0)
            name = i->ifa_name;
    freeifaddrs(ifs);
    if (name.empty()) return mac;
    std::ifstream f("/sys/class/net/" + name + "/address");
    std::string s;
    if (f >> s && s.size() >= 17)
        for (int k = 0; k < 6; ++k) mac[k] = (uint8_t)std::stoi(s.substr(k * 3, 2), nullptr, 16);
#endif
    return mac;
}

std::string hostname() {
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) != 0) return "PC";
    return buf;
}

}  // namespace net
}  // namespace mb2020
