// SPDX-License-Identifier: GPL-3.0-or-later

#include "poppler_render.h"

#include <memory>

// poppler's core headers, installed by its ENABLE_UNSTABLE_API_ABI_HEADERS
// option; poppler.pc puts <includedir>/poppler on the include path.
#include <Object.h>
#include <PDFDoc.h>
#include <SplashOutputDev.h>
#include <Stream.h>
#include <goo/GooString.h>
#include <splash/SplashBitmap.h>
#include <splash/SplashTypes.h>

#include "poppler_errors.h"

namespace grpc_poppler {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

bool NoAnnotation(Annot* /*annot*/, void* /*user_data*/) { return false; }

}  // namespace

CoreRender RenderWithoutAnnotations(const std::string& data,
                                    const std::optional<std::string>& password,
                                    int page_index, double dpi, uint32_t paper,
                                    pdfv1::PageRaster* raster) {
  // Keeps globalParams alive for the core document, and sends what poppler
  // reports to the caller's PopplerErrorScope (src/poppler_errors.h).
  EnsurePopplerErrorCapture();
  std::optional<GooString> owner;
  std::optional<GooString> user;
  if (password.has_value()) {
    owner.emplace(*password);
    user.emplace(*password);
  }
  // MemStream borrows the bytes; `data` outlives the document.
  PDFDoc doc(std::make_unique<MemStream>(data.data(), 0,
                                         static_cast<Goffset>(data.size()),
                                         Object::null()),
             owner, user);
  if (!doc.isOk()) return CoreRender::kDocumentUnreadable;
  if (page_index < 0 || page_index >= doc.getNumPages()) {
    return CoreRender::kFailed;
  }

  // As poppler-cpp's page_renderer::render_page sets splash up for
  // format_bgr24 with its default hints and line mode.
  SplashColor paper_color;
  paper_color[0] = static_cast<unsigned char>(paper & 0xff);
  paper_color[1] = static_cast<unsigned char>((paper >> 8) & 0xff);
  paper_color[2] = static_cast<unsigned char>((paper >> 16) & 0xff);
  SplashOutputDev splash(splashModeBGR8, 4, paper_color, true,
                         splashThinLineDefault);
  splash.setFontAntialias(false);
  splash.setVectorAntialias(false);
  splash.setFreeTypeHinting(false, false);
  splash.startDoc(&doc);
  doc.displayPageSlice(&splash, page_index + 1, dpi, dpi, 0, false, true,
                       false, -1, -1, -1, -1, nullptr, nullptr, &NoAnnotation,
                       nullptr, true);

  SplashBitmap* bitmap = splash.getBitmap();
  if (bitmap == nullptr || bitmap->getDataPtr() == nullptr ||
      bitmap->getWidth() <= 0 || bitmap->getHeight() <= 0) {
    return CoreRender::kFailed;
  }
  const auto width = static_cast<uint32_t>(bitmap->getWidth());
  const auto height = static_cast<uint32_t>(bitmap->getHeight());
  const auto stride = static_cast<uint32_t>(bitmap->getRowSize());
  raster->set_page_index(static_cast<uint32_t>(page_index));
  raster->set_width_px(width);
  raster->set_height_px(height);
  raster->set_stride_bytes(stride);
  raster->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
  raster->set_dpi(dpi);
  raster->set_pixels(bitmap->getDataPtr(),
                     static_cast<size_t>(stride) * height);
  return CoreRender::kRendered;
}

}  // namespace grpc_poppler
