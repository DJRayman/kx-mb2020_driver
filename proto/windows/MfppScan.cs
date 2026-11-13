// SPDX-License-Identifier: GPL-2.0-or-later
// Прототип сетевого скана Panasonic KX-MB2020 без программы Panasonic.
// Протокол восстановлен по захватам (см. docs/protocol-scan.md):
//   UDP ПК -> МФУ:3892, пакеты "MFPP0200ML01" + seq(u16 LE) + класс + код + len(u32 LE) + тело
//   МФУ само подключается к ПК на TCP 3892 и отдаёт картинку по LPR (очередь "lp").
//   Data-файл: заголовок 128 байт + сырой RGB 8 бит, построчно.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

public static class MfppScan
{
    public const int Port = 3892;
    static readonly byte[] Magic = Encoding.ASCII.GetBytes("MFPP0200ML01");
    public static Action<string> Log = s => Console.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff ") + s);

    // ---------- UDP MFPP ----------
    public static byte[] Pkt(ushort seq, byte cls, byte code, byte[] body)
    {
        var b = new byte[20 + body.Length];
        Array.Copy(Magic, b, 12);
        b[12] = (byte)seq; b[13] = (byte)(seq >> 8); b[14] = cls; b[15] = code;
        BitConverter.GetBytes((uint)body.Length).CopyTo(b, 16);
        body.CopyTo(b, 20);
        return b;
    }

    public static byte[] AnnounceBody(IPAddress myIp, string host, byte[] mac)
    {
        // 28 байт: IP(4) + имя ПК (16, ASCII, \0-добивка) + MAC(6) + 00 0f
        var b = new byte[28];
        myIp.GetAddressBytes().CopyTo(b, 0);
        var h = Encoding.ASCII.GetBytes(host.ToUpperInvariant());
        Array.Copy(h, 0, b, 4, Math.Min(h.Length, 16));
        mac.CopyTo(b, 20);
        b[26] = 0x00; b[27] = 0x0f;
        return b;
    }

    public static byte[] ScanParamsBody(int mode, int dpiX, int dpiY, int w, int h)
    {
        // 0x80 байт, как в захвате: 3,3,3, 300,300, 0,0, 2480,3508, остальное 0
        var b = new byte[0x80];
        BitConverter.GetBytes(mode).CopyTo(b, 0x00);
        BitConverter.GetBytes(mode).CopyTo(b, 0x04);
        BitConverter.GetBytes(mode).CopyTo(b, 0x08);
        BitConverter.GetBytes(dpiX).CopyTo(b, 0x0C);
        BitConverter.GetBytes(dpiY).CopyTo(b, 0x10);
        BitConverter.GetBytes(w).CopyTo(b, 0x1C);
        BitConverter.GetBytes(h).CopyTo(b, 0x20);
        return b;
    }

    static string Hex(byte[] b, int n = -1)
    {
        if (n < 0 || n > b.Length) n = b.Length;
        var sb = new StringBuilder();
        for (int i = 0; i < n; i++) sb.Append(b[i].ToString("x2")).Append(i % 16 == 15 ? " | " : " ");
        return sb.ToString().TrimEnd();
    }

    // Отправить пакет с нового эфемерного UDP-порта (как оригинал) и подождать ответ на тот же порт.
    public static byte[] SendUdp(IPEndPoint dev, byte[] pkt, int waitMs)
    {
        using (var u = new UdpClient(0))
        {
            u.Client.ReceiveTimeout = waitMs;
            u.Send(pkt, pkt.Length, dev);
            Log(string.Format("UDP -> {0} [{1}] {2}", dev, pkt.Length, Hex(pkt, Math.Min(pkt.Length, 48))));
            if (waitMs <= 0) return null;
            try
            {
                var from = new IPEndPoint(IPAddress.Any, 0);
                var r = u.Receive(ref from);
                Log(string.Format("UDP <- {0} [{1}] {2}", from, r.Length, Hex(r)));
                return r;
            }
            catch (SocketException) { Log("UDP: ответа нет"); return null; }
        }
    }

    // ---------- Npcap-сниффер ответов (если брандмауэр режет входящий UDP с 655xx) ----------
    [System.Runtime.InteropServices.DllImport(@"C:\Windows\System32\Npcap\wpcap.dll", CharSet = System.Runtime.InteropServices.CharSet.Ansi)]
    static extern IntPtr pcap_open_live(string dev, int snaplen, int promisc, int to_ms, StringBuilder err);
    [System.Runtime.InteropServices.DllImport(@"C:\Windows\System32\Npcap\wpcap.dll")]
    static extern int pcap_next_ex(IntPtr p, out IntPtr hdr, out IntPtr data);
    [System.Runtime.InteropServices.DllImport(@"C:\Windows\System32\Npcap\wpcap.dll")]
    static extern void pcap_close(IntPtr p);

    public static string NpcapDevice = null;   // \Device\NPF_{GUID}; null = не использовать
    static IntPtr sniff = IntPtr.Zero;

    public static void SniffOpen()
    {
        if (NpcapDevice == null) return;
        var err = new StringBuilder(512);
        sniff = pcap_open_live(NpcapDevice, 2048, 0, 50, err);
        Log(sniff == IntPtr.Zero ? "Npcap: не открыт: " + err : "Npcap: слушаю ответы через " + NpcapDevice);
    }
    public static void SniffClose() { if (sniff != IntPtr.Zero) { pcap_close(sniff); sniff = IntPtr.Zero; } }

    // ждём UDP от dev с MFPP и заданным seq/кодом
    public static byte[] SniffReply(IPAddress dev, ushort seq, byte code, int waitMs)
    {
        if (sniff == IntPtr.Zero) return null;
        var devb = dev.GetAddressBytes();
        var t0 = DateTime.Now;
        while ((DateTime.Now - t0).TotalMilliseconds < waitMs)
        {
            IntPtr h, d;
            int r = pcap_next_ex(sniff, out h, out d);
            if (r != 1) continue;
            int cap = System.Runtime.InteropServices.Marshal.ReadInt32(h, 8);
            var b = new byte[cap];
            System.Runtime.InteropServices.Marshal.Copy(d, b, 0, cap);
            if (cap < 14 + 20 + 8 + 16 || b[12] != 8 || b[13] != 0) continue;
            int ip = 14, ihl = (b[ip] & 15) * 4;
            if (b[ip + 9] != 17) continue;
            if (b[ip + 12] != devb[0] || b[ip + 13] != devb[1] || b[ip + 14] != devb[2] || b[ip + 15] != devb[3]) continue;
            int tot = (b[ip + 2] << 8) | b[ip + 3];
            int pl = ip + ihl + 8, len = ip + tot - pl;
            if (len < 16) continue;
            bool ok = true; for (int i = 0; i < 12; i++) if (b[pl + i] != Magic[i]) { ok = false; break; }
            if (!ok) continue;
            ushort s = (ushort)(b[pl + 12] | (b[pl + 13] << 8));
            if (s != seq || b[pl + 15] != code) continue;
            var p = new byte[len]; Array.Copy(b, pl, p, 0, len);
            Log(string.Format("Npcap <- {0} [{1}] {2}", dev, len, Hex(p)));
            return p;
        }
        return null;
    }

    // ---------- LPD-сервер ----------
    public class Job { public string Control; public string DataName; public byte[] Data; }

    static string ReadLine(Stream s)
    {
        var ms = new MemoryStream();
        while (true)
        {
            int c = s.ReadByte();
            if (c < 0) return ms.Length == 0 ? null : Encoding.ASCII.GetString(ms.ToArray());
            if (c == '\n') return Encoding.ASCII.GetString(ms.ToArray());
            ms.WriteByte((byte)c);
        }
    }

    static byte[] ReadExact(Stream s, long n)
    {
        var b = new byte[n]; long got = 0;
        while (got < n)
        {
            int r = s.Read(b, (int)got, (int)Math.Min(1 << 20, n - got));
            if (r <= 0) throw new EndOfStreamException("поток закрыт на " + got + " из " + n);
            got += r;
        }
        return b;
    }

    public static Job HandleLpd(TcpClient c)
    {
        var job = new Job();
        using (var s = c.GetStream())
        {
            s.ReadTimeout = 60000;
            var first = ReadLine(s);
            Log("LPD: команда " + (first == null ? "<eof>" : Hex(Encoding.ASCII.GetBytes(first))) + " '" + (first ?? "").Substring(Math.Min(1, (first ?? "").Length)) + "'");
            if (first == null || first.Length == 0 || first[0] != 2) { s.WriteByte(1); return job; }
            s.WriteByte(0);
            while (true)
            {
                var sub = ReadLine(s);
                if (sub == null || sub.Length == 0) break;
                byte kind = (byte)sub[0];
                var parts = sub.Substring(1).Split(new[] { ' ' }, 2);
                long len = long.Parse(parts[0]);
                string name = parts.Length > 1 ? parts[1] : "";
                Log(string.Format("LPD: подкоманда {0:x2} len={1} name={2}", kind, len, name));
                s.WriteByte(0);
                var body = ReadExact(s, len);
                int term = s.ReadByte();      // завершающий \0
                s.WriteByte(0);
                if (kind == 2) { job.Control = Encoding.ASCII.GetString(body); Log("LPD: control:\n" + Hex(body)); }
                else if (kind == 3) { job.Data = body; job.DataName = name; Log("LPD: data " + len + " байт, заголовок: " + Hex(body, 128)); }
            }
        }
        return job;
    }

    // ---------- PNG без внешних библиотек ----------
    static uint Crc(byte[] b, int o, int n, uint crc = 0xffffffff)
    {
        for (int i = o; i < o + n; i++)
        {
            crc ^= b[i];
            for (int k = 0; k < 8; k++) crc = (crc & 1) != 0 ? 0xEDB88320 ^ (crc >> 1) : crc >> 1;
        }
        return crc;
    }
    static void Chunk(Stream o, string type, byte[] data)
    {
        var len = BitConverter.GetBytes(data.Length); Array.Reverse(len); o.Write(len, 0, 4);
        var td = new byte[4 + data.Length]; Encoding.ASCII.GetBytes(type).CopyTo(td, 0); data.CopyTo(td, 4);
        o.Write(td, 0, td.Length);
        var c = BitConverter.GetBytes(Crc(td, 0, td.Length) ^ 0xffffffff); Array.Reverse(c); o.Write(c, 0, 4);
    }
    static uint Adler(byte[] b, int o, int n, uint a = 1)
    {
        uint s1 = a & 0xffff, s2 = a >> 16;
        for (int i = o; i < o + n; i++) { s1 = (s1 + b[i]) % 65521; s2 = (s2 + s1) % 65521; }
        return (s2 << 16) | s1;
    }
    public static void SavePng(string path, byte[] rgb, int off, int w, int h, int bpp)
    {
        int stride = w * bpp;
        var raw = new MemoryStream();
        uint adler = 1;
        var z = new MemoryStream();
        z.WriteByte(0x78); z.WriteByte(0x9C);
        using (var d = new DeflateStream(z, CompressionLevel.Optimal, true))
        {
            var line = new byte[stride + 1];
            for (int y = 0; y < h; y++)
            {
                line[0] = 0; Array.Copy(rgb, off + y * stride, line, 1, stride);
                d.Write(line, 0, line.Length); adler = Adler(line, 0, line.Length, adler);
            }
        }
        var ad = BitConverter.GetBytes(adler); Array.Reverse(ad); z.Write(ad, 0, 4);
        var ihdr = new byte[13];
        var bw = BitConverter.GetBytes(w); Array.Reverse(bw); bw.CopyTo(ihdr, 0);
        var bh = BitConverter.GetBytes(h); Array.Reverse(bh); bh.CopyTo(ihdr, 4);
        ihdr[8] = 8; ihdr[9] = (byte)(bpp == 3 ? 2 : 0);
        using (var f = File.Create(path))
        {
            f.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }, 0, 8);
            Chunk(f, "IHDR", ihdr); Chunk(f, "IDAT", z.ToArray()); Chunk(f, "IEND", new byte[0]);
        }
    }

    public static void SaveImage(byte[] data, string outDir, string tag)
    {
        Directory.CreateDirectory(outDir);
        File.WriteAllBytes(Path.Combine(outDir, tag + ".dat"), data);
        int mode = BitConverter.ToInt32(data, 0);
        int w = BitConverter.ToInt32(data, 0x1C);
        int bpp = (data.Length - 128) % (w * 3) == 0 ? 3 : 1;   // режим 3 = цвет; для ч/б/серого пока по размеру
        int h = (data.Length - 128) / (w * bpp);
        Log(string.Format("Картинка: mode={0} w={1} h={2} bpp={3} лишних байт={4}", mode, w, h, bpp, (data.Length - 128) - w * h * bpp));
        string png = Path.Combine(outDir, tag + ".png");
        SavePng(png, data, 128, w, h, bpp);
        Log("Сохранено: " + png);
    }

    // ---------- Сценарий ----------
    public static int Run(string devIp, string myIpStr, string host, string macStr, string outDir, int timeoutSec)
    {
        var dev = new IPEndPoint(IPAddress.Parse(devIp), Port);
        var myIp = IPAddress.Parse(myIpStr);
        var mac = new byte[6]; var mp = macStr.Split('-', ':');
        for (int i = 0; i < 6; i++) mac[i] = Convert.ToByte(mp[i], 16);

        TcpListener lst = new TcpListener(IPAddress.Any, Port);
        try { lst.Start(); }
        catch (SocketException e)
        {
            Log("НЕ УДАЛОСЬ занять TCP 0.0.0.0:" + Port + ": " + e.SocketErrorCode + " — порт держит другой процесс (Panasonic PCCMFLPD?)");
            return 10;
        }
        Log("LPD слушает 0.0.0.0:" + Port);
        Job job = null;
        var th = new Thread(() =>
        {
            try
            {
                while (true)   // после данных МФУ шлёт ещё короткое задание-маркер (cfA00N+1 без data) — принимаем и его
                {
                    var c = lst.AcceptTcpClient();
                    Log("TCP: входящее от " + c.Client.RemoteEndPoint);
                    var j = HandleLpd(c);
                    if (j.Data != null) job = j;
                }
            }
            catch (Exception e) { Log("LPD-поток: " + e.Message); }
        });
        th.IsBackground = true; th.Start();

        ushort seq = (ushort)(Environment.TickCount & 0x7fff | 0x100);
        var ann = AnnounceBody(myIp, host, mac);
        SendUdp(dev, Pkt(seq++, 0x31, 0x03, ann), 1500);
        SendUdp(dev, Pkt(seq++, 0x31, 0x05, ann), 0);
        for (int i = 0; i < 3; i++) { SendUdp(dev, Pkt(seq++, 0x32, 0x10, new byte[0]), 300); }
        SniffOpen();
        while (sniff != IntPtr.Zero) { IntPtr hh, dd; if (pcap_next_ex(sniff, out hh, out dd) != 1) break; }  // сбросить буфер
        var r = SendUdp(dev, Pkt(seq, 0x32, 0x41, ScanParamsBody(3, 300, 300, 2480, 3508)), sniff != IntPtr.Zero ? 300 : 3000);
        if (r == null) r = SniffReply(dev.Address, seq, 0x43, 3000);
        SniffClose();
        if (r == null || r.Length < 28 || r[15] != 0x43) { Log("Нет ответа 32 43 на параметры скана"); lst.Stop(); return 11; }
        var id = new byte[8]; Array.Copy(r, 20, id, 0, 8);
        SendUdp(dev, Pkt(seq, 0x32, 0x62, id), 0);
        seq++;
        Log("Задание " + Hex(id) + " запущено, жду данные по TCP до " + timeoutSec + " с");
        var t0 = DateTime.Now;
        while ((job == null || job.Data == null) && (DateTime.Now - t0).TotalSeconds < timeoutSec) Thread.Sleep(200);
        if (job != null && job.Data != null) Thread.Sleep(3000);   // дать МФУ отдать завершающее соединение
        lst.Stop();
        for (int i = 0; i < 3; i++) SendUdp(dev, Pkt(seq++, 0x32, 0x10, new byte[0]), 300);
        if (job == null || job.Data == null) { Log("Данные не пришли"); return 12; }
        SaveImage(job.Data, outDir, "scan_" + DateTime.Now.ToString("yyyyMMdd_HHmmss"));
        return 0;
    }
}
