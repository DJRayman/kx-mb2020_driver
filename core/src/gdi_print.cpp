// SPDX-License-Identifier: GPL-2.0-or-later
#include "mb2020/gdi_print.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "mb2020/jbig_band.h"

namespace mb2020 {

namespace {
void put(std::vector<uint8_t>& o, const std::string& s) { o.insert(o.end(), s.begin(), s.end()); }
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void put_u16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(uint8_t(v));
    o.push_back(uint8_t(v >> 8));
}
uint32_t get_u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

// PJL-значения в кавычках: убрать кавычки и управляющие символы.
std::string pjl_safe(const std::string& s) {
    std::string r;
    for (unsigned char c : s)
        if (c >= 0x20 && c != '"') r += char(c);
    return r.empty() ? std::string("-") : r;
}
const char ESC = '\x1b';
const std::string UEL = std::string(1, ESC) + "%-12345X";
}  // namespace

std::string current_pjl_time() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y/%m/%d %H:%M:%S", &tm);
    return buf;
}

std::vector<uint8_t> build_gdi_job(const Bitmap1& page, const PrintJobInfo& info) {
    if (page.width != kA4Width600 || page.height != kA4Height600)
        throw std::invalid_argument("build_gdi_job: поддерживается только A4 600 dpi (4760x6814)");
    std::vector<uint8_t> o;
    const std::string E(1, ESC);
    const std::string name = pjl_safe(info.name);
    put(o, UEL);
    // Заголовок повторяет драйвер Panasonic 1.048: какие строки обязательны для МФУ, не проверено.
    put(o,
        "@PJL COMMENT Panasonic KX-MB2020 GDI\n"
        "@PJL COMMENT TREATASCHARACTER\n"
        "@PJL COMMENT version AMD64 5.01.032.001\n"
        "@PJL COMMENT DriverVersion=\"1.048(1.048.002.e)\"\n"
        "@PJL COMMENT DriverOrgVer=\"0.048.000.e\"\n"
        "@PJL COMMENT OSVersion=\"Windows 10.0;Build 26300;AMD64\"\n"
        "@PJL COMMENT Monochrome MFP\n"
        "@PJL COMMENT Soc Type=Sirius\n");
    put(o, "@PJL JOB TIME=\"" + (info.time.empty() ? current_pjl_time() : pjl_safe(info.time)) + "\"\n");
    put(o, "@PJL JOB NAME=\"" + name + "\"\n");
    put(o, "@PJL JOB OWNER=\"" + pjl_safe(info.owner) + "\"\n");
    put(o, "@PJL JOB HOST=\"" + pjl_safe(info.host) + "\"\n");
    put(o, "@PJL JOB PCIP=\"" + pjl_safe(info.pc_ip) + "\"\n");
    put(o, "@PJL SET RET=ON\n");
    put(o, std::string("@PJL SET ECONOMODE=") + (info.economode ? "ON" : "OFF") + "\n");
    put(o, "@PJL SET COPIES=" + std::to_string(info.copies < 1 ? 1 : info.copies) + "\n");
    put(o, "@PJL SET DUPLEX=OFF\n@PJL ENTER LANGUAGE=GDI\n");

    put(o, E + "&&0S" + E + "&r600W" + E + "&r600L" + E + "&&0Y" + E + "&l26A" + E + "&l2H" + E + "&&11W");
    put(o, E + "*r" + std::to_string(page.height) + "T" + E + "*r" + std::to_string(page.width) + "S");
    put(o, E + "&a100L" + E + "&l100E" + E + "&&0T");

    const int nb = (page.height + kBandLines - 1) / kBandLines;
    for (int bi = 0; bi < nb; ++bi) {
        const int y0 = bi * kBandLines;
        const int lines = std::min(kBandLines, page.height - y0);
        std::vector<uint8_t> bie = jbig_encode_band(page.band(y0, lines), true);
        put(o, E + "*b0W");
        put_u32(o, uint32_t(bie.size()));
        put_u32(o, (bi == 0 ? 0x10000u : 0u) | (bi == nb - 1 ? 1u : 0u));
        o.push_back(0x0b);
        o.push_back(0x00);
        put_u16(o, uint16_t(lines));
        put_u32(o, 0);
        o.insert(o.end(), bie.begin(), bie.end());
    }
    put(o, E + "&&1R" + UEL + "@PJL EOJ NAME=\"" + name + "\"\n" + UEL);
    return o;
}

GdiJob parse_gdi_job(const std::vector<uint8_t>& d) {
    GdiJob job;
    for (size_t i = 0; i + 5 <= d.size(); ++i) {
        if (d[i] != 0x1b) continue;
        if (i + 3 < d.size() && d[i + 1] == '*' && d[i + 2] == 'r') {
            // ESC*r<число>T / ESC*r<число>S
            size_t j = i + 3;
            long v = 0;
            bool any = false;
            while (j < d.size() && d[j] >= '0' && d[j] <= '9') v = v * 10 + (d[j++] - '0'), any = true;
            if (any && j < d.size()) {
                if (d[j] == 'T') job.height = int(v);
                if (d[j] == 'S') job.width = int(v);
            }
            continue;
        }
        if (std::memcmp(&d[i], "\x1b*b0W", 5) == 0) {
            if (i + 21 > d.size()) throw std::runtime_error("GDI: обрезанный заголовок полосы");
            GdiBand b;
            b.offset = i;
            uint32_t len = get_u32(&d[i + 5]);
            b.flags = get_u32(&d[i + 9]);
            b.lines = d[i + 15] | (d[i + 16] << 8);
            if (i + 21 + size_t(len) > d.size()) throw std::runtime_error("GDI: обрезанная полоса");
            b.bie.assign(d.begin() + i + 21, d.begin() + i + 21 + len);
            job.bands.push_back(std::move(b));
            i += 21 + len - 1;
        }
    }
    return job;
}

Bitmap1 decode_gdi_page(const GdiJob& job) {
    std::vector<Bitmap1> parts;
    int w = 0, h = 0;
    for (const auto& b : job.bands) {
        parts.push_back(jbig_decode(b.bie.data(), b.bie.size()));
        w = parts.back().width;
        h += parts.back().height;
    }
    Bitmap1 page(w, h);
    int y = 0;
    for (const auto& p : parts) {
        std::memcpy(page.row(y), p.bits.data(), p.bits.size());
        y += p.height;
    }
    return page;
}

}  // namespace mb2020
