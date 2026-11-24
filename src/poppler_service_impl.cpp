// SPDX-License-Identifier: GPL-3.0-or-later

#include "poppler_service_impl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-font.h>
#include <poppler/cpp/poppler-image.h>
#include <poppler/cpp/poppler-page-renderer.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-toc.h>
#include <poppler/cpp/poppler-version.h>

#include "document_cache.h"
#include "poppler_attachments.h"
#include "poppler_document_facts.h"
#include "poppler_errors.h"
#include "poppler_forms.h"
#include "poppler_load.h"
#include "poppler_render.h"
#include "sha256.h"
#include "utf8.h"

namespace grpc_poppler {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

constexpr char kBackendName[] = "grpc-poppler";

// Mirrors gRParse's arm64 serialization gate: poppler calls crash under
// concurrency on arm64, so they run one at a time there and concurrently
// elsewhere. A handler holds the gate from construction to return, and
// releases it only around a network write (WriteUngated), when no poppler
// code runs: a client that reads slowly must not hold every other
// request's poppler work behind it. Poppler objects outlive a release but
// are only used, and destroyed, with the gate held, so the gate is
// declared before them.
#if defined(__aarch64__)
class PopplerGate {
 public:
  PopplerGate() : lock_(Mutex()) {}

  void Release() { lock_.unlock(); }
  void Reacquire() { lock_.lock(); }

 private:
  static std::mutex& Mutex() {
    static std::mutex m;
    return m;
  }
  std::unique_lock<std::mutex> lock_;
};
#else
class PopplerGate {
 public:
  void Release() {}
  void Reacquire() {}
};
#endif

// Writes one stream message with the gate released for the duration of
// the write, which blocks for as long as the client takes to read.
template <typename Message>
bool WriteUngated(PopplerGate& gate, MessageSink<Message>* writer,
                  const Message& message) {
  gate.Release();
  const bool written = writer->Write(message);
  gate.Reacquire();
  return written;
}

std::string ToUtf8(const poppler::ustring& s) {
  poppler::byte_array bytes = s.to_utf8();
  return std::string(bytes.begin(), bytes.end());
}

bool FamilySupported(pdfv1::PdfFamily family) {
  switch (family) {
    case pdfv1::PDF_FAMILY_PAGE_INVENTORY:
    case pdfv1::PDF_FAMILY_TEXT_CELLS:
    case pdfv1::PDF_FAMILY_PAGE_RASTER:
    case pdfv1::PDF_FAMILY_FONTS:
    case pdfv1::PDF_FAMILY_DOC_METADATA:
    case pdfv1::PDF_FAMILY_ENCRYPTION_INFO:
    case pdfv1::PDF_FAMILY_OUTLINE:
    case pdfv1::PDF_FAMILY_ATTACHMENTS:
    case pdfv1::PDF_FAMILY_FORM_FIELDS:
      return true;
    default:
      return false;
  }
}

const char* UnsupportedDetail(pdfv1::PdfFamily family) {
  switch (family) {
    case pdfv1::PDF_FAMILY_ANNOTATIONS:
    case pdfv1::PDF_FAMILY_STRUCT_TREE:
      return "reachable through poppler's glib surface, not the cpp wrapper "
             "this build links; planned";
    case pdfv1::PDF_FAMILY_DEEP_RESOURCES:
      return "poppler does not type out page resources";
    default:
      return "not exposed by the poppler-cpp surface";
  }
}

struct LoadedDocument {
  std::unique_ptr<poppler::document> doc;
  pdfv1::LoadStatus status = pdfv1::LOAD_STATUS_UNSPECIFIED;
  std::string detail;
};

// The content-addressed handshake (PdfDocument.sha256), resolved once for
// all three RPCs. bytes is set when status is LOAD_STATUS_OK and points at
// either the request or a cache entry (cached keeps the entry alive).
struct ResolvedBytes {
  pdfv1::LoadStatus status = pdfv1::LOAD_STATUS_OK;
  std::string detail;
  const std::string* bytes = nullptr;
  std::shared_ptr<const std::string> cached;
  bool invalid_argument = false;
};

ResolvedBytes ResolveDocumentBytes(const pdfv1::PdfDocument& document,
                                   DocumentCache& cache) {
  ResolvedBytes out;
  if (document.data().empty()) {
    if (!document.has_sha256()) {
      out.invalid_argument = true;
      out.detail = "data is empty and sha256 is absent";
      return out;
    }
    out.cached = cache.Lookup(document.sha256());
    if (out.cached == nullptr) {
      out.status = pdfv1::LOAD_STATUS_BYTES_REQUIRED;
      out.detail = "no cached bytes for sha256 " + document.sha256();
      return out;
    }
    out.bytes = out.cached.get();
    return out;
  }
  out.bytes = &document.data();
  if (!document.has_sha256()) return out;
  if (Sha256Hex(document.data()) != document.sha256()) {
    out.status = pdfv1::LOAD_STATUS_HASH_MISMATCH;
    out.detail = "data does not hash to the given sha256";
    out.bytes = nullptr;
    return out;
  }
  cache.Insert(document.sha256(), document.data());
  return out;
}

// The largest ParseStream upload accepted: the document cache's default
// byte ceiling. Unlike a unary request, a stream is not held to the
// 520 MiB message limit, which bounds each chunk instead.
constexpr uint64_t kMaxStreamDocumentBytes = 2ull * 1024 * 1024 * 1024;

// How many of one ParseStream's requests run at once. Each loads its own
// poppler document over the shared bytes (the arm64 gate still serializes
// poppler code), so a client rendering a few pages in parallel keeps the
// parallelism it had with separate Render calls.
constexpr unsigned kStreamParallelRequests = 3;

// A ParseStream's upload: complete, and checked against the open's sha256
// by the session already.
ResolvedBytes StreamBytes(const std::string* bytes) {
  ResolvedBytes out;
  out.bytes = bytes;
  return out;
}

// The password a request opens its document with, for the readers that
// open a second, core document over the same bytes.
std::optional<std::string> DocumentPassword(
    const pdfv1::PdfDocument& document) {
  if (!document.has_password()) return std::nullopt;
  return document.password();
}

// Loads the document, or says why it does not load. poppler-cpp tells
// only "not opened" from "opened but locked", so the verdict for a
// failure comes from poppler's core error code for the same bytes
// (src/poppler_load.h) and from what its security handler reported
// (errors holds what poppler said during the load):
//
//   damaged file, unreadable catalog       CORRUPT, password or not
//   encrypted, no usable security handler  UNSUPPORTED_ENCRYPTION, password
//                                          or not: no password opens it
//                                          (poppler itself opens some of
//                                          these as if unencrypted)
//   encrypted, handler refused             PASSWORD_REQUIRED without a
//                                          password, PASSWORD_INCORRECT
//                                          with one
//   anything else                          ENGINE_ERROR
//
// load_detail carries the code and poppler's message.
void LoadDocument(const std::string& data, const pdfv1::PdfDocument& request,
                  const PopplerErrorScope& errors, LoadedDocument* out) {
  if (data.rfind("%PDF-", 0) != 0) {
    out->status = pdfv1::LOAD_STATUS_NOT_PDF;
    out->detail = "missing %PDF- header";
    return;
  }
  std::string owner;
  std::string user;
  if (request.has_password()) {
    owner = request.password();
    user = request.password();
  }
  out->doc.reset(poppler::document::load_from_raw_data(data.data(),
                                                       data.size(), owner,
                                                       user));
  // Every way poppler's SecurityHandler fails to set itself up
  // (poppler/SecurityHandler.cc at the pinned version): no handler by that
  // /Filter name, or a Standard handler dictionary it cannot use. A handler
  // that set itself up and refused the password reports "Incorrect
  // password" alone.
  const std::string* no_handler = errors.Find(
      {"Couldn't find the '", "Missing or invalid 'Filter' entry",
       "Unsupported version/revision", "Invalid encryption key length",
       "Weird encryption"});
  if (out->doc != nullptr && !out->doc->is_locked()) {
    if (no_handler == nullptr) {
      out->status = pdfv1::LOAD_STATUS_OK;
      return;
    }
    // poppler opens a document whose Standard security handler it could
    // not set up as if it were not encrypted, and would hand out the
    // encrypted strings and streams as they are.
    out->doc.reset();
    out->status = pdfv1::LOAD_STATUS_UNSUPPORTED_ENCRYPTION;
    out->detail =
        "poppler could not set up the document's security handler and "
        "would read the document undecrypted: " +
        *no_handler;
    return;
  }
  out->doc.reset();
  const int code = CoreLoadErrorCode(data, DocumentPassword(request));
  const std::string coded = std::string("poppler error code ") +
                            std::to_string(code) + ", " +
                            CoreLoadErrorWords(code);
  const auto with_message = [&coded](const std::string* message) {
    return message != nullptr ? coded + ": " + *message : coded;
  };
  switch (code) {
    case kCoreErrorEncrypted: {
      if (no_handler != nullptr) {
        out->status = pdfv1::LOAD_STATUS_UNSUPPORTED_ENCRYPTION;
        out->detail = with_message(no_handler);
      } else {
        out->status = request.has_password()
                          ? pdfv1::LOAD_STATUS_PASSWORD_INCORRECT
                          : pdfv1::LOAD_STATUS_PASSWORD_REQUIRED;
        out->detail = coded;
      }
      return;
    }
    case kCoreErrorBadCatalog:
    case kCoreErrorDamaged:
      out->status = pdfv1::LOAD_STATUS_CORRUPT;
      out->detail = with_message(errors.Find({}));
      return;
    case kCoreErrorNone:
      out->status = pdfv1::LOAD_STATUS_ENGINE_ERROR;
      out->detail =
          "poppler-cpp did not open a document poppler's core API opens";
      return;
    default:
      out->status = pdfv1::LOAD_STATUS_ENGINE_ERROR;
      out->detail = with_message(errors.Find({}));
      return;
  }
}

// A number for an error message: 1200 rather than 1200.000000.
std::string FormatNumber(double value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

// The one engine identity string, shared by Probe capabilities and
// GetServiceInfo so an orchestrator sees the same value either way.
std::string EngineVersion() {
  return std::string("poppler ") + poppler::version_string();
}

void FillCapabilities(const LoadedDocument& loaded,
                      pdfv1::BackendCapabilities* caps) {
  caps->set_backend_name(kBackendName);
  caps->set_engine_version(EngineVersion());
  caps->set_load_status(loaded.status);
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    if (!loaded.detail.empty()) caps->set_load_detail(loaded.detail);
    return;
  }
  caps->set_page_count(static_cast<uint32_t>(loaded.doc->pages()));
  std::unique_ptr<poppler::toc> toc(loaded.doc->create_toc());
  const bool has_outline = toc != nullptr && toc->root() != nullptr &&
                           !toc->root()->children().empty();
  const bool has_attachments = loaded.doc->has_embedded_files();
  const bool encrypted = loaded.doc->is_encrypted();
  for (int f = pdfv1::PdfFamily_MIN + 1; f <= pdfv1::PdfFamily_MAX; ++f) {
    if (!pdfv1::PdfFamily_IsValid(f)) continue;
    auto family = static_cast<pdfv1::PdfFamily>(f);
    auto* verdict = caps->add_families();
    verdict->set_family(family);
    if (!FamilySupported(family)) {
      verdict->set_support(pdfv1::FAMILY_SUPPORT_UNSUPPORTED_BY_BACKEND);
      verdict->set_detail(UnsupportedDetail(family));
      continue;
    }
    bool absent = (family == pdfv1::PDF_FAMILY_OUTLINE && !has_outline) ||
                  (family == pdfv1::PDF_FAMILY_ATTACHMENTS && !has_attachments) ||
                  (family == pdfv1::PDF_FAMILY_ENCRYPTION_INFO && !encrypted);
    verdict->set_support(absent ? pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT
                                : pdfv1::FAMILY_SUPPORT_SUPPORTED);
  }
}

bool WantFamily(const pdfv1::ParseRequest& request, pdfv1::PdfFamily family) {
  if (request.families().empty()) return true;
  return std::find(request.families().begin(), request.families().end(),
                   family) != request.families().end();
}

// Pages [begin, end) of a request, as engine page indexes.
struct PageSpan {
  int begin = 0;
  int end = 0;
};

// The contract's PageRange is zero-based and half-open, and a set range
// must have end greater than begin; that is its only rule. Any other uint32
// range is valid, and SelectPages clamps it to the document.
grpc::Status CheckPageRange(const pdfv1::PageRange& range) {
  if (range.end() <= range.begin()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "page range end must be greater than begin");
  }
  return grpc::Status::OK;
}

// The pages a checked range selects, clamped to the document in unsigned
// space, so a begin of 2^31 or more selects no page rather than turning
// into a negative index; unset selects every page.
PageSpan SelectPages(bool has_range, const pdfv1::PageRange& range,
                     int page_count) {
  const uint64_t count = page_count > 0 ? static_cast<uint64_t>(page_count) : 0;
  if (!has_range) return {0, static_cast<int>(count)};
  const uint64_t begin = std::min<uint64_t>(range.begin(), count);
  const uint64_t end = std::max(begin, std::min<uint64_t>(range.end(), count));
  return {static_cast<int>(begin), static_cast<int>(end)};
}

bool IsQuarterTurn(const poppler::page& page) {
  const auto orientation = page.orientation();
  return orientation == poppler::page::landscape ||
         orientation == poppler::page::seascape;
}

// The page's /Rotate as the contract reports it: 0, 90, 180 or 270 degrees
// clockwise. page::orientation() reads the /Rotate value poppler folded
// into [0, 360); poppler draws any other value upright, so it reports 0.
int RotationDegrees(const poppler::page& page) {
  switch (page.orientation()) {
    case poppler::page::landscape:
      return 90;
    case poppler::page::upside_down:
      return 180;
    case poppler::page::seascape:
      return 270;
    default:
      return 0;
  }
}

// Maps poppler-cpp's text_list() boxes into the contract's page space.
//
// text_list() lays the page out the way a viewer shows it: 72 DPI, the
// page's /Rotate applied (Page::createGfx adds getRotate() to the rotation
// it is asked for), the CropBox as the device box, origin top-left
// (GfxState's CTM for an upside-down output device). A user-space point
// (x, y) on a page whose CropBox is [cx1 cy1 cx2 cy2] lands at
//
//   /Rotate   device u   device v
//   0         x - cx1    cy2 - y
//   90        y - cy1    x - cx1
//   180       cx2 - x    y - cy1
//   270       cy2 - y    cx2 - x
//
// The contract's page space is PDF user space before /Rotate, origin
// bottom-left, shifted so the CropBox's bottom-left corner is (0, 0)
// (PageInfo.page_space = PAGE_SPACE_CROP_BOX). TextFrame inverts the table
// and subtracts (cx1, cy1).
class TextFrame {
 public:
  explicit TextFrame(const poppler::page& page)
      : rotation_(RotationDegrees(page)),
        crop_(page.page_rect(poppler::crop_box)) {}

  // Sets the box and the quad of one text_list() box. The quad starts at
  // the word's lower-left corner in its own reading frame. text_box's
  // rotation() is the quarter turn of the word's baseline in the device
  // frame (0 reads left to right, 1 down the page, 2 right to left upside
  // down, 3 up the page), which says which device edge is the word's
  // lower edge and where its reading starts.
  void Place(const poppler::text_box& box, pdfv1::BoundingBox* bbox,
             pdfv1::Quad* quad) const {
    // The device box; v grows downward, so rectf's top() is the smaller v.
    const poppler::rectf device = box.bbox();
    const double u0 = device.left();
    const double v0 = device.top();
    const double u1 = device.right();
    const double v1 = device.bottom();
    // Lower-left, lower-right, upper-right, upper-left, in device space.
    std::array<std::array<double, 2>, 4> corners;
    switch (box.rotation()) {
      case 1:
        corners = {{{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}}};
        break;
      case 2:
        corners = {{{u1, v0}, {u0, v0}, {u0, v1}, {u1, v1}}};
        break;
      case 3:
        corners = {{{u1, v1}, {u1, v0}, {u0, v0}, {u0, v1}}};
        break;
      default:
        corners = {{{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}}};
        break;
    }
    for (auto& corner : corners) corner = ToPage(corner[0], corner[1]);
    quad->set_x0(corners[0][0]);
    quad->set_y0(corners[0][1]);
    quad->set_x1(corners[1][0]);
    quad->set_y1(corners[1][1]);
    quad->set_x2(corners[2][0]);
    quad->set_y2(corners[2][1]);
    quad->set_x3(corners[3][0]);
    quad->set_y3(corners[3][1]);
    // The lower-left and upper-right corners are opposite corners of the
    // axis-aligned box.
    bbox->set_x0(std::min(corners[0][0], corners[2][0]));
    bbox->set_y0(std::min(corners[0][1], corners[2][1]));
    bbox->set_x1(std::max(corners[0][0], corners[2][0]));
    bbox->set_y1(std::max(corners[0][1], corners[2][1]));
  }

 private:
  // A device point in contract page space, relative to the CropBox: the
  // table above less (cx1, cy1), with w and h the CropBox's width and
  // height before /Rotate.
  std::array<double, 2> ToPage(double u, double v) const {
    const double w = crop_.width();
    const double h = crop_.height();
    switch (rotation_) {
      case 90:
        return {v, u};
      case 180:
        return {w - u, v};
      case 270:
        return {w - v, h - u};
      default:
        return {u, h - v};
    }
  }

  int rotation_;
  poppler::rectf crop_;
};

// Assigns stable ids to font names within one stream.
class FontInterner {
 public:
  uint32_t Intern(const std::string& name, bool* is_new) {
    auto it = ids_.find(name);
    if (it != ids_.end()) {
      *is_new = false;
      return it->second;
    }
    uint32_t id = static_cast<uint32_t>(ids_.size());
    ids_.emplace(name, id);
    *is_new = true;
    return id;
  }

 private:
  std::map<std::string, uint32_t> ids_;
};

pdfv1::FontKind MapFontKind(poppler::font_info::type_enum type) {
  switch (type) {
    case poppler::font_info::type1:
    case poppler::font_info::type1c:
    case poppler::font_info::type1c_ot:
      return pdfv1::FONT_KIND_TYPE1;
    case poppler::font_info::type3:
      return pdfv1::FONT_KIND_TYPE3;
    case poppler::font_info::truetype:
    case poppler::font_info::truetype_ot:
      return pdfv1::FONT_KIND_TRUETYPE;
    case poppler::font_info::cid_type0:
    case poppler::font_info::cid_type0c:
    case poppler::font_info::cid_type0c_ot:
    case poppler::font_info::cid_truetype:
    case poppler::font_info::cid_truetype_ot:
      return pdfv1::FONT_KIND_TYPE0;
    default:
      return pdfv1::FONT_KIND_UNSPECIFIED;
  }
}

// The outline from the cpp wrapper, titles only: what is left when the
// core API cannot open the document (src/poppler_document_facts.h reads
// the outline with its targets). *cut is set when items nested deeper than
// kMaxOutlineDepth were left out.
void FillOutlineItem(const poppler::toc_item* item, pdfv1::OutlineNode* node,
                     int depth, uint64_t* count, bool* cut) {
  node->set_title(ToUtf8(item->title()));
  ++*count;
  if (depth + 1 >= kMaxOutlineDepth) {
    if (!item->children().empty()) *cut = true;
    return;
  }
  for (const poppler::toc_item* child : item->children()) {
    FillOutlineItem(child, node->add_children(), depth + 1, count, cut);
  }
}

}  // namespace

grpc::Status PopplerServiceImpl::Probe(grpc::ServerContext* /*context*/,
                                       const pdfv1::ProbeRequest* request,
                                       pdfv1::ProbeResponse* response) {
  const ResolvedBytes resolved = ResolveDocumentBytes(request->document(), cache_);
  if (resolved.invalid_argument) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, resolved.detail);
  }
  PopplerGate gate;
  // BackendCapabilities has no place for warnings; what poppler says while
  // probing goes to the log when this closes.
  PopplerErrorScope engine_errors;
  LoadedDocument loaded;
  if (resolved.status == pdfv1::LOAD_STATUS_OK) {
    LoadDocument(*resolved.bytes, request->document(), engine_errors,
                 &loaded);
  } else {
    loaded.status = resolved.status;
    loaded.detail = resolved.detail;
  }
  FillCapabilities(loaded, response->mutable_capabilities());
  return grpc::Status::OK;
}

grpc::Status PopplerServiceImpl::Parse(
    grpc::ServerContext* context, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  ServerWriterSink<pdfv1::ParseResponse> sink(writer);
  return ParseTo(context, request, nullptr, &sink);
}

grpc::Status PopplerServiceImpl::ParseTo(
    grpc::ServerContext* context, const pdfv1::ParseRequest* request,
    const std::string* stream_bytes,
    MessageSink<pdfv1::ParseResponse>* writer) {
  if (request->has_pages()) {
    if (grpc::Status range = CheckPageRange(request->pages()); !range.ok()) {
      return range;
    }
  }
  const ResolvedBytes resolved =
      stream_bytes != nullptr ? StreamBytes(stream_bytes)
                              : ResolveDocumentBytes(request->document(), cache_);
  if (resolved.invalid_argument) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, resolved.detail);
  }
  PopplerGate gate;
  // What poppler reports through its error callback while this request
  // runs. Each piece of work below is followed by a Take that moves what
  // was said during it into the trailer's warnings, on the page and the
  // family the work was for. Declared before the poppler objects, so it
  // outlives them.
  PopplerErrorScope engine_errors;
  std::vector<pdfv1::ParseWarning> warnings;
  LoadedDocument loaded;
  if (resolved.status == pdfv1::LOAD_STATUS_OK) {
    LoadDocument(*resolved.bytes, request->document(), engine_errors,
                 &loaded);
  } else {
    loaded.status = resolved.status;
    loaded.detail = resolved.detail;
  }

  const int page_count =
      loaded.status == pdfv1::LOAD_STATUS_OK ? loaded.doc->pages() : 0;
  std::vector<std::unique_ptr<poppler::page>> pages(
      static_cast<size_t>(page_count));

  pdfv1::ParseResponse header_msg;
  auto* header = header_msg.mutable_header();
  FillCapabilities(loaded, header->mutable_capabilities());
  // What loading the document said (a rebuilt xref table, a damaged page
  // tree) is about the document as a whole: no page, no family.
  engine_errors.Take(std::nullopt, std::nullopt, &warnings);
  // Pages the document counts but poppler cannot load (a page tree that
  // holds fewer pages than its /Count): left out of the inventory, and
  // each named by a warning in the trailer.
  for (int i = 0; i < page_count; ++i) {
    pages[static_cast<size_t>(i)].reset(loaded.doc->create_page(i));
    poppler::page* page = pages[static_cast<size_t>(i)].get();
    if (page == nullptr) {
      pdfv1::ParseWarning& warning = warnings.emplace_back();
      warning.set_page_index(static_cast<uint32_t>(i));
      warning.set_family(pdfv1::PDF_FAMILY_PAGE_INVENTORY);
      warning.set_message("poppler could not load page " + std::to_string(i) +
                          "; it is left out of the inventory and the page "
                          "chunks");
    }
    engine_errors.Take(static_cast<uint32_t>(i),
                       pdfv1::PDF_FAMILY_PAGE_INVENTORY, &warnings);
    if (page == nullptr) continue;
    auto* info = header->add_pages();
    info->set_page_index(static_cast<uint32_t>(i));
    const bool quarter_turn = IsQuarterTurn(*page);
    const auto rect = page->page_rect();
    info->set_width_pts(quarter_turn ? rect.height() : rect.width());
    info->set_height_pts(quarter_turn ? rect.width() : rect.height());
    info->set_rotation_degrees(RotationDegrees(*page));
    auto* media = info->mutable_media_box();
    const auto media_rect = page->page_rect(poppler::media_box);
    media->set_x0(media_rect.left());
    media->set_y0(media_rect.top());
    media->set_x1(media_rect.right());
    media->set_y1(media_rect.bottom());
    auto* crop = info->mutable_crop_box();
    const auto crop_rect = page->page_rect(poppler::crop_box);
    crop->set_x0(crop_rect.left());
    crop->set_y0(crop_rect.top());
    crop->set_x1(crop_rect.right());
    crop->set_y1(crop_rect.bottom());
    info->set_page_space(pdfv1::PAGE_SPACE_CROP_BOX);
  }
  if (!WriteUngated(gate, writer, header_msg) ||
      loaded.status != pdfv1::LOAD_STATUS_OK) {
    return grpc::Status::OK;
  }
  bool client_ok = true;
  // Reported in the trailer: the items this stream carried, family by
  // family, and the warnings.
  std::map<pdfv1::PdfFamily, uint64_t> counts;
  counts[pdfv1::PDF_FAMILY_PAGE_INVENTORY] =
      static_cast<uint64_t>(header->pages_size());

  // What poppler-cpp does not expose (whether the document is tagged, the
  // info keys that are not text, the encryption dictionary, the outline's
  // targets) comes from poppler's core API (src/poppler_document_facts.h),
  // read once for the families that need it.
  const bool want_meta = WantFamily(*request, pdfv1::PDF_FAMILY_DOC_METADATA);
  const bool want_encryption =
      loaded.doc->is_encrypted() &&
      WantFamily(*request, pdfv1::PDF_FAMILY_ENCRYPTION_INFO);
  const bool want_outline = WantFamily(*request, pdfv1::PDF_FAMILY_OUTLINE);
  DocumentFacts facts;
  if (want_meta || want_encryption || want_outline) {
    facts = ReadDocumentFacts(*resolved.bytes,
                              DocumentPassword(request->document()),
                              want_outline);
    // The reader opens the bytes a second time, so what the load said is
    // not said again.
    engine_errors.TakeAfterReopen(std::nullopt, std::nullopt, &warnings);
  }
  const auto warn_unreadable = [&](pdfv1::PdfFamily family, const char* lost) {
    pdfv1::ParseWarning& warning = warnings.emplace_back();
    warning.set_family(family);
    warning.set_message(
        std::string("poppler's core API could not open the document; ") + lost);
  };

  // Document-level families.
  if (want_meta) {
    pdfv1::ParseResponse msg;
    auto* meta = msg.mutable_doc_meta();
    auto set_if = [](const std::string& v, auto setter) {
      if (!v.empty()) setter(v);
    };
    set_if(ToUtf8(loaded.doc->get_title()),
           [meta](const std::string& v) { meta->set_title(v); });
    set_if(ToUtf8(loaded.doc->get_author()),
           [meta](const std::string& v) { meta->set_author(v); });
    set_if(ToUtf8(loaded.doc->get_subject()),
           [meta](const std::string& v) { meta->set_subject(v); });
    set_if(ToUtf8(loaded.doc->get_keywords()),
           [meta](const std::string& v) { meta->set_keywords(v); });
    set_if(ToUtf8(loaded.doc->get_creator()),
           [meta](const std::string& v) { meta->set_creator(v); });
    set_if(ToUtf8(loaded.doc->get_producer()),
           [meta](const std::string& v) { meta->set_producer(v); });
    set_if(ToUtf8(loaded.doc->info_key("CreationDate")),
           [meta](const std::string& v) { meta->set_created_raw(v); });
    set_if(ToUtf8(loaded.doc->info_key("ModDate")),
           [meta](const std::string& v) { meta->set_modified_raw(v); });
    set_if(ToUtf8(loaded.doc->metadata()),
           [meta](const std::string& v) { meta->set_xmp_xml(v); });
    int major = 0;
    int minor = 0;
    loaded.doc->get_pdf_version(&major, &minor);
    if (major > 0) {
      meta->set_pdf_version(std::to_string(major) + "." +
                            std::to_string(minor));
    }
    if (loaded.doc->is_linearized()) meta->set_linearized(true);
    if (facts.readable) {
      meta->set_tagged(facts.tagged);
      if (facts.trapped.has_value()) meta->set_trapped(*facts.trapped);
      for (const auto& [key, value] : facts.custom_info) {
        auto* entry = meta->add_custom();
        entry->set_key(key);
        entry->set_value(value);
      }
      for (const std::string& key : facts.info_not_carried) {
        pdfv1::ParseWarning& warning = warnings.emplace_back();
        warning.set_family(pdfv1::PDF_FAMILY_DOC_METADATA);
        warning.set_message("info key " + key +
                            " is not carried: its value is not text, a "
                            "name, a number or a boolean");
      }
    } else {
      warn_unreadable(pdfv1::PDF_FAMILY_DOC_METADATA,
                      "whether it is tagged, /Trapped and the nonstandard "
                      "info keys are not reported");
    }
    ++counts[pdfv1::PDF_FAMILY_DOC_METADATA];
    engine_errors.Take(std::nullopt, pdfv1::PDF_FAMILY_DOC_METADATA,
                       &warnings);
    client_ok = WriteUngated(gate, writer, msg);
  }
  if (client_ok && want_encryption) {
    pdfv1::ParseResponse msg;
    auto* enc = msg.mutable_encryption();
    if (facts.readable && facts.encryption.has_value()) {
      const EncryptionFacts& stored = *facts.encryption;
      if (stored.filter.has_value()) enc->set_filter(*stored.filter);
      if (stored.revision.has_value()) enc->set_revision(*stored.revision);
      if (stored.key_length_bits.has_value()) {
        enc->set_key_length_bits(*stored.key_length_bits);
      }
      if (stored.permissions_raw.has_value()) {
        enc->set_permissions_raw(*stored.permissions_raw);
      }
    } else {
      warn_unreadable(pdfv1::PDF_FAMILY_ENCRYPTION_INFO,
                      "the handler, its revision, the key length and the raw "
                      "permissions are not reported");
    }
    enc->set_can_print(loaded.doc->has_permission(poppler::perm_print));
    enc->set_can_modify(loaded.doc->has_permission(poppler::perm_change));
    enc->set_can_copy(loaded.doc->has_permission(poppler::perm_copy));
    enc->set_can_annotate(loaded.doc->has_permission(poppler::perm_add_notes));
    enc->set_can_fill_forms(
        loaded.doc->has_permission(poppler::perm_fill_forms));
    enc->set_can_copy_for_accessibility(
        loaded.doc->has_permission(poppler::perm_accessibility));
    enc->set_can_assemble(loaded.doc->has_permission(poppler::perm_assemble));
    enc->set_can_print_high_res(
        loaded.doc->has_permission(poppler::perm_print_high_resolution));
    ++counts[pdfv1::PDF_FAMILY_ENCRYPTION_INFO];
    engine_errors.Take(std::nullopt, pdfv1::PDF_FAMILY_ENCRYPTION_INFO,
                       &warnings);
    client_ok = WriteUngated(gate, writer, msg);
  }
  if (client_ok && want_outline && facts.readable) {
    if (facts.outline_cut) {
      pdfv1::ParseWarning& warning = warnings.emplace_back();
      warning.set_family(pdfv1::PDF_FAMILY_OUTLINE);
      warning.set_message("outline items nested deeper than " +
                          std::to_string(kMaxOutlineDepth) +
                          " levels are left out");
    }
    if (!facts.outline.empty()) {
      pdfv1::ParseResponse msg;
      auto* chunk = msg.mutable_outline();
      for (pdfv1::OutlineNode& root : facts.outline) {
        *chunk->add_roots() = std::move(root);
      }
      counts[pdfv1::PDF_FAMILY_OUTLINE] += facts.outline_nodes;
      client_ok = WriteUngated(gate, writer, msg);
    }
  } else if (client_ok && want_outline) {
    // The cpp wrapper still has the titles, without their targets.
    warn_unreadable(pdfv1::PDF_FAMILY_OUTLINE,
                    "outline items carry their titles only, no destination "
                    "or URI");
    std::unique_ptr<poppler::toc> toc(loaded.doc->create_toc());
    engine_errors.Take(std::nullopt, pdfv1::PDF_FAMILY_OUTLINE, &warnings);
    if (toc != nullptr && toc->root() != nullptr &&
        !toc->root()->children().empty()) {
      pdfv1::ParseResponse msg;
      auto* chunk = msg.mutable_outline();
      bool cut = false;
      for (const poppler::toc_item* item : toc->root()->children()) {
        FillOutlineItem(item, chunk->add_roots(), 0,
                        &counts[pdfv1::PDF_FAMILY_OUTLINE], &cut);
      }
      if (cut) {
        pdfv1::ParseWarning& warning = warnings.emplace_back();
        warning.set_family(pdfv1::PDF_FAMILY_OUTLINE);
        warning.set_message("outline items nested deeper than " +
                            std::to_string(kMaxOutlineDepth) +
                            " levels are left out");
      }
      client_ok = WriteUngated(gate, writer, msg);
    }
  }
  // Embedded files come from poppler's core API (src/poppler_attachments.h):
  // the cpp wrapper cuts a UTF-16 name short and inflates a whole payload
  // before handing it out.
  if (client_ok && loaded.doc->has_embedded_files() &&
      WantFamily(*request, pdfv1::PDF_FAMILY_ATTACHMENTS)) {
    std::optional<uint64_t> max_data_bytes;
    if (request->options().include_attachment_data()) {
      max_data_bytes = limits_.max_attachment_bytes;
    }
    int rejected_specs = 0;
    const AttachmentsRead read = ReadAttachments(
        *resolved.bytes, DocumentPassword(request->document()), max_data_bytes,
        [&](pdfv1::AttachmentMeta&& meta, AttachmentData data) {
          const auto warn = [&warnings](std::string message) {
            pdfv1::ParseWarning& warning = warnings.emplace_back();
            warning.set_family(pdfv1::PDF_FAMILY_ATTACHMENTS);
            warning.set_message(std::move(message));
          };
          const std::string quoted = "attachment \"" + meta.name() + "\"";
          switch (data) {
            case AttachmentData::kOverLimit:
              warn(quoted + " decodes to more than " +
                   std::to_string(limits_.max_attachment_bytes) +
                   " bytes; its data is omitted");
              break;
            case AttachmentData::kUnavailable:
              warn(quoted +
                   " has no readable embedded file stream; its data is "
                   "omitted");
              break;
            case AttachmentData::kUndecodable:
              warn(quoted +
                   " has an embedded file stream poppler cannot start to "
                   "decode; its data is omitted");
              break;
            case AttachmentData::kIncluded:
              // poppler ends a stream it stops decoding part way as if at its
              // end, so a length that differs from the declared /Params /Size
              // is the one sign of a damaged or misdeclared file.
              if (meta.has_size_bytes() &&
                  meta.data().size() != meta.size_bytes()) {
                warn(quoted + " decodes to " +
                     std::to_string(meta.data().size()) +
                     " bytes but declares /Params /Size " +
                     std::to_string(meta.size_bytes()) +
                     "; its data may be damaged");
              }
              break;
            case AttachmentData::kNotRequested:
              break;
          }
          pdfv1::ParseResponse msg;
          *msg.mutable_attachment() = std::move(meta);
          ++counts[pdfv1::PDF_FAMILY_ATTACHMENTS];
          return WriteUngated(gate, writer, msg);
        },
        &rejected_specs);
    // The reader opens the bytes a second time, so what the load said is
    // not said again.
    engine_errors.TakeAfterReopen(std::nullopt, pdfv1::PDF_FAMILY_ATTACHMENTS,
                                  &warnings);
    if (rejected_specs > 0) {
      pdfv1::ParseWarning& warning = warnings.emplace_back();
      warning.set_family(pdfv1::PDF_FAMILY_ATTACHMENTS);
      warning.set_message(std::to_string(rejected_specs) +
                          " embedded file(s) have a file specification "
                          "poppler rejects and are not listed");
    }
    switch (read) {
      case AttachmentsRead::kDone:
        break;
      case AttachmentsRead::kStopped:
        client_ok = false;
        break;
      case AttachmentsRead::kDocumentUnreadable: {
        pdfv1::ParseWarning& warning = warnings.emplace_back();
        warning.set_family(pdfv1::PDF_FAMILY_ATTACHMENTS);
        warning.set_message(
            "poppler's core API could not open the document; its embedded "
            "files are not listed");
        break;
      }
    }
  }

  // Font table from the document surface.
  FontInterner fonts;
  const bool want_fonts = WantFamily(*request, pdfv1::PDF_FAMILY_FONTS);
  if (client_ok && want_fonts) {
    pdfv1::ParseResponse msg;
    auto* chunk = msg.mutable_fonts();
    uint64_t unnamed = 0;
    for (const poppler::font_info& info : loaded.doc->fonts()) {
      // Font ids are keyed by name, so a font without one has no entry.
      if (info.name().empty()) {
        ++unnamed;
        continue;
      }
      bool is_new = false;
      uint32_t id = fonts.Intern(info.name(), &is_new);
      if (!is_new) continue;
      auto* ref = chunk->add_fonts();
      ref->set_font_id(id);
      // Font names are PDF names, raw bytes in whatever encoding the
      // producer used (GBK and Shift-JIS are common); the ids stay keyed
      // by the raw name.
      ref->set_base_name(ValidUtf8(info.name()));
      ref->set_kind(MapFontKind(info.type()));
      ref->set_embedded(info.is_embedded());
    }
    engine_errors.Take(std::nullopt, pdfv1::PDF_FAMILY_FONTS, &warnings);
    if (unnamed > 0) {
      pdfv1::ParseWarning& warning = warnings.emplace_back();
      warning.set_family(pdfv1::PDF_FAMILY_FONTS);
      warning.set_message(std::to_string(unnamed) +
                          " font(s) without a name are not in the font table");
    }
    if (chunk->fonts_size() > 0) {
      counts[pdfv1::PDF_FAMILY_FONTS] +=
          static_cast<uint64_t>(chunk->fonts_size());
      client_ok = WriteUngated(gate, writer, msg);
    }
  }

  const auto [begin, end] =
      SelectPages(request->has_pages(), request->pages(), page_count);

  const bool want_text = WantFamily(*request, pdfv1::PDF_FAMILY_TEXT_CELLS);
  const bool want_fields = WantFamily(*request, pdfv1::PDF_FAMILY_FORM_FIELDS);
  // Form widgets come from poppler's core API (src/poppler_forms.h); the
  // cpp wrapper has no forms surface.
  std::map<int, std::vector<pdfv1::FormField>> form_fields;
  if (want_fields) {
    FormFieldsRead read = ReadFormFields(
        *resolved.bytes, DocumentPassword(request->document()), begin, end);
    engine_errors.TakeAfterReopen(std::nullopt, pdfv1::PDF_FAMILY_FORM_FIELDS,
                                  &warnings);
    if (read.document_unreadable) {
      warn_unreadable(pdfv1::PDF_FAMILY_FORM_FIELDS,
                      "its form fields are not listed");
    }
    for (const FormFieldsRead::MultipleSelection& multiple :
         read.multiple_selections) {
      pdfv1::ParseWarning& warning = warnings.emplace_back();
      warning.set_page_index(static_cast<uint32_t>(multiple.page_index));
      warning.set_family(pdfv1::PDF_FAMILY_FORM_FIELDS);
      warning.set_message("form field \"" + multiple.field_name + "\" has " +
                          std::to_string(multiple.selected) +
                          " selected values; FormField.value carries the "
                          "first only");
    }
    form_fields = std::move(read.fields);
  }
  // Page chunks carry only page-level families; a request for none of them
  // gets no chunks.
  const bool want_pages = want_text || want_fields;
  for (int i = begin; client_ok && want_pages && i < end; ++i) {
    if (context->IsCancelled()) return grpc::Status::CANCELLED;
    poppler::page* page = pages[static_cast<size_t>(i)].get();
    if (page == nullptr) continue;
    pdfv1::ParseResponse page_msg;
    auto* chunk = page_msg.mutable_page();
    chunk->set_page_index(static_cast<uint32_t>(i));
    // The page's fonts as poppler lists them, read the first time a cell
    // shows a font the stream has not announced.
    std::optional<std::vector<poppler::font_info>> page_fonts;
    if (want_text) {
      const TextFrame frame(*page);
      const auto boxes =
          page->text_list(poppler::page::text_list_include_font);
      // text_list() returns the words it laid out before the content
      // stream broke off, with nothing to tell them from a whole page;
      // what poppler said about the stream is the only sign.
      engine_errors.Take(static_cast<uint32_t>(i),
                         pdfv1::PDF_FAMILY_TEXT_CELLS, &warnings);
      for (const auto& box : boxes) {
        auto* cell = chunk->add_text_cells();
        cell->set_text(ToUtf8(box.text()));
        frame.Place(box, cell->mutable_bbox(), cell->mutable_quad());
        if (box.has_font_info()) {
          cell->set_font_size(box.get_font_size());
          const std::string name = box.get_font_name();
          if (!name.empty()) {
            bool is_new = false;
            uint32_t id = fonts.Intern(name, &is_new);
            cell->set_font_id(id);
            // A cell's font_id must name a FontRef on the stream, so a font
            // first met here is sent ahead of its page chunk even when the
            // request did not ask for the document font table, as
            // grpc-pdfium does.
            if (is_new) {
              pdfv1::ParseResponse fonts_msg;
              auto* ref = fonts_msg.mutable_fonts()->add_fonts();
              ref->set_font_id(id);
              ref->set_base_name(ValidUtf8(name));
              // FontRef.embedded is a plain bool, so an entry with the
              // name alone would read as "not embedded": the kind and the
              // embedding come from the page's own font list.
              if (!page_fonts.has_value()) {
                std::unique_ptr<poppler::font_iterator> on_page(
                    loaded.doc->create_font_iterator(i));
                page_fonts.emplace();
                if (on_page != nullptr && on_page->has_next()) {
                  *page_fonts = on_page->next();
                }
                engine_errors.Take(static_cast<uint32_t>(i),
                                   pdfv1::PDF_FAMILY_FONTS, &warnings);
              }
              bool described = false;
              for (const poppler::font_info& info : *page_fonts) {
                if (info.name() != name) continue;
                ref->set_kind(MapFontKind(info.type()));
                ref->set_embedded(info.is_embedded());
                described = true;
                break;
              }
              if (!described) {
                pdfv1::ParseWarning& warning = warnings.emplace_back();
                warning.set_page_index(static_cast<uint32_t>(i));
                warning.set_family(pdfv1::PDF_FAMILY_FONTS);
                warning.set_message(
                    "font \"" + ValidUtf8(name) +
                    "\" is used by text on this page and is not in "
                    "poppler's font list for it; its kind and whether it is "
                    "embedded are not known");
              }
              ++counts[pdfv1::PDF_FAMILY_FONTS];
              client_ok = WriteUngated(gate, writer, fonts_msg);
              if (!client_ok) break;
            }
          }
        }
        ++counts[pdfv1::PDF_FAMILY_TEXT_CELLS];
      }
    }
    if (!client_ok) break;
    if (auto found = form_fields.find(i); found != form_fields.end()) {
      for (auto& field : found->second) {
        *chunk->add_form_fields() = std::move(field);
        ++counts[pdfv1::PDF_FAMILY_FORM_FIELDS];
      }
    }
    client_ok = WriteUngated(gate, writer, page_msg);
  }
  if (!client_ok) return grpc::Status::OK;

  pdfv1::ParseResponse trailer_msg;
  auto* trailer = trailer_msg.mutable_trailer();
  for (const auto& [family, count] : counts) {
    auto* entry = trailer->add_counts();
    entry->set_family(family);
    entry->set_count(count);
  }
  for (pdfv1::ParseWarning& warning : warnings) {
    *trailer->add_warnings() = std::move(warning);
  }
  WriteUngated(gate, writer, trailer_msg);
  return grpc::Status::OK;
}

grpc::Status PopplerServiceImpl::Render(
    grpc::ServerContext* context, const pdfv1::RenderRequest* request,
    grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  ServerWriterSink<pdfv1::RenderResponse> sink(writer);
  return RenderTo(context, request, nullptr, &sink);
}

namespace {

// What poppler reported first since the last Take, as the tail of a status
// message; empty when it said nothing.
std::string SaidByPoppler(const PopplerErrorScope& errors) {
  const std::string* first = errors.Find({});
  return first != nullptr ? ": " + *first : std::string();
}

}  // namespace

grpc::Status PopplerServiceImpl::RenderTo(
    grpc::ServerContext* context, const pdfv1::RenderRequest* request,
    const std::string* stream_bytes,
    MessageSink<pdfv1::RenderResponse>* writer) {
  // dpi sizes the raster splash allocates, so NaN and infinity are refused
  // along with zero and negative values, and so is a dpi above the cap;
  // the per-page pixel cap below bounds what remains.
  const double dpi = request->dpi();
  if (!std::isfinite(dpi) || dpi <= 0.0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dpi must be a positive number");
  }
  if (dpi > limits_.max_render_dpi) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dpi " + FormatNumber(dpi) +
                            " is above the maximum of " +
                            FormatNumber(limits_.max_render_dpi));
  }
  if (request->has_pages()) {
    if (grpc::Status range = CheckPageRange(request->pages()); !range.ok()) {
      return range;
    }
  }
  // The page's paper: the request's background laid over white, since a
  // raster carries no transparency; white when the request sets none.
  uint32_t paper = 0xffffff;
  if (request->has_background()) {
    const pdfv1::Color& color = request->background();
    for (const double component :
         {color.red(), color.green(), color.blue(), color.alpha()}) {
      if (!std::isfinite(component)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "background components must be finite numbers");
      }
    }
    const double alpha = std::clamp(color.alpha(), 0.0, 1.0);
    const auto channel = [alpha](double component) {
      const double over_white =
          std::clamp(component, 0.0, 1.0) * alpha + (1.0 - alpha);
      return static_cast<uint32_t>(std::lround(over_white * 255.0));
    };
    paper = (channel(color.red()) << 16) | (channel(color.green()) << 8) |
            channel(color.blue());
  }
  const ResolvedBytes resolved =
      stream_bytes != nullptr ? StreamBytes(stream_bytes)
                              : ResolveDocumentBytes(request->document(), cache_);
  if (resolved.invalid_argument) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, resolved.detail);
  }
  PopplerGate gate;
  // What poppler says while it draws a page rides beside that page's
  // raster (RenderResponse.warnings), so a partly drawn page is not handed
  // out as a whole one.
  PopplerErrorScope engine_errors;
  LoadedDocument loaded;
  if (resolved.status == pdfv1::LOAD_STATUS_OK) {
    LoadDocument(*resolved.bytes, request->document(), engine_errors,
                 &loaded);
  } else {
    loaded.status = resolved.status;
    loaded.detail = resolved.detail;
  }
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    // The contract types load failures in a one-message head, never as a
    // bare gRPC error.
    pdfv1::RenderResponse head_msg;
    auto* head = head_msg.mutable_head();
    head->set_load_status(loaded.status);
    if (!loaded.detail.empty()) head->set_load_detail(loaded.detail);
    WriteUngated(gate, writer, head_msg);
    return grpc::Status::OK;
  }

  const auto [begin, end] = SelectPages(request->has_pages(), request->pages(),
                                        loaded.doc->pages());
  // What loading and counting the pages said is about the document, not a
  // page: Parse carries it in its trailer. Render's warnings are about the
  // raster they ride with, so here it goes to the log, and stays off the
  // first page.
  {
    std::vector<pdfv1::ParseWarning> load_said;
    engine_errors.Take(std::nullopt, std::nullopt, &load_said);
    for (const pdfv1::ParseWarning& said : load_said) {
      std::fprintf(stderr, "grpc-poppler: Render: %s\n",
                   said.message().c_str());
    }
  }

  poppler::page_renderer renderer;
  renderer.set_image_format(poppler::image::format_bgr24);
  renderer.set_paper_color(0xff000000u | paper);
  for (int i = begin; i < end; ++i) {
    if (context->IsCancelled()) return grpc::Status::CANCELLED;
    pdfv1::RenderResponse msg;
    {
      // The page and its image are freed here, under the gate, before the
      // write; the message holds the only copy of the pixels on the wire.
      std::unique_ptr<poppler::page> page(loaded.doc->create_page(i));
      if (page == nullptr) {
        return grpc::Status(
            grpc::StatusCode::INTERNAL,
            "poppler could not load page " + std::to_string(i) +
                SaidByPoppler(engine_errors));
      }
      // The raster's size is known before splash allocates it: the CropBox
      // at the requested dpi (rounded up here; splash rounds to nearest).
      const poppler::rectf box = page->page_rect();
      const double scale = dpi / 72.0;
      const double pixels =
          std::ceil(box.width() * scale) * std::ceil(box.height() * scale);
      if (pixels > static_cast<double>(limits_.max_raster_pixels)) {
        return grpc::Status(
            grpc::StatusCode::RESOURCE_EXHAUSTED,
            "page " + std::to_string(i) + " at " + FormatNumber(dpi) +
                " dpi would be " + FormatNumber(pixels) +
                " pixels, above the limit of " +
                std::to_string(limits_.max_raster_pixels));
      }
      auto* raster = msg.mutable_raster();
      if (request->omit_annotations()) {
        // poppler-cpp's renderer always draws annotations; the page goes
        // through the core API instead (src/poppler_render.h).
        switch (RenderWithoutAnnotations(*resolved.bytes,
                                         DocumentPassword(request->document()),
                                         i, dpi, paper, raster)) {
          case CoreRender::kRendered:
            break;
          case CoreRender::kDocumentUnreadable:
            return grpc::Status(
                grpc::StatusCode::INTERNAL,
                "poppler's core API could not open the document to render "
                "page " + std::to_string(i) + " without its annotations" +
                    SaidByPoppler(engine_errors));
          case CoreRender::kFailed:
            return grpc::Status(
                grpc::StatusCode::INTERNAL,
                "poppler could not render page " + std::to_string(i) +
                    SaidByPoppler(engine_errors));
        }
      } else {
        const poppler::image image = renderer.render_page(page.get(), dpi, dpi);
        if (!image.is_valid()) {
          return grpc::Status(
              grpc::StatusCode::INTERNAL,
              "poppler could not render page " + std::to_string(i) +
                  SaidByPoppler(engine_errors));
        }
        raster->set_page_index(static_cast<uint32_t>(i));
        raster->set_width_px(static_cast<uint32_t>(image.width()));
        raster->set_height_px(static_cast<uint32_t>(image.height()));
        raster->set_stride_bytes(static_cast<uint32_t>(image.bytes_per_row()));
        raster->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
        raster->set_dpi(dpi);
        raster->set_pixels(image.const_data(),
                           static_cast<size_t>(image.bytes_per_row()) *
                               image.height());
      }
      std::vector<pdfv1::ParseWarning> said;
      engine_errors.Take(static_cast<uint32_t>(i),
                         pdfv1::PDF_FAMILY_PAGE_RASTER, &said);
      for (pdfv1::ParseWarning& warning : said) {
        *msg.add_warnings() = std::move(warning);
      }
    }
    if (!WriteUngated(gate, writer, msg)) return grpc::Status::OK;
  }
  return grpc::Status::OK;
}

grpc::Status PopplerServiceImpl::ParseStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<pdfv1::ParseStreamResponse, pdfv1::ParseStreamRequest>*
        stream) {
  ParseStreamHandlers handlers;
  handlers.parse = [this, context](const pdfv1::ParseRequest& request,
                                   const std::shared_ptr<std::string>& bytes,
                                   MessageSink<pdfv1::ParseResponse>* sink,
                                   RequestCancel*) {
    return ParseTo(context, &request, bytes.get(), sink);
  };
  handlers.render = [this, context](const pdfv1::RenderRequest& request,
                                    const std::shared_ptr<std::string>& bytes,
                                    MessageSink<pdfv1::RenderResponse>* sink,
                                    RequestCancel*) {
    return RenderTo(context, &request, bytes.get(), sink);
  };
  handlers.verdict = [](pdfv1::BackendCapabilities* caps, pdfv1::LoadStatus status,
                        const std::string& detail) {
    LoadedDocument verdict;
    verdict.status = status;
    verdict.detail = detail;
    FillCapabilities(verdict, caps);
  };
  handlers.sha256_hex = Sha256Hex;
  // A verified upload is cached like a verified unary upload, so a client
  // that falls back to Probe, Parse or Render can address it by hash.
  handlers.verified = [this](const std::string& sha256,
                             const std::shared_ptr<std::string>& bytes) {
    cache_.Insert(sha256, *bytes);
  };
  ParseStreamLimits limits;
  limits.max_document_bytes = kMaxStreamDocumentBytes;
  limits.max_parallel_requests = kStreamParallelRequests;
  return ServeParseStream(context, stream, handlers, limits);
}

grpc::Status PopplerServiceImpl::GetServiceInfo(
    grpc::ServerContext* /*context*/,
    const pdfv1::ServiceInfoRequest* /*request*/,
    pdfv1::ServiceInfoResponse* response) {
  response->set_backend_name(kBackendName);
  response->set_engine_version(EngineVersion());
  response->set_build_version(GRPC_POPPLER_BUILD_VERSION);
  auto* ui = response->mutable_ui();
  ui->set_title("Poppler");
  ui->set_path("/ui/poppler");
  ui->set_description(
      "Poppler PDF backend, the fleet's GPL extraction-quality reference; "
      "no web UI yet");
  return grpc::Status::OK;
}

}  // namespace grpc_poppler
