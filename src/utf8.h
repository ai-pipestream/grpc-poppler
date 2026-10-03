#pragma once

#include <string>
#include <string_view>

namespace grpc_poppler {

// Bytes as valid UTF-8, for the contract's proto3 string fields. Protobuf
// rejects a message whose string field is not UTF-8, so one font or
// attachment name in a legacy encoding would otherwise cost the client the
// whole Parse stream. Well-formed UTF-8 passes through unchanged; each byte
// of an ill-formed sequence is read as Latin-1, since PDF names and legacy
// byte strings are most often in a single-byte encoding.
std::string ValidUtf8(std::string_view bytes);

// A PDF text string (PDFDocEncoding, or UTF-16 behind a byte order mark)
// as UTF-8, through poppler's TextStringToUCS4. Not its TextStringToUtf8,
// which in this poppler keeps a trailing NUL inside the returned string and
// narrows PDFDocEncoding code points above 0x7F to one byte.
std::string PdfTextStringToUtf8(std::string_view text);

}  // namespace grpc_poppler
