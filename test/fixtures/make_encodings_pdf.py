#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates encodings.pdf, the string-encoding fixture: names that are
not UTF-8 in the file, which must still reach the client as valid UTF-8
(protobuf rejects a message whose string field is not, and the client then
loses the whole Parse stream), plus an attachment that inflates far past
its declared size.

  - a font whose /BaseFont is the GBK bytes of SimSun (#CB#CE#CC#E5),
    used for the page's one word "Hello"
  - a check box widget whose value and /AS state are the PDF name
    #E9tat (Latin-1 bytes, not UTF-8)
  - three embedded files:
      resume  /UF is UTF-16BE "r\\u00e9sum\\u00e9.txt", /F the Latin-1
              bytes; the stream's /Subtype is the name text#2F#E9
      cafe    only /F, "caf\\xe9.csv" in PDFDocEncoding
      zeros   a FlateDecode stream that inflates to 65536 zero bytes while
              /Params /Size claims 16

Kept as a generator so the fixture is reproducible; the checked in
encodings.pdf is the test input."""

import zlib
from pathlib import Path

ZEROS = 65536


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


def utf16be(text: str) -> bytes:
    return b"<FEFF" + text.encode("utf-16-be").hex().upper().encode() + b">"


content = b"BT /F1 24 Tf 72 700 Td (Hello) Tj ET"
widths = b" ".join(b"500" for _ in range(32, 127))

objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R] >> "
    b"/Names << /EmbeddedFiles << /Names "
    b"[(1-resume) 9 0 R (2-cafe) 11 0 R (3-zeros) 13 0 R] >> >> >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R "
    b"/Annots [6 0 R] >>",
    # 4: the font named in GBK bytes
    b"<< /Type /Font /Subtype /Type1 /BaseFont /#CB#CE#CC#E5 "
    b"/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 126 "
    b"/Widths [" + widths + b"] >>",
    # 5: content
    stream(b"", content),
    # 6: check box widget showing the #E9tat state
    b"<< /Type /Annot /Subtype /Widget /FT /Btn /T (choice) /V /#E9tat "
    b"/AS /#E9tat /Rect [72 600 87 615] /F 4 /P 3 0 R "
    b"/AP << /N << /#E9tat 7 0 R /Off 8 0 R >> >> >>",
    # 7: the on appearance
    stream(b"/Type /XObject /Subtype /Form /BBox [0 0 15 15]", b"0 g 3 3 9 9 re f"),
    # 8: the off appearance
    stream(b"/Type /XObject /Subtype /Form /BBox [0 0 15 15]", b""),
    # 9: resume file spec
    b"<< /Type /Filespec /F (r\xe9sum\xe9.txt) /UF "
    + utf16be("résumé.txt")
    + b" /EF << /F 10 0 R >> >>",
    # 10: resume bytes
    stream(b"/Type /EmbeddedFile /Subtype /text#2F#E9 /Params << /Size 11 >>",
           b"plain text\n"),
    # 11: cafe file spec
    b"<< /Type /Filespec /F (caf\xe9.csv) /EF << /F 12 0 R >> >>",
    # 12: cafe bytes
    stream(b"/Type /EmbeddedFile /Subtype /text#2Fcsv /Params << /Size 8 >>",
           b"a,b\n1,2\n"),
    # 13: zeros file spec
    b"<< /Type /Filespec /F (zeros.bin) /UF (zeros.bin) /EF << /F 14 0 R >> >>",
    # 14: zeros bytes, deflated, with a /Size that understates them
    stream(b"/Type /EmbeddedFile /Filter /FlateDecode /Params << /Size 16 >>",
           zlib.compress(b"\0" * ZEROS, 9)),
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

Path(__file__).with_name("encodings.pdf").write_bytes(bytes(out))
print(f"wrote encodings.pdf ({len(out)} bytes)")
