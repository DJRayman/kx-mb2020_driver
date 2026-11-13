// SPDX-License-Identifier: GPL-2.0-or-later
// mbtool — консольная утилита печати и сканирования для Panasonic KX-MB2020 по сети.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "mb2020/gdi_print.h"
#include "mb2020/net.h"
#include "mb2020/printer.h"
#include "mb2020/raster.h"
#include "mb2020/scan.h"
#include "mb2020/version.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace mb2020;

namespace {

void usage() {
    std::printf(
        "mbtool %s — печать и сканирование Panasonic KX-MB2020 по сети\n"
        "\n"
        "  mbtool print [-p IP] [опции] ФАЙЛ...     напечатать картинки (PNG/JPEG/BMP/GIF/PNM), по странице на файл\n"
        "      --dither auto|fs|threshold          способ перевода в ч/б (auto: текст — порог, фото — растр)\n"
        "      --actual-size DPI                   не масштабировать: картинка с разрешением DPI\n"
        "      --copies N                          копий (через PJL; проверено только 1)\n"
        "      --economode                         экономия тонера (не проверено)\n"
        "      --name ИМЯ                          имя задания на МФУ\n"
        "      --save-prn ФАЙЛ                     сохранить поток задания\n"
        "      --preview ФАЙЛ.png                  сохранить ч/б страницу, как она уйдёт на принтер\n"
        "      --dry-run                           ничего не отправлять на МФУ\n"
        "  mbtool scan [-p IP] [-o ФАЙЛ.png|.jpg]   отсканировать лист со стекла (A4, 300 dpi, цвет)\n"
        "      --timeout СЕК                       ожидание картинки (по умолчанию 180)\n"
        "      --pc-name ИМЯ                       имя ПК, которое увидит МФУ\n"
        "  mbtool decode-prn ЗАДАНИЕ.prn ФАЙЛ.png  раскодировать поток печати в картинку\n"
        "  mbtool setup                            (Windows, от администратора) разрешить mbtool в брандмауэре\n"
        "\n"
        "IP МФУ берётся из -p или переменной окружения MB2020_PRINTER.\n"
        "Для скана порт TCP 3892 на ПК должен быть свободен: остановите службу «Panasonic Local Printer Service».\n",
        kVersion);
}

std::vector<uint8_t> read_file(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    FILE* f = _wfopen(w.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) throw std::runtime_error("не удалось открыть " + path);
    std::vector<uint8_t> d;
    uint8_t buf[1 << 16];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + r);
    std::fclose(f);
    return d;
}

void write_file(const std::string& path, const std::vector<uint8_t>& d) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    FILE* f = _wfopen(w.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) throw std::runtime_error("не удалось создать " + path);
    if (!d.empty() && std::fwrite(d.data(), 1, d.size(), f) != d.size()) {
        std::fclose(f);
        throw std::runtime_error("ошибка записи " + path);
    }
    std::fclose(f);
}

std::string basename_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string printer_from(const std::string& opt) {
    if (!opt.empty()) return opt;
    if (const char* e = std::getenv("MB2020_PRINTER")) return e;
    throw std::runtime_error("не задан IP МФУ: укажите -p 192.168.x.x или переменную MB2020_PRINTER");
}

std::string user_name() {
    for (const char* v : {"USERNAME", "USER", "LOGNAME"})
        if (const char* e = std::getenv(v)) return e;
    return "user";
}

std::string ip_str(const std::array<uint8_t, 4>& ip) {
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);
}

struct Args {
    std::vector<std::string> v;
    size_t i = 0;
    bool more() const { return i < v.size(); }
    const std::string& next(const std::string& opt) {
        if (i >= v.size()) throw std::runtime_error("после " + opt + " нужно значение");
        return v[i++];
    }
};

int cmd_print(Args a) {
    std::string printer, save_prn, preview, name;
    RasterOptions ro;
    PrintJobInfo info;
    bool dry = false;
    std::vector<std::string> files;
    while (a.more()) {
        std::string o = a.v[a.i++];
        if (o == "-p" || o == "--printer") printer = a.next(o);
        else if (o == "--dither") {
            std::string d = a.next(o);
            ro.dither = d == "fs" ? Dither::FloydSteinberg : d == "threshold" ? Dither::Threshold : Dither::Auto;
        } else if (o == "--actual-size") {
            ro.fit = false;
            ro.src_dpi = std::atof(a.next(o).c_str());
        } else if (o == "--copies") info.copies = std::atoi(a.next(o).c_str());
        else if (o == "--economode") info.economode = true;
        else if (o == "--name") name = a.next(o);
        else if (o == "--save-prn") save_prn = a.next(o);
        else if (o == "--preview") preview = a.next(o);
        else if (o == "--dry-run") dry = true;
        else if (!o.empty() && o[0] == '-') throw std::runtime_error("неизвестная опция " + o);
        else files.push_back(o);
    }
    if (files.empty()) throw std::runtime_error("не указаны файлы для печати");
    if (!dry) printer = printer_from(printer);

    info.owner = user_name();
    info.host = net::hostname();
    if (!dry) {
        net::init();
        info.pc_ip = ip_str(net::local_ip_towards(net::resolve(printer, kRawPrintPort)));
    }
    for (size_t k = 0; k < files.size(); ++k) {
        const std::string& f = files[k];
        Image8 img = load_image(f, 1);
        Bitmap1 page = render_page(img, ro);
        info.name = name.empty() ? basename_of(f) : name;
        std::vector<uint8_t> job = build_gdi_job(page, info);
        std::string suffix = files.size() > 1 ? "_" + std::to_string(k + 1) : "";
        auto with_suffix = [&](const std::string& p) {
            if (suffix.empty()) return p;
            size_t dot = p.find_last_of('.');
            return dot == std::string::npos ? p + suffix : p.substr(0, dot) + suffix + p.substr(dot);
        };
        if (!preview.empty()) save_bitmap1_png(with_suffix(preview), page);
        if (!save_prn.empty()) write_file(with_suffix(save_prn), job);
        if (dry) {
            std::printf("%s: задание %zu байт (не отправлено)\n", f.c_str(), job.size());
            continue;
        }
        std::printf("%s: отправляю на %s (%zu байт)…\n", f.c_str(), printer.c_str(), job.size());
        send_raw(printer, job);
        std::printf("  отправлено\n");
    }
    return 0;
}

int cmd_scan(Args a) {
    std::string printer, out;
    ScanOptions so;
    while (a.more()) {
        std::string o = a.v[a.i++];
        if (o == "-p" || o == "--printer") printer = a.next(o);
        else if (o == "-o" || o == "--out") out = a.next(o);
        else if (o == "--timeout") so.data_timeout_s = std::atoi(a.next(o).c_str());
        else if (o == "--pc-name") so.pc_name = a.next(o);
        else throw std::runtime_error("неизвестная опция " + o);
    }
    so.device = printer_from(printer);
    if (out.empty()) {
        char buf[64];
        std::time_t t = std::time(nullptr);
        std::strftime(buf, sizeof buf, "scan_%Y%m%d_%H%M%S.png", std::localtime(&t));
        out = buf;
    }
    so.log = [](const std::string& m) { std::printf("%s\n", m.c_str()); };
    so.progress = [](uint64_t done, uint64_t total) {
        std::printf("\r  получено %llu из %llu КБ", (unsigned long long)(done / 1024), (unsigned long long)(total / 1024));
        if (done == total) std::printf("\n");
        std::fflush(stdout);
    };
    std::printf("сканирую на %s…\n", so.device.c_str());
    ScanResult r = scan_once(so);
    save_image(out, r.image);
    std::printf("готово: %s (%dx%d, %u dpi)\n", out.c_str(), r.image.width, r.image.height, r.header.dpi_x);
    return 0;
}

int cmd_decode(Args a) {
    if (a.v.size() != 2) throw std::runtime_error("нужно: mbtool decode-prn ЗАДАНИЕ.prn ФАЙЛ.png");
    GdiJob job = parse_gdi_job(read_file(a.v[0]));
    if (job.bands.empty()) throw std::runtime_error("в файле нет полос растра (ESC*b0W)");
    Bitmap1 page = decode_gdi_page(job);
    save_bitmap1_png(a.v[1], page);
    std::printf("%zu полос, страница %dx%d -> %s\n", job.bands.size(), page.width, page.height, a.v[1].c_str());
    return 0;
}

int cmd_setup() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring del = L"netsh advfirewall firewall delete rule name=\"KX-MB2020 mbtool\" >nul 2>&1";
    std::wstring add = std::wstring(L"netsh advfirewall firewall add rule name=\"KX-MB2020 mbtool\" dir=in action=allow enable=yes profile=any program=\"") +
                       exe + L"\"";
    _wsystem(del.c_str());
    int r = _wsystem(add.c_str());
    if (r != 0) {
        std::printf("не удалось добавить правило брандмауэра: запустите «mbtool setup» от имени администратора\n");
        return 1;
    }
    std::printf("правило брандмауэра «KX-MB2020 mbtool» добавлено (входящие для mbtool.exe)\n");
    std::printf("если установлена программа Panasonic, для скана остановите её службу:\n"
                "  Stop-Service \"Panasonic Local Printer Service\"\n");
    return 0;
#else
    std::printf("на этой системе настройка не нужна; убедитесь, что входящий TCP 3892 и UDP от МФУ не блокируются\n");
    return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wn = 0;
    LPWSTR* wv = CommandLineToArgvW(GetCommandLineW(), &wn);
    for (int i = 1; i < wn; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, &s[0], n, nullptr, nullptr);
        a.v.push_back(s);
    }
    LocalFree(wv);
    (void)argc;
    (void)argv;
#else
    for (int i = 1; i < argc; ++i) a.v.push_back(argv[i]);
#endif
    if (a.v.empty() || a.v[0] == "-h" || a.v[0] == "--help" || a.v[0] == "help") {
        usage();
        return a.v.empty() ? 2 : 0;
    }
    std::string cmd = a.v[0];
    a.i = 1;
    try {
        if (cmd == "print") return cmd_print(a);
        if (cmd == "scan") return cmd_scan(a);
        if (cmd == "decode-prn") {
            Args b;
            b.v.assign(a.v.begin() + 1, a.v.end());
            return cmd_decode(b);
        }
        if (cmd == "setup") return cmd_setup();
        if (cmd == "version" || cmd == "--version") {
            std::printf("mbtool %s\n", kVersion);
            return 0;
        }
        std::fprintf(stderr, "неизвестная команда: %s\n\n", cmd.c_str());
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ошибка: %s\n", e.what());
        return 1;
    }
}
