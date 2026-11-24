// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

namespace grpc_poppler {

// The encryption dictionary as stored, for EncryptionInfo's raw fields.
struct EncryptionFacts {
  // /Filter, the security handler, without its slash.
  std::optional<std::string> filter;
  // /R, the handler revision.
  std::optional<uint32_t> revision;
  // The length of the file key poppler derived, in bits.
  std::optional<uint32_t> key_length_bits;
  // /P as an unsigned 32-bit field.
  std::optional<uint32_t> permissions_raw;
};

// What poppler-cpp does not expose and poppler's core API does.
struct DocumentFacts {
  // False when the core API could not open the bytes; nothing else is set
  // then, and the caller says so instead of reporting defaults.
  bool readable = false;
  // The catalog's /MarkInfo /Marked.
  bool tagged = false;
  // Set for an encrypted document.
  std::optional<EncryptionFacts> encryption;
  // The info dictionary's /Trapped as stored (a name, without its slash).
  std::optional<std::string> trapped;
  // The info dictionary's keys outside the standard set, with text, name,
  // number and boolean values as text.
  std::vector<std::pair<std::string, std::string>> custom_info;
  // Info keys whose value is none of those and so has no place in the
  // contract.
  std::vector<std::string> info_not_carried;
  // The outline with each item's target: a page destination (explicit or
  // named, resolved to a page index, its position relative to that page's
  // CropBox) or a URI. Filled when asked for.
  std::vector<ai::protomolt::parse::pdf::v1::OutlineNode> outline;
  // Every node in outline, children included.
  uint64_t outline_nodes = 0;
  // Items nested deeper than kMaxOutlineDepth were left out.
  bool outline_cut = false;
};

// The deepest outline level reported; an outline is rarely deeper than a
// handful, and a hostile one must not exhaust the stack.
inline constexpr int kMaxOutlineDepth = 64;

// Reads them through a second core document opened over the same bytes,
// as the form field and attachment readers do (poppler-cpp keeps its own
// core document private).
DocumentFacts ReadDocumentFacts(const std::string& data,
                                const std::optional<std::string>& password,
                                bool want_outline);

}  // namespace grpc_poppler
