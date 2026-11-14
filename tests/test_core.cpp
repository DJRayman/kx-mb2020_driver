// SPDX-License-Identifier: GPL-2.0-or-later
// Тесты ядра без железа: JBIG, формат задания печати, растр, MFPP, LPD и полный скан с эмулятором МФУ.
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#include "mb2020/gdi_print.h"
#include "mb2020/jbig_band.h"
#include "mb2020/net.h"
#include "mb2020/pdf.h"
#include "mb2020/raster.h"
#include "mb2020/scan.h"

using namespace mb2020;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (c) ++g_pass;                                                         \
        else {                                                                   \
            ++g_fail;                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
        }                                                                        \
    } while (0)

static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

// Тестовая страница как у нас на железе: рамка 0,5 мм в 1 см от краёв + «текст» и немного шума.
static Bitmap1 test_page() {
    Bitmap1 p(kA4Width600, kA4Height600);
    const int m = 236, t = 12;
    p.fill_rect(m, m, p.width - m, m + t);
    p.fill_rect(m, p.height - m - t, p.width - m, p.height - m);
    p.fill_rect(m, m, m + t, p.height - m);
    p.fill_rect(p.width - m - t, m, p.width - m, p.height - m);
    std::mt19937 rng(42);
    for (int i = 0; i < 40; ++i) {
        int x = 400 + int(rng() % 3500), y = 400 + int(rng() % 500);
        p.fill_rect(x, y, x + 20 + int(rng() % 60), y + 60);
    }
    return p;
}

static void test_jbig() {
    std::printf("jbig\n");
    Bitmap1 band = test_page().band(0, 512);
    auto bie = jbig_encode_band(band, true);
    CHECK(bie.size() > 22);
    CHECK(bie[0] == 0 && bie[1] == 0 && bie[2] == 1 && bie[3] == 0);  // DL D P -
    CHECK(be32(&bie[4]) == uint32_t(band.width));
    CHECK(be32(&bie[8]) == uint32_t(band.height));
    CHECK(be32(&bie[12]) == uint32_t(band.height));  // L0 = вся полоса
    CHECK(bie[16] == 0 && bie[17] == 0);              // MX MY
    CHECK(bie[18] == 0);                              // order
    CHECK(bie[19] == 0x48);                           // LRLTWO | TPBON
    CHECK(bie[bie.size() - 3] == 0x00 && bie[bie.size() - 2] == 0xff && bie[bie.size() - 1] == 0x02);
    Bitmap1 back = jbig_decode(bie.data(), bie.size());
    CHECK(back.width == band.width && back.height == band.height && back.bits == band.bits);

    auto nopad = jbig_encode_band(band, false);
    CHECK(nopad.size() + 1 == bie.size());

    // пустая полоса с боковыми линиями сжимается до нескольких байт (как у Panasonic: ~13 байт SDE)
    Bitmap1 side(kA4Width600, 512);
    side.fill_rect(236, 0, 248, 512);
    side.fill_rect(kA4Width600 - 248, 0, kA4Width600 - 236, 512);
    auto small = jbig_encode_band(side, true);
    std::printf("  SDE полосы с двумя линиями: %zu байт\n", small.size() - 20);
    CHECK(small.size() - 20 < 40);
}

static void test_gdi_job() {
    std::printf("gdi job\n");
    Bitmap1 page = test_page();
    PrintJobInfo info;
    info.name = "TEST \"1\"";
    info.time = "2026/11/13 12:00:00";
    auto job = build_gdi_job(page, info);
    std::string s(job.begin(), job.end());
    CHECK(s.rfind("\x1b%-12345X@PJL COMMENT Panasonic KX-MB2020 GDI\n", 0) == 0);
    CHECK(s.find("@PJL JOB NAME=\"TEST 1\"\n") != std::string::npos);  // кавычки вырезаны
    CHECK(s.find("@PJL ENTER LANGUAGE=GDI\n\x1b&&0S\x1b&r600W\x1b&r600L\x1b&&0Y\x1b&l26A\x1b&l2H\x1b&&11W\x1b*r6814T\x1b*r4760S"
                 "\x1b&a100L\x1b&l100E\x1b&&0T\x1b*b0W") != std::string::npos);
    const std::string tail = "\x1b&&1R\x1b%-12345X@PJL EOJ NAME=\"TEST 1\"\n\x1b%-12345X";
    CHECK(s.size() > tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0);

    GdiJob parsed = parse_gdi_job(job);
    CHECK(parsed.width == 4760 && parsed.height == 6814);
    CHECK(parsed.bands.size() == 14);
    for (size_t i = 0; i < parsed.bands.size(); ++i) {
        const auto& b = parsed.bands[i];
        uint32_t want = (i == 0 ? 0x10000u : 0u) | (i == 13 ? 1u : 0u);
        CHECK(b.flags == want);
        CHECK(b.lines == (i == 13 ? 158 : 512));
        const uint8_t* h = &job[b.offset + 13];
        CHECK(h[0] == 0x0b && h[1] == 0 && le32(&job[b.offset + 17]) == 0);
    }
    Bitmap1 back = decode_gdi_page(parsed);
    CHECK(back.bits == page.bits);
    std::printf("  задание %zu байт\n", job.size());

    bool threw = false;
    try {
        build_gdi_job(Bitmap1(100, 100), info);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

static void test_raster() {
    std::printf("raster\n");
    // белый лист 1000x1414 с чёрным квадратом в центре -> порог, вписывание в A4
    Image8 img(1000, 1414, 1);
    for (int y = 657; y < 757; ++y)
        for (int x = 450; x < 550; ++x) img.data[size_t(y) * 1000 + x] = 0;
    Bitmap1 p = render_page(img, RasterOptions{});
    CHECK(p.width == kA4Width600 && p.height == kA4Height600);
    CHECK(p.get(kA4Width600 / 2, kA4Height600 / 2));
    CHECK(!p.get(100, 100));
    // серая заливка 50% -> растр примерно наполовину чёрный
    Image8 grey(200, 283, 1);
    std::fill(grey.data.begin(), grey.data.end(), 128);
    RasterOptions fs;
    fs.dither = Dither::FloydSteinberg;
    Bitmap1 q = render_page(grey, fs);
    size_t black = 0;
    for (int y = 3000; y < 3100; ++y)
        for (int x = 2000; x < 2100; ++x) black += q.get(x, y);
    std::printf("  чёрных в сером квадрате: %zu из 10000\n", black);
    CHECK(black > 4000 && black < 6000);
    // RGB -> серый
    Image8 rgb(1, 1, 3);
    rgb.data = {255, 0, 0};
    CHECK(to_gray(rgb).data[0] == 76);
}

static void test_mfpp() {
    std::printf("mfpp\n");
    MfppPacket p{0x7db5, 0x32, 0x41, mfpp_scan_params_body(ScanParams{})};
    auto d = mfpp_encode(p);
    CHECK(d.size() == 148);
    CHECK(std::memcmp(d.data(), "MFPP0200ML01", 12) == 0);
    CHECK(d[12] == 0xb5 && d[13] == 0x7d && d[14] == 0x32 && d[15] == 0x41 && le32(&d[16]) == 0x80);
    CHECK(le32(&d[20]) == 3 && le32(&d[24]) == 3 && le32(&d[28]) == 3);
    CHECK(le32(&d[20 + 0x0C]) == 300 && le32(&d[20 + 0x10]) == 300);
    CHECK(le32(&d[20 + 0x1C]) == 2480 && le32(&d[20 + 0x20]) == 3508);
    MfppPacket q;
    CHECK(mfpp_decode(d, q) && q.seq == 0x7db5 && q.code == 0x41 && q.body.size() == 0x80);
    d[0] = 'X';
    CHECK(!mfpp_decode(d, q));

    auto a = mfpp_announce_body({192, 168, 1, 10}, "mypc", {0, 0x11, 0x22, 0x33, 0x44, 0x55});
    CHECK(a.size() == 28);
    CHECK(a[0] == 192 && a[3] == 10);
    CHECK(std::memcmp(&a[4], "MYPC\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);
    CHECK(a[20] == 0 && a[25] == 0x55 && a[26] == 0 && a[27] == 0x0f);
}

// Данные скана: заголовок 128 байт + RGB.
static std::vector<uint8_t> fake_scan_data(int w, int h) {
    std::vector<uint8_t> d(128 + size_t(w) * h * 3, 0);
    auto put = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) d[at + i] = uint8_t(v >> (8 * i));
    };
    put(0, 3), put(4, 3), put(8, 3), put(0x0C, 300), put(0x10, 300), put(0x1C, uint32_t(w)), put(0x20, uint32_t(h + 4));
    for (size_t i = 128; i < d.size(); ++i) d[i] = uint8_t(i * 7);
    return d;
}

static void lpr_send(uint16_t port, const std::vector<uint8_t>& data, int n) {
    net::Endpoint ep = net::resolve("127.0.0.1", port);
    net::TcpStream s = net::TcpStream::connect(ep, 2000);
    auto expect0 = [&] {
        uint8_t b = 9;
        if (s.read_some(&b, 1, 2000) != 1 || b != 0) throw std::runtime_error("lpr: нет ack");
    };
    auto send_str = [&](const std::string& x) { s.write_all((const uint8_t*)x.data(), x.size()); };
    send_str("\x02lp\n");
    expect0();
    std::string host = "MB2020RU_xxxxxx";
    char num[8];
    std::snprintf(num, sizeof num, "%03d", n);
    std::string ctrl = "H" + host + "\nN013_" + num + ".DAT\n";
    send_str("\x02" + std::to_string(ctrl.size()) + " cfA" + num + host + "\n");
    expect0();
    send_str(ctrl);
    send_str(std::string(1, '\0'));
    expect0();
    if (!data.empty()) {
        send_str("\x03" + std::to_string(data.size()) + " dfA" + num + host + "\n");
        expect0();
        s.write_all(data);
        send_str(std::string(1, '\0'));
        expect0();
    }
    s.shutdown_send();
}

static void test_lpd() {
    std::printf("lpd\n");
    net::init();
    net::TcpListener l(0, true);
    auto data = fake_scan_data(8, 4);
    std::thread t([&] { lpr_send(l.local_port(), data, 1); });
    std::unique_ptr<net::TcpStream> c;
    CHECK(l.accept(c, 3000));
    uint64_t last = 0;
    LpdJob j = lpd_receive(*c, 3000, [&](uint64_t done, uint64_t) { last = done; });
    t.join();
    CHECK(j.queue == "lp");
    CHECK(j.control.find("N013_001.DAT") != std::string::npos);
    CHECK(j.data == data);
    CHECK(last == data.size());
    Image8 img = decode_scan_data(j.data);
    CHECK(img.width == 8 && img.height == 4 && img.channels == 3);
    CHECK(img.data[0] == uint8_t(128 * 7));
}

// Эмулятор МФУ: отвечает на 32 41 и отдаёт картинку на LPD-порт ПК.
static void test_scan_end_to_end() {
    std::printf("scan (эмулятор МФУ)\n");
    net::init();
    net::Udp mfp(0);
    const uint16_t mfp_port = mfp.local_port();
    uint16_t lpd_port;
    {
        net::TcpListener probe(0, true);
        lpd_port = probe.local_port();
    }
    auto data = fake_scan_data(64, 32);
    std::atomic<int> got_ann{0}, got_ack{0};
    std::thread fake([&] {
        for (;;) {
            std::vector<uint8_t> d;
            net::Endpoint from;
            if (!mfp.recv_from(d, from, 5000)) return;
            MfppPacket p;
            if (!mfpp_decode(d, p)) continue;
            if (p.cls == 0x31 && p.code == 0x03) {
                ++got_ann;
                p.code = 0xC4;
                net::Udp r(0);  // ответ с другого порта, как настоящее МФУ (655xx)
                r.send_to(from, mfpp_encode(p));
            } else if (p.cls == 0x32 && p.code == 0x41) {
                MfppPacket rep{p.seq, 0x32, 0x43, {0xf0, 0x18, 0x53, 0x04, '0', '0', '0', '7'}};
                net::Udp r(0);
                r.send_to(from, mfpp_encode(rep));
            } else if (p.cls == 0x32 && p.code == 0x62) {
                ++got_ack;
                lpr_send(lpd_port, data, 7);
                lpr_send(lpd_port, {}, 8);  // маркер конца
                return;
            }
        }
    });
    ScanOptions so;
    so.device = "127.0.0.1";
    so.device_port = mfp_port;
    so.lpd_port = lpd_port;
    so.lpd_loopback_only = true;
    so.pc_name = "TESTPC";
    so.data_timeout_s = 10;
    ScanResult r;
    bool ok = true;
    try {
        r = scan_once(so);
    } catch (const std::exception& e) {
        std::printf("  исключение: %s\n", e.what());
        ok = false;
    }
    fake.join();
    CHECK(ok);
    CHECK(got_ann == 1 && got_ack == 1);
    CHECK(r.job_id == "f0185304:0007");
    CHECK(r.image.width == 64 && r.image.height == 32 && r.image.channels == 3);
    CHECK(r.header.requested_height == 36);
    CHECK(std::memcmp(r.image.data.data(), data.data() + 128, r.image.data.size()) == 0);
}

// Минимальный PDF: страница 1 — A4 с рамкой в 1 см и надписью; страница 2 — A4 альбомная с чёрным квадратом.
static std::vector<uint8_t> make_test_pdf() {
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.276 841.89] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "",  // поток 1
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 841.89 595.276] /Contents 7 0 R >>",
        "",  // поток 2
    };
    auto stream = [](const std::string& c) { return "<< /Length " + std::to_string(c.size()) + " >>\nstream\n" + c + "\nendstream"; };
    objs[3] = stream("2 w 28.35 28.35 538.58 785.2 re S BT /F1 36 Tf 72 740 Td (PDF TEST) Tj ET");
    objs[6] = stream("0 g 100 100 200 200 re f");
    std::string out = "%PDF-1.4\n";
    std::vector<size_t> off;
    for (size_t i = 0; i < objs.size(); ++i) {
        off.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    char line[32];
    for (size_t o : off) {
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return std::vector<uint8_t>(out.begin(), out.end());
}

// Нужна библиотека PDFium: путь в переменной MB2020_PDFIUM (иначе тест пропускается).
static void test_pdf() {
    const char* lib = std::getenv("MB2020_PDFIUM");
    if (!lib) {
        std::printf("pdf: пропущено (MB2020_PDFIUM не задан)\n");
        return;
    }
    std::printf("pdf\n");
    PdfDocument::load_library(lib);
    PdfDocument doc(make_test_pdf());
    CHECK(doc.page_count() == 2);
    double w, h;
    doc.page_size_pt(0, w, h);
    CHECK(std::abs(w - 595.276) < 0.01 && std::abs(h - 841.89) < 0.01);

    // A4 в натуральную величину: рамка в 1 см (236 пикс) от края листа = ~135 пикс от края печатаемой области,
    // минус половина толщины линии 2 pt (~8 пикс) -> внешний край ~127 пикс
    Image8 g = doc.render_a4_600(0, PdfRenderOptions{});
    CHECK(g.width == kA4Width600 && g.height == kA4Height600 && g.channels == 1);
    auto px = [&](int x, int y) { return g.data[size_t(y) * g.width + x]; };
    int left = -1;
    for (int x = 0; x < 400 && left < 0; ++x)
        if (px(x, 3400) < 128) left = x;
    int top = -1;
    for (int y = 0; y < 400 && top < 0; ++y)
        if (px(2380, y) < 128) top = y;
    std::printf("  рамка: слева %d, сверху %d пикс от края печатаемой области\n", left, top);
    CHECK(left >= 122 && left <= 134);
    CHECK(top >= 122 && top <= 134);
    size_t black = 0;  // текст «PDF TEST»
    for (int y = 0; y < 500; ++y)
        for (int x = 400; x < 2000; ++x) black += px(x, 300 + y) < 128;
    CHECK(black > 5000);
    Bitmap1 page = render_page(g, RasterOptions{});
    CHECK(page.get(left + 2, 3400));

    // Альбомная страница поворачивается; чёрный квадрат 200x200 pt -> ~1667x1667 пикс
    Image8 g2 = doc.render_a4_600(1, PdfRenderOptions{});
    size_t b2 = 0;
    for (uint8_t v : g2.data) b2 += v < 128;
    std::printf("  чёрных на альбомной странице: %zu\n", b2);
    CHECK(b2 > 1600u * 1600u && b2 < 1700u * 1700u);
    PdfRenderOptions fit;
    fit.scale = PdfScale::Fit;
    Image8 g3 = doc.render_a4_600(0, fit);
    int left3 = -1;
    for (int x = 0; x < 400 && left3 < 0; ++x)
        if (g3.data[size_t(3400) * g3.width + x] < 128) left3 = x;
    // вписанная страница (масштаб ~0,96) начинается с края области, а не с -100 пикс: рамка ~219 пикс
    std::printf("  рамка при --fit: слева %d\n", left3);
    CHECK(left3 >= 210 && left3 <= 228);
}

int main() {
    try {
        test_jbig();
        test_gdi_job();
        test_raster();
        test_mfpp();
        test_lpd();
        test_scan_end_to_end();
        test_pdf();
    } catch (const std::exception& e) {
        std::printf("НЕОЖИДАННОЕ ИСКЛЮЧЕНИЕ: %s\n", e.what());
        return 2;
    }
    std::printf("\n%d проверок прошло, %d упало\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
