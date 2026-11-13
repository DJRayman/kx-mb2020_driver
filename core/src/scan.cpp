// SPDX-License-Identifier: GPL-2.0-or-later
#include "mb2020/scan.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace mb2020 {

namespace {
const char kMagic[] = "MFPP0200ML01";  // 12 байт
void put_u32(std::vector<uint8_t>& o, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) o[at + i] = uint8_t(v >> (8 * i));
}
uint32_t get_u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}
}  // namespace

std::vector<uint8_t> mfpp_encode(const MfppPacket& p) {
    std::vector<uint8_t> o(20 + p.body.size());
    std::memcpy(o.data(), kMagic, 12);
    o[12] = uint8_t(p.seq);
    o[13] = uint8_t(p.seq >> 8);
    o[14] = p.cls;
    o[15] = p.code;
    put_u32(o, 16, uint32_t(p.body.size()));
    std::copy(p.body.begin(), p.body.end(), o.begin() + 20);
    return o;
}

bool mfpp_decode(const std::vector<uint8_t>& d, MfppPacket& out) {
    if (d.size() < 20 || std::memcmp(d.data(), kMagic, 12) != 0) return false;
    uint32_t len = get_u32(&d[16]);
    if (d.size() < 20 + size_t(len)) return false;
    out.seq = uint16_t(d[12] | (d[13] << 8));
    out.cls = d[14];
    out.code = d[15];
    out.body.assign(d.begin() + 20, d.begin() + 20 + len);
    return true;
}

std::vector<uint8_t> mfpp_announce_body(const std::array<uint8_t, 4>& ip, const std::string& pc_name,
                                        const std::array<uint8_t, 6>& mac) {
    std::vector<uint8_t> b(28, 0);
    std::copy(ip.begin(), ip.end(), b.begin());
    for (size_t i = 0; i < pc_name.size() && i < 15; ++i) {
        unsigned char c = (unsigned char)pc_name[i];
        b[4 + i] = (c >= 'a' && c <= 'z') ? uint8_t(c - 32) : (c >= 0x20 && c < 0x7f ? c : '_');
    }
    std::copy(mac.begin(), mac.end(), b.begin() + 20);
    b[26] = 0x00;
    b[27] = 0x0f;
    return b;
}

std::vector<uint8_t> mfpp_scan_params_body(const ScanParams& p) {
    std::vector<uint8_t> b(0x80, 0);
    put_u32(b, 0x00, uint32_t(p.mode));
    put_u32(b, 0x04, uint32_t(p.mode));
    put_u32(b, 0x08, uint32_t(p.mode));
    put_u32(b, 0x0C, uint32_t(p.dpi));
    put_u32(b, 0x10, uint32_t(p.dpi));
    put_u32(b, 0x1C, uint32_t(p.width));
    put_u32(b, 0x20, uint32_t(p.height));
    return b;
}

ScanDataHeader parse_scan_header(const std::vector<uint8_t>& d) {
    if (d.size() < kScanHeaderSize) throw std::runtime_error("скан: данных меньше заголовка");
    ScanDataHeader h;
    h.mode = get_u32(&d[0x00]);
    h.dpi_x = get_u32(&d[0x0C]);
    h.dpi_y = get_u32(&d[0x10]);
    h.width = get_u32(&d[0x1C]);
    h.requested_height = get_u32(&d[0x20]);
    return h;
}

Image8 decode_scan_data(const std::vector<uint8_t>& d) {
    ScanDataHeader h = parse_scan_header(d);
    if (h.width == 0 || h.width > 20000) throw std::runtime_error("скан: странная ширина " + std::to_string(h.width));
    const size_t payload = d.size() - kScanHeaderSize;
    // Цвет (режим 3) — RGB; для других режимов определяем по кратности размера (не проверено на железе).
    int ch = 3;
    if (h.mode != 3 && payload % (size_t(h.width) * 3) != 0 && payload % h.width == 0) ch = 1;
    const size_t stride = size_t(h.width) * ch;
    const int height = int(payload / stride);
    if (height <= 0) throw std::runtime_error("скан: нет строк изображения");
    Image8 img(int(h.width), height, ch);
    std::memcpy(img.data.data(), d.data() + kScanHeaderSize, stride * height);
    return img;
}

// ---------- LPD ----------
namespace {
struct Reader {
    net::TcpStream& s;
    int timeout_ms;
    std::vector<uint8_t> buf;
    size_t pos = 0;

    bool fill() {
        if (pos < buf.size()) return true;
        buf.resize(1 << 16);
        size_t n = s.read_some(buf.data(), buf.size(), timeout_ms);
        buf.resize(n);
        pos = 0;
        return n > 0;
    }
    // Строка до '\n'; false — соединение закрыто до начала строки.
    bool line(std::string& out) {
        out.clear();
        for (;;) {
            if (!fill()) {
                if (out.empty()) return false;
                throw std::runtime_error("LPD: обрыв посреди команды");
            }
            uint8_t c = buf[pos++];
            if (c == '\n') return true;
            out += char(c);
            if (out.size() > 4096) throw std::runtime_error("LPD: слишком длинная команда");
        }
    }
    void exact(std::vector<uint8_t>& out, uint64_t n, const ProgressFn& progress) {
        out.clear();
        out.reserve(size_t(n));
        uint64_t last = 0;
        while (out.size() < n) {
            if (!fill()) throw std::runtime_error("LPD: соединение закрыто на " + std::to_string(out.size()) + " из " + std::to_string(n));
            size_t take = std::min<size_t>(buf.size() - pos, size_t(n - out.size()));
            out.insert(out.end(), buf.begin() + pos, buf.begin() + pos + take);
            pos += take;
            if (progress && (out.size() - last >= (1u << 20) || out.size() == n)) {
                progress(out.size(), n);
                last = out.size();
            }
        }
    }
    uint8_t byte() {
        if (!fill()) throw std::runtime_error("LPD: обрыв");
        return buf[pos++];
    }
};
void ack(net::TcpStream& s) {
    uint8_t z = 0;
    s.write_all(&z, 1);
}
}  // namespace

LpdJob lpd_receive(net::TcpStream& s, int timeout_ms, const ProgressFn& progress) {
    Reader r{s, timeout_ms, {}, 0};
    LpdJob job;
    std::string cmd;
    if (!r.line(cmd) || cmd.empty()) throw std::runtime_error("LPD: пустое соединение");
    if (cmd[0] != 0x02) {
        uint8_t nak = 1;
        s.write_all(&nak, 1);
        throw std::runtime_error("LPD: неподдерживаемая команда " + std::to_string(int((unsigned char)cmd[0])));
    }
    job.queue = cmd.substr(1);
    ack(s);
    std::string sub;
    while (r.line(sub)) {
        if (sub.empty()) break;
        const uint8_t kind = uint8_t(sub[0]);
        if (kind == 0x01) {  // отмена задания
            ack(s);
            continue;
        }
        if (kind != 0x02 && kind != 0x03) throw std::runtime_error("LPD: неизвестная подкоманда " + std::to_string(kind));
        const std::string rest = sub.substr(1);
        const size_t sp = rest.find(' ');
        const uint64_t len = std::stoull(rest.substr(0, sp));
        const std::string name = sp == std::string::npos ? "" : rest.substr(sp + 1);
        ack(s);
        std::vector<uint8_t> body;
        r.exact(body, len, kind == 0x03 ? progress : ProgressFn());
        r.byte();  // завершающий \0
        ack(s);
        if (kind == 0x02)
            job.control.assign(body.begin(), body.end());
        else {
            job.data_name = name;
            job.data = std::move(body);
        }
    }
    return job;
}

// ---------- сканер ----------
ScanResult scan_once(const ScanOptions& opt) {
    auto log = [&](const std::string& m) {
        if (opt.log) opt.log(m);
    };
    net::init();
    const net::Endpoint dev = net::resolve(opt.device, opt.device_port);

    // 1. Сначала занимаем TCP 3892: МФУ подключится сюда с картинкой.
    net::TcpListener listener(opt.lpd_port, opt.lpd_loopback_only);
    log("жду картинку на TCP " + std::to_string(listener.local_port()));

    const auto my_ip = net::local_ip_towards(dev);
    const auto mac = net::mac_for_ip(my_ip);
    std::string name = opt.pc_name.empty() ? net::hostname() : opt.pc_name;
    const auto ann = mfpp_announce_body(my_ip, name, mac);

    uint16_t seq = uint16_t((std::chrono::steady_clock::now().time_since_epoch().count() & 0x7fff) | 0x100);
    auto send = [&](uint8_t cls, uint8_t code, const std::vector<uint8_t>& body, int wait_ms, MfppPacket* reply) {
        net::Udp u;  // как оригинал: новый эфемерный порт на каждую команду
        MfppPacket p{seq, cls, code, body};
        u.send_to(dev, mfpp_encode(p));
        const auto t0 = std::chrono::steady_clock::now();
        while (reply) {
            int left = wait_ms - int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
            if (left <= 0) return false;
            std::vector<uint8_t> d;
            net::Endpoint from;
            if (!u.recv_from(d, from, left)) return false;
            if (from.ip != dev.ip || !mfpp_decode(d, *reply)) continue;
            if (reply->seq == seq) return true;
        }
        return true;
    };

    MfppPacket rep;
    send(0x31, 0x03, ann, 1500, &rep);  // ответ 31 C4 не обязателен
    ++seq;
    send(0x31, 0x05, ann, 0, nullptr);
    ++seq;
    for (int i = 0; i < 3; ++i) {
        send(0x32, 0x10, {}, 0, nullptr);
        ++seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!send(0x32, 0x41, mfpp_scan_params_body(opt.params), opt.reply_timeout_ms, &rep) || rep.code != 0x43 || rep.body.size() < 8)
        throw std::runtime_error(
            "МФУ не ответило на команду скана (32 43). Проверьте, что МФУ включено и не занято, а входящий UDP от МФУ "
            "разрешён в брандмауэре (на Windows: mbtool setup от администратора).");
    const std::vector<uint8_t> id(rep.body.begin(), rep.body.begin() + 8);
    send(0x32, 0x62, id, 0, nullptr);
    ++seq;
    ScanResult res;
    res.job_id = hex(id.data(), 4) + ":" + std::string(id.begin() + 4, id.end());
    log("задание " + res.job_id + " принято, жду картинку");

    // 2. МФУ подключается и отдаёт картинку по LPR; затем короткое соединение-маркер.
    std::vector<uint8_t> data;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opt.data_timeout_s);
    while (data.empty()) {
        int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) throw std::runtime_error("картинка не пришла за " + std::to_string(opt.data_timeout_s) + " с (брандмауэр: входящий TCP 3892?)");
        std::unique_ptr<net::TcpStream> c;
        if (!listener.accept(c, left)) continue;
        log("подключение от " + c->peer().to_string());
        LpdJob job = lpd_receive(*c, 60000, opt.progress);
        if (!job.data.empty()) data = std::move(job.data);
    }
    // принять маркер конца, чтобы МФУ не ждало
    for (;;) {
        std::unique_ptr<net::TcpStream> c;
        if (!listener.accept(c, 3000)) break;
        try {
            lpd_receive(*c, 5000);
        } catch (const std::exception&) {
        }
    }
    for (int i = 0; i < 3; ++i) {
        send(0x32, 0x10, {}, 0, nullptr);
        ++seq;
    }
    res.header = parse_scan_header(data);
    res.image = decode_scan_data(data);
    return res;
}

}  // namespace mb2020
