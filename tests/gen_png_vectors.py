# SPDX-License-Identifier: MIT
"""Генерирует tests/png_vectors.h: эталонные PNG и zlib-потоки для тестов декодера.

Только стандартная библиотека (zlib). Пиксели задаются формулой, которую тест
повторяет сам, поэтому ожидаемые значения в заголовок не пишутся.
Запуск: python tests/gen_png_vectors.py
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "png_vectors.h")


def pixel(x, y):
    # Должно совпадать с expected_pixel() в tests/test_png.cpp.
    return ((x * 37 + y * 11) & 255,
            ((x * 13) ^ (y * 29)) & 255,
            (x * y * 7 + 3) & 255,
            (255 - x * 9 - y * 5) & 255)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def raw_rows(w, h, bpp):
    """Строки с фильтрами 0..4 по кругу (y % 5): покрываются все типы фильтров."""
    prev = bytes(w * bpp)
    out = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes(pixel(x, y)[:bpp])
        ft = y % 5
        enc = bytearray()
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = [0, a, b, (a + b) >> 1, paeth(a, b, c)][ft]
            enc.append((row[i] - pred) & 255)
        out.append(ft)
        out += enc
        prev = bytes(row)
    return bytes(out)


def chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)


def zlib_stream(data, level, strategy):
    c = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
    return c.compress(data) + c.flush(zlib.Z_FINISH)


def btype(z):
    return (z[2] >> 1) & 3


def png(w, h, ctype, level, strategy, idat_parts=1, want_btype=None):
    bpp = 4 if ctype == 6 else 3
    z = zlib_stream(raw_rows(w, h, bpp), level, strategy)
    if want_btype is not None:
        assert btype(z) == want_btype, (w, h, btype(z))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)
    body = chunk(b"IHDR", ihdr) + chunk(b"tEXt", b"k\x00v")
    step = (len(z) + idat_parts - 1) // idat_parts
    for i in range(idat_parts):
        body += chunk(b"IDAT", z[i * step:(i + 1) * step])
    return b"\x89PNG\r\n\x1a\n" + body + chunk(b"IEND", b"")


def mixed_zlib():
    """Один zlib-поток из трёх видов блоков подряд: stored, fixed, dynamic."""
    # Должно совпадать с mixed_expected() в tests/test_png.cpp.
    a = bytes(range(256)) * 2
    b = b"ab" * 200 + b"x" * 300  # длинные повторы, перекрытие при dist=1
    c = bytes((i * 7 + (i >> 3)) & 255 for i in range(3000)) + b"tail" * 50
    raw = b""
    s = zlib.compressobj(0, zlib.DEFLATED, -15, 9, zlib.Z_DEFAULT_STRATEGY)
    raw += s.compress(a) + s.flush(zlib.Z_SYNC_FLUSH)
    f = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_FIXED)
    raw += f.compress(b) + f.flush(zlib.Z_SYNC_FLUSH)
    d = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_DEFAULT_STRATEGY)
    raw += d.compress(c) + d.flush(zlib.Z_FINISH)
    assert (raw[0] >> 1) & 3 == 0  # первый блок stored
    plain = a + b + c
    assert zlib.decompress(raw, -15) == plain
    z = b"\x78\xda" + raw + struct.pack(">I", zlib.adler32(plain) & 0xFFFFFFFF)
    assert zlib.decompress(z) == plain
    return z, len(plain)


def emit(name, data, lines):
    lines.append("static const unsigned char %s[%d] = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join("0x%02x" % v for v in data[i:i + 16]) + ",")
    lines.append("};")


def main():
    lines = [
        "// SPDX-License-Identifier: MIT",
        "// Сгенерировано tests/gen_png_vectors.py — не править вручную.",
        "#pragma once",
        "",
    ]
    vecs = [
        ("kPngRgbaStored", 7, 5, png(7, 5, 6, 0, zlib.Z_DEFAULT_STRATEGY, want_btype=0)),
        ("kPngRgbaFixed", 13, 9, png(13, 9, 6, 9, zlib.Z_FIXED, want_btype=1)),
        ("kPngRgbDynamic", 32, 24, png(32, 24, 2, 9, zlib.Z_DEFAULT_STRATEGY, want_btype=2)),
        ("kPngRgbaMultiIdat", 48, 48, png(48, 48, 6, 9, zlib.Z_DEFAULT_STRATEGY, idat_parts=3, want_btype=2)),
    ]
    for name, w, h, data in vecs:
        lines.append("// %dx%d" % (w, h))
        emit(name, data, lines)
        lines.append("")
    z, n = mixed_zlib()
    lines.append("// stored + fixed + dynamic в одном zlib-потоке; распакованный размер:")
    lines.append("static const unsigned kZlibMixedPlainSize = %d;" % n)
    emit("kZlibMixed", z, lines)
    lines.append("")
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print("written", OUT)


if __name__ == "__main__":
    main()
