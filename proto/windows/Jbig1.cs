// SPDX-License-Identifier: GPL-2.0-or-later
// JBIG-1 QM-кодер, таблицы и логика контекстов основаны на jbig-kit (c) Markus Kuhn, GPL — см. third_party/jbigkit.
// Минимальный JBIG-1 (ITU-T T.82) кодер/декодер: один битовый слой (P=1), только нижнее разрешение (D=0),
// одна полоса на BIE (L0 = YD), двух- или трёхстрочный шаблон, TPBON, без AT-перемещений.
// Арифметический QM-кодер и таблицы перенесены из jbig-kit (Markus Kuhn, GPL) — third_party\jbigkit.
using System;
using System.Collections.Generic;
using System.IO;

public static class Jbig1
{
    public const int LRLTWO = 0x40, VLENGTH = 0x20, TPDON = 0x10, TPBON = 0x08, DPON = 0x04;
    const int TPB2CX = 0x195, TPB3CX = 0x0e5;

    static readonly ushort[] Lsz = {
      0x5a1d,0x2586,0x1114,0x080b,0x03d8,0x01da,0x00e5,0x006f,0x0036,0x001a,0x000d,0x0006,0x0003,0x0001,0x5a7f,0x3f25,
      0x2cf2,0x207c,0x17b9,0x1182,0x0cef,0x09a1,0x072f,0x055c,0x0406,0x0303,0x0240,0x01b1,0x0144,0x00f5,0x00b7,0x008a,
      0x0068,0x004e,0x003b,0x002c,0x5ae1,0x484c,0x3a0d,0x2ef1,0x261f,0x1f33,0x19a8,0x1518,0x1177,0x0e74,0x0bfb,0x09f8,
      0x0861,0x0706,0x05cd,0x04de,0x040f,0x0363,0x02d4,0x025c,0x01f8,0x01a4,0x0160,0x0125,0x00f6,0x00cb,0x00ab,0x008f,
      0x5b12,0x4d04,0x412c,0x37d8,0x2fe8,0x293c,0x2379,0x1edf,0x1aa9,0x174e,0x1424,0x119c,0x0f6b,0x0d51,0x0bb6,0x0a40,
      0x5832,0x4d1c,0x438e,0x3bdd,0x34ee,0x2eae,0x299a,0x2516,0x5570,0x4ca9,0x44d9,0x3e22,0x3824,0x32b4,0x2e17,0x56a8,
      0x4f46,0x47e5,0x41cf,0x3c3d,0x375e,0x5231,0x4c0f,0x4639,0x415e,0x5627,0x50e7,0x4b85,0x5597,0x504f,0x5a10,0x5522,
      0x59eb };
    static readonly byte[] Nmps = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,13,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
        33,34,35,9,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,32,
        65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,48,81,82,83,84,85,86,87,71,89,90,91,92,93,94,86,96,
        97,98,99,100,93,102,103,104,99,106,107,103,109,107,111,109,111 };
    static readonly byte[] Nlps = {
        129,14,16,18,20,23,25,28,30,33,35,9,10,12,143,36,38,39,40,42,43,45,46,48,49,51,52,54,56,57,59,60,
        62,63,32,33,165,64,65,67,68,69,70,72,73,74,75,77,78,79,48,50,50,51,52,53,54,55,56,57,58,59,61,61,
        193,80,81,82,83,84,86,87,87,72,72,74,74,75,77,77,208,88,89,90,91,92,93,86,216,95,96,97,99,99,93,223,
        101,102,103,104,99,105,106,107,103,233,108,109,110,111,238,112,240 };

    public class Image { public int W, H; public byte[] Bits; public int Bpl { get { return (W + 7) >> 3; } }
        public int Get(int x, int y) { if (x < 0 || y < 0 || x >= W || y >= H) return 0; return (Bits[y * Bpl + (x >> 3)] >> (7 - (x & 7))) & 1; }
        public void Set(int x, int y, int v) { int i = y * Bpl + (x >> 3); if (v != 0) Bits[i] |= (byte)(0x80 >> (x & 7)); else Bits[i] &= (byte)~(0x80 >> (x & 7)); }
        public Image(int w, int h) { W = w; H = h; Bits = new byte[((w + 7) >> 3) * h]; } }

    static uint BE32(byte[] b, int o) { return (uint)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]); }

    static int Ctx(Image im, int x, int y, bool two)
    {
        if (two)   // ряд y-1: x-3..x+2 (бит 9..4, x+2 = AT по умолчанию), ряд y: x-4..x-1 (бит 3..0)
            return (im.Get(x - 3, y - 1) << 9) | (im.Get(x - 2, y - 1) << 8) | (im.Get(x - 1, y - 1) << 7) | (im.Get(x, y - 1) << 6)
                 | (im.Get(x + 1, y - 1) << 5) | (im.Get(x + 2, y - 1) << 4)
                 | (im.Get(x - 4, y) << 3) | (im.Get(x - 3, y) << 2) | (im.Get(x - 2, y) << 1) | im.Get(x - 1, y);
        // трёхстрочный: y-2: x-1..x+1 (9..7), y-1: x-2..x+2 (6..2, x+2=AT), y: x-2,x-1 (1,0)
        return (im.Get(x - 1, y - 2) << 9) | (im.Get(x, y - 2) << 8) | (im.Get(x + 1, y - 2) << 7)
             | (im.Get(x - 2, y - 1) << 6) | (im.Get(x - 1, y - 1) << 5) | (im.Get(x, y - 1) << 4) | (im.Get(x + 1, y - 1) << 3) | (im.Get(x + 2, y - 1) << 2)
             | (im.Get(x - 2, y) << 1) | im.Get(x - 1, y);
    }

    // ---------------- декодер ----------------
    class Dec
    {
        byte[] d; int p, end; long c, a; int ct; bool startup = true, ready; public byte[] st = new byte[4096];
        public Dec(byte[] data, int start, int stop) { d = data; p = start; end = stop; c = 0; a = 1; ct = 0; }
        public int Bit(int cx)
        {
            while (a < 0x8000 || startup)
            {
                if (ct < 1 && !ready)
                {
                    if (p >= end) ready = true;
                    else if (d[p] == 0xff)
                    {
                        if (p + 1 < end && d[p + 1] == 0x00) { c |= 0xffL << (8 - ct); ct += 8; p += 2; }
                        else ready = true;
                    }
                    else { c |= (long)d[p++] << (8 - ct); ct += 8; }
                }
                c <<= 1; a <<= 1; --ct;
                if (a == 0x10000L) startup = false;
            }
            int ss = st[cx] & 0x7f; uint lsz = Lsz[ss]; int pix;
            a -= lsz;
            if ((c >> 16) < a)
            {
                if ((a & 0xffff8000L) != 0) return st[cx] >> 7;
                if (a < lsz) { pix = 1 - (st[cx] >> 7); st[cx] = (byte)((st[cx] & 0x80) ^ Nlps[ss]); }
                else { pix = st[cx] >> 7; st[cx] = (byte)((st[cx] & 0x80) | Nmps[ss]); }
            }
            else
            {
                c -= a << 16;
                if (a < lsz) { a = lsz; pix = st[cx] >> 7; st[cx] = (byte)((st[cx] & 0x80) | Nmps[ss]); }
                else { a = lsz; pix = 1 - (st[cx] >> 7); st[cx] = (byte)((st[cx] & 0x80) ^ Nlps[ss]); }
            }
            return pix;
        }
    }

    public static Image Decode(byte[] bie, int off, int len)
    {
        if (bie[off] != 0 || bie[off + 1] != 0 || bie[off + 2] != 1) throw new NotSupportedException("нужно DL=0 D=0 P=1");
        int w = (int)BE32(bie, off + 4), h = (int)BE32(bie, off + 8), l0 = (int)BE32(bie, off + 12);
        int opt = bie[off + 19]; bool two = (opt & LRLTWO) != 0, tpb = (opt & TPBON) != 0;
        if (bie[off + 16] != 0) throw new NotSupportedException("AT (MX) не поддержан");
        if (l0 < h) throw new NotSupportedException("несколько полос в BIE не поддержано");
        if ((opt & (DPON | TPDON)) != 0) throw new NotSupportedException("DPON/TPDON");
        var im = new Image(w, h);
        var dec = new Dec(bie, off + 20, off + len);
        int lntp = 1; int cxTp = two ? TPB2CX : TPB3CX;
        for (int y = 0; y < h; y++)
        {
            if (tpb)
            {
                int s = dec.Bit(cxTp);
                if (s == 0) lntp ^= 1;
                if (lntp == 0)
                {   // типовая строка = копия предыдущей
                    if (y > 0) Array.Copy(im.Bits, (y - 1) * im.Bpl, im.Bits, y * im.Bpl, im.Bpl);
                    continue;
                }
            }
            for (int x = 0; x < w; x++) if (dec.Bit(Ctx(im, x, y, two)) != 0) im.Set(x, y, 1);
        }
        return im;
    }

    // ---------------- кодер ----------------
    class Enc
    {
        long c = 0, a = 0x10000; int ct = 11, sc = 0, buffer = -1; public byte[] st = new byte[4096];
        public MemoryStream o = new MemoryStream();
        void Out(int b) { o.WriteByte((byte)b); }
        public void Bit(int cx, int pix)
        {
            int ss = st[cx] & 0x7f; uint lsz = Lsz[ss];
            if ((((pix << 7) ^ st[cx]) & 0x80) != 0)
            {
                a -= lsz;
                if (a >= lsz) { c += a; a = lsz; }
                st[cx] = (byte)((st[cx] & 0x80) ^ Nlps[ss]);
            }
            else
            {
                a -= lsz;
                if ((a & 0xffff8000L) != 0) return;
                if (a < lsz) { c += a; a = lsz; }
                st[cx] = (byte)((st[cx] & 0x80) | Nmps[ss]);
            }
            do
            {
                a <<= 1; c <<= 1; --ct;
                if (ct == 0)
                {
                    long temp = c >> 19;
                    if ((temp & 0xffffff00L) != 0)
                    {
                        if (buffer >= 0) { ++buffer; Out(buffer); if (buffer == 0xff) Out(0); }
                        for (; sc > 0; --sc) Out(0x00);
                        buffer = (int)(temp & 0xff);
                    }
                    else if (temp == 0xff) ++sc;
                    else
                    {
                        if (buffer >= 0) Out(buffer);
                        for (; sc > 0; --sc) { Out(0xff); Out(0x00); }
                        buffer = (int)temp;
                    }
                    c &= 0x7ffffL; ct = 8;
                }
            } while (a < 0x8000);
        }
        public void Flush()
        {
            long temp = (a - 1 + c) & 0xffff0000L;
            c = temp < c ? temp + 0x8000 : temp;
            c <<= ct;
            if ((c & 0xf8000000L) != 0)
            {
                if (buffer >= 0) { Out(buffer + 1); if (buffer + 1 == 0xff) Out(0); }
                if ((c & 0x7fff800L) != 0) for (; sc > 0; --sc) Out(0x00);
            }
            else
            {
                if (buffer >= 0) Out(buffer);
                for (; sc > 0; --sc) { Out(0xff); Out(0x00); }
            }
            if ((c & 0x7fff800L) != 0)
            {
                Out((int)((c >> 19) & 0xff)); if (((c >> 19) & 0xff) == 0xff) Out(0);
                if ((c & 0x7f800L) != 0) { Out((int)((c >> 11) & 0xff)); if (((c >> 11) & 0xff) == 0xff) Out(0); }
            }
        }
    }

    // Один BIE: BIH(20) + SDE + FF 02 (SDNORM)
    // panasonicPad: кодер Panasonic дописывает лишний 0x00 перед FF 02 (так получается байт-в-байт как у драйвера)
    public static byte[] Encode(Image im, int opt, bool panasonicPad = false)
    {
        bool two = (opt & LRLTWO) != 0, tpb = (opt & TPBON) != 0;
        var e = new Enc();
        int ltpOld = 0, cxTp = two ? TPB2CX : TPB3CX;
        for (int y = 0; y < im.H; y++)
        {
            if (tpb)
            {
                bool same = true;
                for (int i = 0; i < im.Bpl && same; i++) same = im.Bits[y * im.Bpl + i] == (y > 0 ? im.Bits[(y - 1) * im.Bpl + i] : 0);
                int ltp = same ? 1 : 0;
                e.Bit(cxTp, ltp == ltpOld ? 1 : 0);
                ltpOld = ltp;
                if (same) continue;
            }
            for (int x = 0; x < im.W; x++) e.Bit(Ctx(im, x, y, two), im.Get(x, y));
        }
        e.Flush();
        if (panasonicPad) e.o.WriteByte(0);
        var sde = e.o.ToArray();
        var r = new byte[20 + sde.Length + 2];
        r[0] = 0; r[1] = 0; r[2] = 1; r[3] = 0;
        Action<int, int> be = (o, v) => { r[o] = (byte)(v >> 24); r[o + 1] = (byte)(v >> 16); r[o + 2] = (byte)(v >> 8); r[o + 3] = (byte)v; };
        be(4, im.W); be(8, im.H); be(12, im.H);
        r[16] = 0; r[17] = 0; r[18] = 0; r[19] = (byte)opt;
        sde.CopyTo(r, 20);
        r[r.Length - 2] = 0xff; r[r.Length - 1] = 0x02;
        return r;
    }
}
