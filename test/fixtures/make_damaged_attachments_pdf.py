#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates damaged_attachments.pdf, the fixture for attachments whose
data cannot be had whole. Asked for their data, each must still be listed
and the trailer must say what went wrong, rather than the attachment
arriving with empty or missing data and no word.

  - garbage.bin  a FlateDecode stream of bytes that are not deflate data,
                 declaring /Params /Size 5; poppler decodes nothing
  - lost.txt     a file spec whose /EF /F names an object the file does
                 not hold, so there is no embedded stream to read

Kept as a generator so the fixture is reproducible; the checked in
damaged_attachments.pdf is the test input."""

from pathlib import Path


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R "
    b"/Names << /EmbeddedFiles << /Names "
    b"[(1-garbage) 4 0 R (2-lost) 6 0 R] >> >> >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: an empty page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
    # 4: garbage file spec
    b"<< /Type /Filespec /F (garbage.bin) /EF << /F 5 0 R >> >>",
    # 5: garbage bytes under a deflate filter
    stream(b"/Type /EmbeddedFile /Filter /FlateDecode /Params << /Size 5 >>",
           b"not deflate data"),
    # 6: lost file spec; object 99 does not exist
    b"<< /Type /Filespec /F (lost.txt) /EF << /F 99 0 R >> >>",
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

Path(__file__).with_name("damaged_attachments.pdf").write_bytes(bytes(out))
print(f"wrote damaged_attachments.pdf ({len(out)} bytes)")
