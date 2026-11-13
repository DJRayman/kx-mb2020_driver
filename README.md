# kx-mb2020_driver

Открытый драйвер для сетевого МФУ **Panasonic KX-MB2020** (серия KX-MB2000): печать и сканирование по LAN без фирменной программы Panasonic Multi-Function Station.

## Статус

| Часть | Состояние |
|---|---|
| Протокол сканирования (LAN) | разобран по захватам трафика, прототип на Windows получает скан A4/300 dpi/цвет и сохраняет PNG |
| Протокол печати (GDI, TCP 9100) | разобран: PJL + ESC-команды + полосы JBIG-1 (T.82); свой поток собирается байт-в-байт как у драйвера Panasonic |
| Отправка своего задания на печать | проверена на устройстве: страница, собранная своим кодом, напечаталась |
| Ядро на C++ (`core/`) и утилита `mbtool` | печать картинок и скан в PNG/JPEG проверены на МФУ; печать PDF (через PDFium) — 0.2.0, на МФУ ещё не проверена |
| JBIG-1 кодер/декодер | готов (`proto/windows/Jbig1.cs`, на основе jbig-kit): перекодирование драйверного задания совпадает байт в байт |
| Linux (SANE/CUPS) | не начато |

## Кратко о протоколах

**Сканирование** — подробно в [docs/protocol-scan.md](docs/protocol-scan.md):

1. ПК шлёт UDP-датаграммы на порт **3892** МФУ, каждая начинается с ASCII `MFPP0200ML01`: объявление ПК (`31 03`/`31 05`), опрос (`32 10`), параметры скана (`32 41`).
2. МФУ отвечает `32 43` с ID задания (с UDP-порта 655xx — брандмауэр Windows считает это «не ответом»), ПК подтверждает `32 62`.
3. **МФУ само подключается к ПК на TCP 3892** и передаёт картинку по протоколу **LPR/LPD** (очередь `lp`).
4. Data-файл: заголовок 128 байт + сырой RGB 8 бит построчно (A4 300 dpi = 2480×3504).

**Печать** — подробно в [docs/protocol-print.md](docs/protocol-print.md):

RAW TCP 9100: `UEL` + PJL (`ENTER LANGUAGE=GDI`) → ESC-команды страницы (`ESC&r600W`, `ESC&l26A`, `ESC*r<H>T`, `ESC*r<W>S` …) → полосы `ESC*b0W` по 512 строк, каждая — JBIG-1 BIE (двухстрочный шаблон, TPBON) → `ESC&&1R` + `UEL @PJL EOJ`.

## Утилита mbtool (Windows / Linux)

```
mbtool print -p 192.168.1.50 документ.pdf            # печать PDF (все страницы, A4 в натуральную величину)
mbtool print -p 192.168.1.50 --pages 1-3 --fit документ.pdf   # страницы 1–3, вписать в печатаемую область
mbtool print -p 192.168.1.50 страница.png            # печать картинки на A4 (вписывается в лист)
mbtool print -p 192.168.1.50 --dry-run --preview p.png страница.png   # посмотреть ч/б страницу без печати
mbtool scan  -p 192.168.1.50 -o скан.png              # скан со стекла: A4, 300 dpi, цвет
mbtool decode-prn задание.prn страница.png            # раскодировать поток печати
mbtool setup                                          # Windows, от администратора: правило брандмауэра
```

IP МФУ можно задать переменной `MB2020_PRINTER`. Для скана на Windows порт TCP 3892 должен быть свободен
(остановите службу «Panasonic Local Printer Service»), а mbtool — разрешён в брандмауэре (`mbtool setup`).

Пока поддерживаются: только A4, печать 600 dpi, скан 300 dpi в цвете. Каждая страница PDF уходит отдельным заданием.

Для PDF нужна библиотека PDFium (`pdfium.dll` / `libpdfium.so`) рядом с mbtool. Она подгружается во время работы,
в репозиторий не входит; в сборке для Windows лежит готовая из [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)
вместе с её лицензиями (BSD-3-Clause и сторонние). Тест PDF запускается так: `MB2020_PDFIUM=/путь/libpdfium.so ./build/mb2020_tests`.

### Сборка

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/mb2020_tests
# Windows из Linux (mingw-w64):
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win
```

## Структура

```
core/                      переносимое ядро на C++17 (печать, скан, сеть, растр) — основа для Windows и Android
tools/mbtool/              консольная утилита
tests/                     тесты ядра (JBIG, формат задания, MFPP, LPD, скан с эмулятором МФУ)
cmake/                     файл кросс-сборки под Windows
third_party/stb/           stb_image / stb_image_write (public domain) — чтение и запись картинок
docs/protocol-scan.md      протокол сканирования
docs/protocol-print.md     протокол печати
third_party/jbigkit/       jbig-kit 1.6 (GPL) — JBIG-1 кодек ядра
proto/windows/             прототипы (PowerShell + C#, только .NET Framework, без внешних библиотек)
  MfppScan.cs, mfpp_scan.ps1   скан по сети (UDP MFPP + свой LPD на TCP 3892 + PNG)
  Jbig1.cs                     JBIG-1 (T.82) кодер/декодер, на основе jbig-kit
  GdiPrn.cs                    разбор/сборка потока печати, PNG 1 бит
  prn_test.ps1                 проверка: раскодировать print1.prn, перекодировать, собрать свой print-own.prn
  print_test.ps1               тестовая страница через штатный драйвер Windows
```

## Как запустить прототип сканирования (Windows)

1. Порт 3892 на ПК не должен быть занят программой Panasonic (служба «Panasonic Local Printer Service», процессы PCCMFLPD/PCMFSMLM) — остановите их от администратора.
2. Разрешите входящий TCP 3892 от МФУ:
   `New-NetFirewallRule -DisplayName "KX-MB2020 LPD 3892" -Direction Inbound -Protocol TCP -LocalPort 3892 -RemoteAddress <IP МФУ> -Action Allow`
3. Ответ МФУ по UDP прототип ловит через Npcap (если установлен), т.к. брандмауэр его режет; альтернатива — правило на входящий UDP от IP МФУ.
4. `powershell -ExecutionPolicy Bypass -File proto\windows\mfpp_scan.ps1 -Device <IP МФУ> -MyIp <IP ПК>`

## Правовое

Протоколы восстановлены по сетевому трафику собственного устройства для обеспечения совместимости. Файлы и программы Panasonic в репозиторий не входят.

## Лицензия

GPL-2.0-or-later (см. [LICENSE](LICENSE)). JBIG-кодек (proto/windows/Jbig1.cs) основан на jbig-kit (Markus Kuhn), распространяемом под GNU GPL; ядро на C++ использует сам jbig-kit из [third_party/jbigkit](third_party/jbigkit). stb_image и stb_image_write — public domain.
