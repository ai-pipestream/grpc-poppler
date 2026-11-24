#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates encrypted.pdf, the password fixture: one page drawing
"Sealed", encrypted with the Standard security handler (V 1, R 2, 40-bit
RC4), which poppler implements.

  - user password "user", owner password "owner"
  - every permission granted (/P -4)

The load verdicts read it three ways: without a password
(PASSWORD_REQUIRED), with a wrong one (PASSWORD_INCORRECT) and with the
user password (OK, and the text decrypts).

Kept as a generator so the fixture is reproducible; the checked in
encrypted.pdf is the test input."""

import hashlib
from pathlib import Path

PAD = bytes.fromhex(
    "28bf4e5e4e758a4164004e56fffa0108"
    "2e2e00b6d0683e802f0ca9fe6453697a"
)
USER = b"user"
OWNER = b"owner"
PERMISSIONS = -4
FILE_ID = bytes(range(16))


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for byte in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(byte ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def pad(password: bytes) -> bytes:
    return (password + PAD)[:32]


# ISO 32000-1 algorithm 3: the /O entry.
owner_entry = rc4(hashlib.md5(pad(OWNER)).digest()[:5], pad(USER))
# Algorithm 2: the file key.
file_key = hashlib.md5(
    pad(USER)
    + owner_entry
    + (PERMISSIONS & 0xFFFFFFFF).to_bytes(4, "little")
    + FILE_ID
).digest()[:5]
# Algorithm 4: the /U entry.
user_entry = rc4(file_key, PAD)


def encrypted(number: int, data: bytes) -> bytes:
    """Algorithm 1: the object's own key, then RC4."""
    key = hashlib.md5(
        file_key + number.to_bytes(3, "little") + (0).to_bytes(2, "little")
    ).digest()[: len(file_key) + 5]
    return rc4(key, data)


def hex_string(data: bytes) -> bytes:
    return b"<" + data.hex().encode() + b">"


def stream(number: int, data: bytes) -> bytes:
    data = encrypted(number, data)
    return (
        b"<< /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: the page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    # 4: font
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 5: content
    stream(5, b"BT /F1 24 Tf 72 700 Td (Sealed) Tj ET"),
    # 6: the encryption dictionary (its strings are never encrypted)
    b"<< /Filter /Standard /V 1 /R 2 /P "
    + str(PERMISSIONS).encode()
    + b" /O "
    + hex_string(owner_entry)
    + b" /U "
    + hex_string(user_entry)
    + b" >>",
]

out = bytearray(b"%PDF-1.4\n")
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
    b"trailer\n<< /Size "
    + str(len(objects) + 1).encode()
    + b" /Root 1 0 R /Encrypt 6 0 R /ID ["
    + hex_string(FILE_ID)
    + b" "
    + hex_string(FILE_ID)
    + b"] >>\n"
    b"startxref\n" + str(xref_pos).encode() + b"\n%%EOF\n"
)

Path(__file__).with_name("encrypted.pdf").write_bytes(bytes(out))
