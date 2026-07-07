#!/usr/bin/env python3
"""Generate corrupt .dta test files for error-handling tests.

These files exercise the reader's validation of size fields read from the
file (variable count, observation count, value-label table sizes, strL
lengths) and its handling of label text that lacks a NUL terminator.
Derived by byte-patching the committed good files, so this script has no
dependencies beyond the standard library.
"""

import struct
import os

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")


def read(name):
    with open(os.path.join(DATA_DIR, name), "rb") as f:
        return bytearray(f.read())


def write(name, buf):
    path = os.path.join(DATA_DIR, name)
    with open(path, "wb") as f:
        f.write(buf)
    print(f"  Written {path}")


def corrupt_nvars():
    """Format 119 header claiming 4 billion variables."""
    buf = read("format_119.dta")
    k_pos = buf.index(b"<K>") + 3
    assert buf[k_pos + 4 : k_pos + 8] == b"</K>"
    struct.pack_into("<I", buf, k_pos, 0xFFFFFFFF)
    write("corrupt_nvars.dta", buf)


def corrupt_nobs():
    """Format 119 header claiming 2^60 observations."""
    buf = read("format_119.dta")
    n_pos = buf.index(b"<N>") + 3
    assert buf[n_pos + 8 : n_pos + 12] == b"</N>"
    struct.pack_into("<Q", buf, n_pos, 1 << 60)
    write("corrupt_nobs.dta", buf)


def corrupt_value_label():
    """Value-label table claiming 2 billion entries."""
    buf = read("value_labels.dta")
    # <lbl> + len(4) + labname(33) + pad(3), then n(4), txtlen(4)
    n_pos = buf.index(b"<lbl>") + 5 + 4 + 33 + 3
    struct.pack_into("<I", buf, n_pos, 0x7FFFFFFF)
    write("corrupt_value_label.dta", buf)


def unterminated_label():
    """Label text blobs with every NUL replaced, so no terminator exists."""
    buf = read("value_labels.dta")
    pos = 0
    while True:
        pos = buf.find(b"<lbl>", pos)
        if pos < 0:
            break
        header = pos + 5 + 4 + 33 + 3
        n, txtlen = struct.unpack_from("<II", buf, header)
        txt_start = header + 8 + 8 * n
        for i in range(txt_start, txt_start + txtlen):
            if buf[i] == 0:
                buf[i] = ord("X")
        pos = txt_start + txtlen
    write("unterminated_label.dta", buf)


def corrupt_strl_len():
    """Minimal format 117 file with one strL column whose GSO record claims
    a 4 GB payload that is not present."""
    buf = bytearray()
    offsets = [0] * 14

    def tag(t):
        buf.extend(t.encode())

    def fixed(s, length):
        b = s.encode()[: length - 1]
        buf.extend(b + b"\x00" * (length - len(b)))

    offsets[0] = len(buf)
    tag("<stata_dta><header><release>117</release><byteorder>LSF</byteorder><K>")
    buf.extend(struct.pack("<H", 1))
    tag("</K><N>")
    buf.extend(struct.pack("<I", 1))
    tag("</N><label>")
    buf.extend(struct.pack("B", 0))
    tag("</label><timestamp>")
    buf.extend(struct.pack("B", 0))
    tag("</timestamp></header>")

    offsets[1] = len(buf)
    tag("<map>")
    map_pos = len(buf)
    buf.extend(b"\x00" * (14 * 8))
    tag("</map>")

    offsets[2] = len(buf)
    tag("<variable_types>")
    buf.extend(struct.pack("<H", 32768))  # strL
    tag("</variable_types>")

    offsets[3] = len(buf)
    tag("<varnames>")
    fixed("note", 33)
    tag("</varnames>")

    offsets[4] = len(buf)
    tag("<sortlist>")
    buf.extend(b"\x00" * (2 * 2))
    tag("</sortlist>")

    offsets[5] = len(buf)
    tag("<formats>")
    fixed("%9s", 49)
    tag("</formats>")

    offsets[6] = len(buf)
    tag("<value_label_names>")
    fixed("", 33)
    tag("</value_label_names>")

    offsets[7] = len(buf)
    tag("<variable_labels>")
    fixed("", 81)
    tag("</variable_labels>")

    offsets[8] = len(buf)
    tag("<characteristics></characteristics>")

    offsets[9] = len(buf)
    tag("<data>")
    buf.extend(struct.pack("<II", 1, 1))  # (v, o) reference
    tag("</data>")

    offsets[10] = len(buf)
    tag("<strls>")
    tag("GSO")
    buf.extend(struct.pack("<II", 1, 1))  # v, o
    buf.extend(struct.pack("B", 130))  # type: ASCII
    buf.extend(struct.pack("<I", 0xFFFFFF00))  # claimed length
    buf.extend(b"hi\x00")
    tag("</strls>")

    offsets[11] = len(buf)
    tag("<value_labels></value_labels>")

    offsets[12] = len(buf)
    tag("</stata_dta>")
    offsets[13] = len(buf)

    for i, off in enumerate(offsets):
        struct.pack_into("<Q", buf, map_pos + i * 8, off)
    write("corrupt_strl_len.dta", buf)


if __name__ == "__main__":
    corrupt_nvars()
    corrupt_nobs()
    corrupt_value_label()
    unterminated_label()
    corrupt_strl_len()
