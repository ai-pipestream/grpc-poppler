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
// Each rect is the widget's /Rect relative to the page's CropBox, the
// contract's page space.
//
// poppler-cpp has no forms surface, so this reads poppler's core API (the
// Form, FormWidget and Object classes of libpoppler, which poppler-cpp
// itself sits on) through a second core document opened over the same
// bytes. The field type, /Ff, /TU and the value are looked up through the
// /Parent chain; /AS is read from the widget annotation alone, keeping the
// leading slash of the PDF name, as docling-core's PdfWidget does.
//
// With document_unreadable set, the core document could not be opened and
// no field was read: the caller says so, since an empty map alone reads as
// a document without fields.
//
// A list box with several options selected stores them as an array;
// FormField.value is one string and carries the first, and each such field
// is listed in multiple_selections so the caller can say the others are
// missing.
struct FormFieldsRead {
  struct MultipleSelection {
    int page_index = 0;
    std::string field_name;
    int selected = 0;
  };
  std::map<int, std::vector<ai::protomolt::parse::pdf::v1::FormField>> fields;
  std::vector<MultipleSelection> multiple_selections;
  bool document_unreadable = false;
};

FormFieldsRead ReadFormFields(const std::string& data,
                              const std::optional<std::string>& password,
                              int begin, int end);

}  // namespace grpc_poppler
