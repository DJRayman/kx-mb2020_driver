# kx-mb2020_driver

Открытый драйвер для сетевого МФУ **Panasonic KX-MB2020** (серия KX-MB2000): печать и сканирование по LAN без фирменной программы Panasonic Multi-Function Station.

## Статус

| Часть | Состояние |
|---|---|
| Протокол сканирования (LAN) | разобран по захватам трафика, прототип на Windows получает скан A4/300 dpi/цвет и сохраняет PNG |
| Протокол печати (GDI, TCP 9100) | разобран: PJL + ESC-команды + полосы JBIG-1 (T.82); свой поток собирается байт-в-байт как у драйвера Panasonic |
| Отправка своего задания на печать | ещё не проверена на устройстве |
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

## Структура

```
docs/protocol-scan.md      протокол сканирования
docs/protocol-print.md     протокол печати
third_party/jbigkit/       исходники jbig-kit (справка, не собираются)
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

GPL-2.0-or-later (см. [LICENSE](LICENSE)). JBIG-кодек (proto/windows/Jbig1.cs) основан на jbig-kit (Markus Kuhn), распространяемом под GNU GPL; исходники jbig-kit для справки лежат в [third_party/jbigkit](third_party/jbigkit).
