#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates damaged_content.pdf, the damaged-page-content fixture: a
document that loads cleanly and whose pages poppler can only partly read.
poppler reports that through its error callback alone, so Parse must carry
what it said in the trailer rather than hand back the part it read as a
whole page.

  - page 0 draws "Clean" and has nothing wrong with it
  - page 1 draws "Before", then runs 200 times into an operator that does
    not exist (one message, many times over), then draws "After"
  - page 2 says its content is Flate data and holds bytes that are not

Kept as a generator so the fixture is reproducible; the checked in
damaged_content.pdf is the test input."""

from pathlib import Path


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


def page(contents: int) -> bytes:
    return (
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 6 0 R >> >> /Contents "
        + str(contents).encode()
        + b" 0 R >>"
    )


clean = b"BT /F1 24 Tf 72 700 Td (Clean) Tj ET"
broken_operators = (
    b"BT /F1 24 Tf 72 700 Td (Before) Tj ET\n"
    + b"1 2 3 bogusop\n" * 200
    + b"BT /F1 24 Tf 72 600 Td (After) Tj ET"
)
not_flate = b"this is not flate data, whatever the dictionary says " * 4

objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
    # 3, 4, 5: the pages
    page(7),
    page(8),
    page(9),
    # 6: font
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 7: clean content
    stream(b"", clean),
    # 8: content with an operator poppler does not know, 200 times
    stream(b"", broken_operators),
    # 9: content that claims to be Flate data
    stream(b"/Filter /FlateDecode", not_flate),
]

out = bytearray(b"%PDF-1.7\n")
offsets = []
for i, body in enumerate(objects, start=1):
    offsets.append(len(out))
    out += str(i).encode() + b" 0 obj\n" + body + b"\nendobj\n"

xref_pos = len(out)
out += b"xref\n0 " + str(len(objects) + 1).encode() + b"\n"
out += b"0000000000 65535 f \n"
for off in offsets:
    out += f"{off:010d} 00000 n \n".encode()
out += (
    b"trailer\n<< /Size " + str(len(objects) + 1).encode() + b" /Root 1 0 R >>\n"
    b"startxref\n" + str(xref_pos).encode() + b"\n%%EOF\n"
)

Path(__file__).with_name("damaged_content.pdf").write_bytes(bytes(out))
