// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"

namespace grpc_poppler {

enum class CoreRender {
  // raster holds the page.
  kRendered,
  // poppler's core API could not open the document.
  kDocumentUnreadable,
  // The document has no such page, or splash produced no bitmap.
  kFailed,
};

// Renders one page as BGR8 without its annotations: the content stream
// alone, no markup, no form widgets. poppler-cpp's page_renderer always
// draws annotations, so this goes through poppler's core API with a second
// core document over the same bytes and the same splash set-up
// page_renderer uses (BGR8, rows padded to four bytes, no antialiasing
// hints), which keeps the content pixel for pixel what Render gives with
// annotations left in. paper is the page's background as 0xRRGGBB.
CoreRender RenderWithoutAnnotations(
    const std::string& data, const std::optional<std::string>& password,
    int page_index, double dpi, uint32_t paper,
    ai::protomolt::parse::pdf::v1::PageRaster* raster);

}  // namespace grpc_poppler
