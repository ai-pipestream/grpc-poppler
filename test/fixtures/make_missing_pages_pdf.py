#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates missing_pages.pdf, the unloadable-page fixture: a page tree
whose /Count says 3 while it holds one page. poppler reports three pages
and cannot load pages 1 and 2, which Render must fail on and Parse must
warn about rather than leave out without a word.

  - page 0 draws "Hello"
  - pages 1 and 2 exist only in /Count; two spare objects keep the count
    under the number of objects, so poppler keeps it

Kept as a generator so the fixture is reproducible; the checked in
missing_pages.pdf is the test input."""

from pathlib import Path


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


content = b"BT /F1 24 Tf 72 700 Td (Hello) Tj ET"

objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R >>",
    # 2: pages, counting two pages it does not hold
    b"<< /Type /Pages /Kids [3 0 R] /Count 3 >>",
    # 3: the one page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    # 4: font
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 5: content
    stream(b"", content),
    # 6, 7: spare objects
    b"<< >>",
    b"<< >>",
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

Path(__file__).with_name("missing_pages.pdf").write_bytes(bytes(out))
print(f"wrote missing_pages.pdf ({len(out)} bytes)")
