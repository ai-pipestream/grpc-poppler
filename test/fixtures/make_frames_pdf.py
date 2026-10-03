#!/usr/bin/env python3
"""Regenerates frames.pdf, the page-frame fixture: ten Letter pages that
draw the same Helvetica 24pt word "Frame" with its baseline at (100, 700)
in PDF user space, under every /Rotate value and with offset CropBoxes, so
a backend that reports geometry in the contract's frame (user space before
/Rotate, CropBox origin included) gives every page the same box as the
upright page 0.

  page 0  /Rotate 0
  page 1  /Rotate 90
  page 2  /Rotate 180
  page 3  /Rotate 270
  page 4  /Rotate -90 (the same turn as 270)
  page 5  /CropBox [36 36 576 756]
  page 6  /Rotate 90, /CropBox [50 20 560 760], and a text field widget
          whose /Rect [90 690 260 730] surrounds the word
  page 7  /Rotate 180, /CropBox [50 20 560 760]
  page 8  /Rotate 270, /CropBox [50 20 560 760]
  page 9  /Rotate 0, the word turned a quarter counterclockwise
          (reading upward) with its baseline starting at (300, 400)

Kept as a generator so the fixture is reproducible; the checked in
frames.pdf is the test input."""

from pathlib import Path

WORD = b"BT /F1 24 Tf 100 700 Td (Frame) Tj ET"
UPWARD_WORD = b"BT /F1 24 Tf 0 1 -1 0 300 400 Tm (Frame) Tj ET"
CROP = b"/CropBox [50 20 560 760] "

# (extra page dictionary entries, content stream)
pages = [
    (b"", WORD),
    (b"/Rotate 90 ", WORD),
    (b"/Rotate 180 ", WORD),
    (b"/Rotate 270 ", WORD),
    (b"/Rotate -90 ", WORD),
    (b"/CropBox [36 36 576 756] ", WORD),
    (b"/Rotate 90 " + CROP + b"/Annots [WIDGET] ", WORD),
    (b"/Rotate 180 " + CROP, WORD),
    (b"/Rotate 270 " + CROP, WORD),
    (b"", UPWARD_WORD),
]

# Object numbers: 1 catalog, 2 pages, 3 font, 4 widget, 5 widget
# appearance, then a page object and its content stream per page.
FIRST_PAGE = 6
page_refs = [FIRST_PAGE + 2 * i for i in range(len(pages))]
widget_page = page_refs[6]


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] >> >>",
    # 2: pages
    b"<< /Type /Pages /Kids ["
    + b" ".join(str(ref).encode() + b" 0 R" for ref in page_refs)
    + b"] /Count " + str(len(pages)).encode() + b" >>",
    # 3: Helvetica
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 4: text field widget around the word on page 6, with an empty
    # appearance so it adds no text of its own
    b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (frame_field) "
    b"/Rect [90 690 260 730] /F 4 /P " + str(widget_page).encode() + b" 0 R "
    b"/AP << /N 5 0 R >> >>",
    # 5: the empty appearance
    stream(b"/Type /XObject /Subtype /Form /BBox [0 0 170 40]", b""),
]
for i, (extra, content) in enumerate(pages):
    content_ref = page_refs[i] + 1
    page = (
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] " + extra
        + b"/Resources << /Font << /F1 3 0 R >> >> /Contents "
        + str(content_ref).encode() + b" 0 R >>"
    )
    objects.append(page.replace(b"[WIDGET]", b"[4 0 R]"))
    objects.append(stream(b"", content))

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

Path(__file__).with_name("frames.pdf").write_bytes(bytes(out))
print(f"wrote frames.pdf ({len(out)} bytes, {len(pages)} pages)")
