#!/usr/bin/env python3
"""Generate .dta test files in various format versions.

Pandas supports writing Stata format versions 114-119.
For versions 120-121, we construct minimal binary files manually.
"""

import struct
import pandas as pd
import numpy as np
import os

OUT_DIR = os.path.join(os.path.dirname(__file__), "data")

# ============================================================
# Common test data: 3 rows with int, string, double columns
# ============================================================
df = pd.DataFrame(
    {
        "id": pd.array([1, 2, 3], dtype="int32"),
        "name": ["Alice", "Bob", "Charlie"],
        "score": [95.5, 87.3, 91.0],
    }
)


def generate_pandas_formats():
    """Generate format versions 117-119 using pandas."""
    for version in [117, 118, 119]:
        path = os.path.join(OUT_DIR, f"format_{version}.dta")
        df.to_stata(
            path,
            write_index=False,
            version=version,
        )
        print(f"  Written {path} (format {version})")


def write_dta_binary(path, version, rows, columns):
    """Write a minimal .dta file in the specified format version.

    columns: list of (name, type_code, byte_width, format_str)
    rows: list of row tuples (raw bytes per column)
    """
    # Determine version parameters
    if version <= 117:
        varname_len = 33
        fmt_len = 49
        label_name_len = 33
        var_label_len = 81
        k_size = 2
        n_size = 4
        ds_label_len_size = 1
        sortlist_entry = 2
    elif version in (118, 120):
        varname_len = 129
        fmt_len = 57
        label_name_len = 129
        var_label_len = 321
        k_size = 2
        n_size = 8
        ds_label_len_size = 2
        sortlist_entry = 2
    else:  # 119, 121
        varname_len = 129
        fmt_len = 57
        label_name_len = 129
        var_label_len = 321
        k_size = 4
        n_size = 8
        ds_label_len_size = 2
        sortlist_entry = 4

    nvar = len(columns)
    nobs = len(rows)
    row_width = sum(c[2] for c in columns)

    # Two-pass approach: first build without map, then patch map offsets
    buf = bytearray()
    map_placeholder_pos = None  # position of the 14*8 bytes in buf

    def w(data):
        buf.extend(data)

    def pos():
        return len(buf)

    def w_str(s, length):
        b = s.encode("utf-8")[: length - 1]
        buf.extend(b)
        buf.extend(b"\x00" * (length - len(b)))

    def w_tag(tag):
        buf.extend(f"<{tag}>".encode())

    def w_endtag(tag):
        buf.extend(f"</{tag}>".encode())

    # Track offsets for map entries 0-13
    # Index meaning: 0=stata_dta, 1=map, 2=variable_types, 3=varnames,
    # 4=sortlist, 5=formats, 6=value_label_names, 7=variable_labels,
    # 8=characteristics, 9=data, 10=strls, 11=value_labels, 12=</stata_dta>, 13=EOF
    offsets = [0] * 14

    # <stata_dta>
    offsets[0] = pos()
    w_tag("stata_dta")

    # <header>
    w_tag("header")
    w(b"<release>")
    w(str(version).encode())
    w(b"</release>")
    w(b"<byteorder>LSF</byteorder>")

    w(b"<K>")
    if k_size == 2:
        w(struct.pack("<H", nvar))
    else:
        w(struct.pack("<I", nvar))
    w(b"</K>")

    w(b"<N>")
    if n_size == 4:
        w(struct.pack("<I", nobs))
    else:
        w(struct.pack("<Q", nobs))
    w(b"</N>")

    w(b"<label>")
    if ds_label_len_size == 1:
        w(struct.pack("B", 0))
    else:
        w(struct.pack("<H", 0))
    w(b"</label>")

    w(b"<timestamp>")
    w(struct.pack("B", 0))
    w(b"</timestamp>")

    w_endtag("header")

    # <map>
    offsets[1] = pos()
    w_tag("map")
    map_placeholder_pos = pos()
    w(b"\x00" * (14 * 8))  # placeholder
    w_endtag("map")

    # <variable_types>
    offsets[2] = pos()
    w_tag("variable_types")
    for _, tc, _, _ in columns:
        w(struct.pack("<H", tc))
    w_endtag("variable_types")

    # <varnames>
    offsets[3] = pos()
    w_tag("varnames")
    for name, _, _, _ in columns:
        w_str(name, varname_len)
    w_endtag("varnames")

    # <sortlist>
    offsets[4] = pos()
    w_tag("sortlist")
    w(b"\x00" * (sortlist_entry * (nvar + 1)))
    w_endtag("sortlist")

    # <formats>
    offsets[5] = pos()
    w_tag("formats")
    for _, _, _, fmt in columns:
        w_str(fmt, fmt_len)
    w_endtag("formats")

    # <value_label_names>
    offsets[6] = pos()
    w_tag("value_label_names")
    for _ in columns:
        w_str("", label_name_len)
    w_endtag("value_label_names")

    # <variable_labels>
    offsets[7] = pos()
    w_tag("variable_labels")
    for _ in columns:
        w_str("", var_label_len)
    w_endtag("variable_labels")

    # <characteristics>
    offsets[8] = pos()
    w_tag("characteristics")
    w_endtag("characteristics")

    # <data>
    offsets[9] = pos()
    w_tag("data")
    for row in rows:
        w(row)
    w_endtag("data")

    # <strls>
    offsets[10] = pos()
    w_tag("strls")
    w_endtag("strls")

    # <value_labels>
    offsets[11] = pos()
    w_tag("value_labels")
    w_endtag("value_labels")

    # </stata_dta>
    offsets[12] = pos()
    w_endtag("stata_dta")
    offsets[13] = pos()  # EOF

    # Patch map offsets
    for i, off in enumerate(offsets):
        struct.pack_into("<Q", buf, map_placeholder_pos + i * 8, off)

    with open(path, "wb") as f:
        f.write(buf)
    print(f"  Written {path} (format {version}, binary)")


def generate_binary_formats():
    """Generate format versions 120 and 121 manually."""
    columns = [
        ("id", 65528, 4, "%12.0g"),  # long
        ("name", 7, 7, "%7s"),  # str7 (fixed width)
        ("score", 65526, 8, "%10.0g"),  # double
    ]

    rows_data = [
        (1, "Alice", 95.5),
        (2, "Bob", 87.3),
        (3, "Charlie", 91.0),
    ]

    rows = []
    for id_val, name_val, score_val in rows_data:
        row = bytearray()
        row.extend(struct.pack("<i", id_val))
        name_bytes = name_val.encode("utf-8")
        row.extend(name_bytes[:7])
        row.extend(b"\x00" * (7 - len(name_bytes)))
        row.extend(struct.pack("<d", score_val))
        rows.append(bytes(row))

    for version in [120, 121]:
        path = os.path.join(OUT_DIR, f"format_{version}.dta")
        write_dta_binary(path, version, rows, columns)


def generate_value_labels_file():
    """Generate a .dta file with value labels using pandas."""
    df_labels = pd.DataFrame(
        {
            "gender": pd.Categorical([1, 2, 1, 2, 1], categories=[1, 2]),
            "region": pd.Categorical([1, 1, 2, 3, 3], categories=[1, 2, 3]),
        }
    )
    # Convert to int for Stata
    df_out = pd.DataFrame(
        {
            "gender": df_labels["gender"].cat.codes.astype("int8") + 1,
            "region": df_labels["region"].cat.codes.astype("int8") + 1,
        }
    )
    path = os.path.join(OUT_DIR, "value_labels.dta")
    writer = pd.io.stata.StataWriter117(
        path,
        df_out,
        write_index=False,
        value_labels={
            "gender": {1: "Male", 2: "Female"},
            "region": {1: "North", 2: "South", 3: "East"},
        },
    )
    writer.write_file()
    print(f"  Written {path} (with value labels)")


def generate_strl_file():
    """Generate a format 118 .dta file with strL columns using pandas.

    In formats 118+ the 8-byte strL reference in the data section is
    v(2 bytes) + o(6 bytes), unlike format 117's v(4) + o(4), so a
    Stata-tool-written file catches layout bugs that extension-only
    roundtrip tests cannot.
    """
    df_strl = pd.DataFrame(
        {
            "id": pd.array([1, 2, 3], dtype="int32"),
            "txt": ["first strl value", "second strl value", "x" * 3000],
        }
    )
    path = os.path.join(OUT_DIR, "strl_118.dta")
    df_strl.to_stata(path, write_index=False, version=118, convert_strl=["txt"])
    print(f"  Written {path} (format 118 with strLs)")


def generate_latin1_file():
    """Generate a format 117 .dta file with Latin-1 encoded strings.

    Format 117 predates Stata's UTF-8 support; strings are stored in the
    writing machine's ANSI code page (Latin-1 here, following pandas).
    """
    df_latin1 = pd.DataFrame(
        {
            "id": pd.array([1, 2, 3], dtype="int32"),
            "name": ["café", "naïve", "plain"],
        }
    )
    path = os.path.join(OUT_DIR, "latin1_117.dta")
    writer = pd.io.stata.StataWriter117(path, df_latin1, write_index=False)
    writer.write_file()
    print(f"  Written {path} (format 117 with Latin-1 strings)")


def generate_int_date_file():
    """Generate a .dta file whose %td date columns are stored as integers.

    Stata commonly stores %td dates in int/long variables, not just double
    (pandas always writes them as double, so this file is built manually).
    """
    from datetime import date

    columns = [
        ("id", 65528, 4, "%12.0g"),  # long
        ("d_long", 65528, 4, "%td"),  # long %td
        ("d_int", 65529, 2, "%td"),  # int %td
    ]

    epoch = date(1960, 1, 1)
    dates = [date(1960, 1, 1), date(2024, 6, 15), date(1959, 12, 25)]
    rows = []
    for i, d in enumerate(dates):
        days = (d - epoch).days
        rows.append(struct.pack("<iih", i + 1, days, days))

    path = os.path.join(OUT_DIR, "int_dates.dta")
    write_dta_binary(path, 118, rows, columns)


def generate_legacy_formats():
    """Generate legacy (pre-XML) format files 114 and 115.

    pandas writes format 114; format 115 (Stata 12) has a byte-identical
    layout, so it is produced by patching the format byte on a second 114
    file, which also carries Latin-1 content to exercise transcoding.
    """
    path114 = os.path.join(OUT_DIR, "format_114.dta")
    df.to_stata(path114, write_index=False, version=114)
    print(f"  Written {path114} (format 114)")

    df_115 = pd.DataFrame(
        {
            "id": pd.array([1, 2, 3], dtype="int32"),
            "name": ["café", "naïve", "plain"],
            "score": [95.5, 87.3, 91.0],
        }
    )
    path115 = os.path.join(OUT_DIR, "format_115.dta")
    df_115.to_stata(path115, write_index=False, version=114)
    with open(path115, "r+b") as f:
        assert f.read(1) == bytes([114])
        f.seek(0)
        f.write(bytes([115]))
    print(f"  Written {path115} (format 115, patched from 114 layout)")

    # Value labels in a legacy file
    df_labels = pd.DataFrame(
        {
            "gender": pd.array([1, 2, 1, 2, 1], dtype="int8"),
            "region": pd.array([1, 1, 2, 3, 3], dtype="int8"),
        }
    )
    path_vl = os.path.join(OUT_DIR, "value_labels_114.dta")
    writer = pd.io.stata.StataWriter(
        path_vl,
        df_labels,
        write_index=False,
        value_labels={
            "gender": {1: "Male", 2: "Female"},
            "region": {1: "North", 2: "South", 3: "East"},
        },
    )
    writer.write_file()
    print(f"  Written {path_vl} (format 114 with value labels)")


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    print("Generating pandas format files (117-119)...")
    generate_pandas_formats()
    print("Generating legacy format files (114-115)...")
    generate_legacy_formats()
    print("Generating binary format files (120-121)...")
    generate_binary_formats()
    print("Generating value labels file...")
    generate_value_labels_file()
    print("Generating strL file...")
    generate_strl_file()
    print("Generating Latin-1 file...")
    generate_latin1_file()
    print("Generating integer-date file...")
    generate_int_date_file()
    print("Done!")
