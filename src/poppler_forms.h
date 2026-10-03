// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

namespace grpc_poppler {

// The AcroForm widgets of pages [begin, end), keyed by zero-based page
// index, one FormField per widget annotation in the page's /Annots order.
//
// poppler-cpp has no forms surface, so this reads poppler's core API (the
// Form, FormWidget and Object classes of libpoppler, which poppler-cpp
// itself sits on) through a second core document opened over the same
// bytes. The field type, /Ff, /TU and the value are looked up through the
// /Parent chain; /AS is read from the widget annotation alone, keeping the
// leading slash of the PDF name, as docling-core's PdfWidget does.
//
// An empty map means no page in the range has a widget or the core
// document could not be opened (the caller already loaded the document
// through poppler-cpp, so the second open only fails on the same bytes for
// reasons that would have failed the first).
std::map<int, std::vector<ai::protomolt::parse::pdf::v1::FormField>>
ReadFormFields(const std::string& data, const std::optional<std::string>& password,
               int begin, int end);

}  // namespace grpc_poppler
