// SPDX-License-Identifier: GPL-2.0-or-later
// Разбор/сборка потока печати Panasonic KX-MB2020 GDI (PJL + ESC-команды + JBIG-полосы ESC*b0W).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

public static class GdiPrn
{
    public class Band { public int Offset, Len; public uint Flags; public int Lines; public byte[] Bie; public byte[] Hdr12; }

    public static List<Band> ParseBands(byte[] d)
    {
        var L = new List<Band>();
        for (int i = 0; i + 21 < d.Length; i++)
        {
            if (d[i] == 0x1b && d[i + 1] == (byte)'*' && d[i + 2] == (byte)'b' && d[i + 3] == (byte)'0' && d[i + 4] == (byte)'W')
            {
                var b = new Band { Offset = i, Len = BitConverter.ToInt32(d, i + 5), Flags = BitConverter.ToUInt32(d, i + 9), Lines = BitConverter.ToUInt16(d, i + 15) };
                b.Hdr12 = new byte[12]; Array.Copy(d, i + 9, b.Hdr12, 0, 12);
                b.Bie = new byte[b.Len]; Array.Copy(d, i + 21, b.Bie, 0, b.Len);   // длина включает BIH и завершающий FF 02
                L.Add(b); i += 21 + b.Len - 1;
            }
        }
        return L;
    }

    public static Jbig1.Image DecodePage(byte[] d, List<string> log)
    {
        var bands = ParseBands(d);
        int w = 0, h = 0; var imgs = new List<Jbig1.Image>();
        foreach (var b in bands)
        {
            var im = Jbig1.Decode(b.Bie, 0, b.Bie.Length);
            int black = 0; foreach (var x in im.Bits) for (int k = 0; k < 8; k++) black += (x >> k) & 1;
            log.Add(string.Format("полоса @0x{0:x4}: BIE {1} байт, {2}x{3}, чёрных пикселей {4}", b.Offset, b.Len, im.W, im.H, black));
            imgs.Add(im); w = im.W; h += im.H;
        }
        var page = new Jbig1.Image(w, h); int y0 = 0;
        foreach (var im in imgs) { Array.Copy(im.Bits, 0, page.Bits, y0 * page.Bpl, im.Bits.Length); y0 += im.H; }
        return page;
    }

    // ---------- PNG 1 бит (0 = чёрный в PNG, в JBIG 1 = чёрный -> инвертируем) ----------
    static uint Crc(byte[] b) { uint c = 0xffffffff; foreach (var x in b) { c ^= x; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1; } return c ^ 0xffffffff; }
    static void Chunk(Stream o, string t, byte[] data)
    {
        var td = new byte[4 + data.Length]; Encoding.ASCII.GetBytes(t).CopyTo(td, 0); data.CopyTo(td, 4);
        var l = BitConverter.GetBytes(data.Length); Array.Reverse(l); o.Write(l, 0, 4); o.Write(td, 0, td.Length);
        var c = BitConverter.GetBytes(Crc(td)); Array.Reverse(c); o.Write(c, 0, 4);
    }
    public static void SavePng1(Jbig1.Image im, string path)
    {
        var z = new MemoryStream(); z.WriteByte(0x78); z.WriteByte(0x9C);
        uint s1 = 1, s2 = 0;
        using (var df = new DeflateStream(z, CompressionLevel.Optimal, true))
        {
            var line = new byte[im.Bpl + 1];
            for (int y = 0; y < im.H; y++)
            {
                line[0] = 0; for (int i = 0; i < im.Bpl; i++) line[i + 1] = (byte)~im.Bits[y * im.Bpl + i];
                df.Write(line, 0, line.Length);
                foreach (var x in line) { s1 = (s1 + x) % 65521; s2 = (s2 + s1) % 65521; }
            }
        }
        var ad = BitConverter.GetBytes((s2 << 16) | s1); Array.Reverse(ad); z.Write(ad, 0, 4);
        var ih = new byte[13]; var bw = BitConverter.GetBytes(im.W); Array.Reverse(bw); bw.CopyTo(ih, 0);
        var bh = BitConverter.GetBytes(im.H); Array.Reverse(bh); bh.CopyTo(ih, 4); ih[8] = 1; ih[9] = 0;
        using (var f = File.Create(path)) { f.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }, 0, 8); Chunk(f, "IHDR", ih); Chunk(f, "IDAT", z.ToArray()); Chunk(f, "IEND", new byte[0]); }
    }

    // ---------- сборка задания ----------
    public static byte[] BuildJob(Jbig1.Image page, string jobName, string owner, string host, string pcIp, DateTime t, int bandLines = 512)
    {
        var o = new MemoryStream();
        Action<string> A = s => { var b = Encoding.ASCII.GetBytes(s); o.Write(b, 0, b.Length); };
        string E = "\x1b";
        A(E + "%-12345X");
        A("@PJL COMMENT Panasonic KX-MB2020 GDI\n@PJL COMMENT TREATASCHARACTER\n@PJL COMMENT version AMD64 5.01.032.001\n");
        A("@PJL COMMENT DriverVersion=\"1.048(1.048.002.e)\"\n@PJL COMMENT DriverOrgVer=\"0.048.000.e\"\n");
        A("@PJL COMMENT OSVersion=\"Windows 10.0;Build 26300;AMD64\"\n@PJL COMMENT Monochrome MFP\n@PJL COMMENT Soc Type=Sirius\n");
        A("@PJL JOB TIME=\"" + t.ToString("yyyy/MM/dd HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture) + "\"\n@PJL JOB NAME=\"" + jobName + "\"\n@PJL JOB OWNER=\"" + owner + "\"\n");
        A("@PJL JOB HOST=\"" + host + "\"\n@PJL JOB PCIP=\"" + pcIp + "\"\n");
        A("@PJL SET RET=ON\n@PJL SET ECONOMODE=OFF\n@PJL SET COPIES=1\n@PJL SET DUPLEX=OFF\n@PJL ENTER LANGUAGE=GDI\n");
        A(E + "&&0S" + E + "&r600W" + E + "&r600L" + E + "&&0Y" + E + "&l26A" + E + "&l2H" + E + "&&11W");
        A(E + "*r" + page.H + "T" + E + "*r" + page.W + "S" + E + "&a100L" + E + "&l100E" + E + "&&0T");
        int nb = (page.H + bandLines - 1) / bandLines;
        for (int bi = 0; bi < nb; bi++)
        {
            int y0 = bi * bandLines, lines = Math.Min(bandLines, page.H - y0);
            var band = new Jbig1.Image(page.W, lines);
            Array.Copy(page.Bits, y0 * page.Bpl, band.Bits, 0, lines * page.Bpl);
            var bie = Jbig1.Encode(band, Jbig1.LRLTWO | Jbig1.TPBON, true);   // заканчивается FF 02
            A(E + "*b0W");
            o.Write(BitConverter.GetBytes(bie.Length), 0, 4);   // длина = BIH + SDE + FF 02
            uint flags = (bi == 0 ? 0x10000u : 0u) | (bi == nb - 1 ? 1u : 0u);
            o.Write(BitConverter.GetBytes(flags), 0, 4);
            o.WriteByte(0x0b); o.WriteByte(0); o.Write(BitConverter.GetBytes((ushort)lines), 0, 2);
            o.Write(new byte[4], 0, 4);
            o.Write(bie, 0, bie.Length);
        }
        A(E + "&&1R" + E + "%-12345X@PJL EOJ NAME=\"" + jobName + "\"\n" + E + "%-12345X");
        return o.ToArray();
    }
}
