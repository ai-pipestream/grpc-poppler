#include "poppler_service_impl.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-embedded-file.h>
#include <poppler/cpp/poppler-font.h>
#include <poppler/cpp/poppler-image.h>
#include <poppler/cpp/poppler-page-renderer.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-toc.h>
#include <poppler/cpp/poppler-version.h>

#include "document_cache.h"
#include "sha256.h"

namespace grpc_poppler {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

constexpr char kBackendName[] = "grpc-poppler";

// Mirrors gRParse's arm64 serialization gate: poppler calls crash under
// concurrency on arm64, so they run one at a time there and concurrently
// elsewhere.
#if defined(__aarch64__)
class PopplerGate {
 public:
  PopplerGate() : lock_(Mutex()) {}

 private:
  static std::mutex& Mutex() {
    static std::mutex m;
    return m;
  }
  std::lock_guard<std::mutex> lock_;
};
#else
class PopplerGate {};
#endif

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
      return true;
    default:
      return false;
  }
}

const char* UnsupportedDetail(pdfv1::PdfFamily family) {
  switch (family) {
    case pdfv1::PDF_FAMILY_ANNOTATIONS:
    case pdfv1::PDF_FAMILY_FORM_FIELDS:
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

void LoadDocument(const std::string& data, const pdfv1::PdfDocument& request,
                  LoadedDocument* out) {
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
  if (out->doc == nullptr) {
    out->status = request.has_password()
                      ? pdfv1::LOAD_STATUS_PASSWORD_INCORRECT
                      : pdfv1::LOAD_STATUS_CORRUPT;
    out->detail = "poppler could not open the document";
    return;
  }
  if (out->doc->is_locked()) {
    out->status = request.has_password()
                      ? pdfv1::LOAD_STATUS_PASSWORD_INCORRECT
                      : pdfv1::LOAD_STATUS_PASSWORD_REQUIRED;
    out->doc.reset();
    return;
  }
  out->status = pdfv1::LOAD_STATUS_OK;
}

void FillCapabilities(const LoadedDocument& loaded,
                      pdfv1::BackendCapabilities* caps) {
  caps->set_backend_name(kBackendName);
  caps->set_engine_version(std::string("poppler ") + poppler::version_string());
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

bool IsQuarterTurn(const poppler::page& page) {
  const auto orientation = page.orientation();
  return orientation == poppler::page::landscape ||
         orientation == poppler::page::seascape;
}

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

void FillOutlineItem(const poppler::toc_item* item, pdfv1::OutlineNode* node,
                     int depth) {
  node->set_title(ToUtf8(item->title()));
  if (depth >= 64) return;
  for (const poppler::toc_item* child : item->children()) {
    FillOutlineItem(child, node->add_children(), depth + 1);
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
  LoadedDocument loaded;
  if (resolved.status == pdfv1::LOAD_STATUS_OK) {
    LoadDocument(*resolved.bytes, request->document(), &loaded);
  } else {
    loaded.status = resolved.status;
    loaded.detail = resolved.detail;
  }
  FillCapabilities(loaded, response->mutable_capabilities());
  return grpc::Status::OK;
}

grpc::Status PopplerServiceImpl::Parse(
    grpc::ServerContext* /*context*/, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  const ResolvedBytes resolved = ResolveDocumentBytes(request->document(), cache_);
  if (resolved.invalid_argument) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, resolved.detail);
  }
  PopplerGate gate;
  LoadedDocument loaded;
  if (resolved.status == pdfv1::LOAD_STATUS_OK) {
    LoadDocument(*resolved.bytes, request->document(), &loaded);
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
  for (int i = 0; i < page_count; ++i) {
    pages[static_cast<size_t>(i)].reset(loaded.doc->create_page(i));
    poppler::page* page = pages[static_cast<size_t>(i)].get();
    if (page == nullptr) continue;
    auto* info = header->add_pages();
    info->set_page_index(static_cast<uint32_t>(i));
    const bool quarter_turn = IsQuarterTurn(*page);
    const auto rect = page->page_rect();
    info->set_width_pts(quarter_turn ? rect.height() : rect.width());
    info->set_height_pts(quarter_turn ? rect.width() : rect.height());
    // The cpp wrapper reports orientation, not the /Rotate value; a
    // quarter turn is reported as 90 by convention here.
    info->set_rotation_degrees(quarter_turn ? 90 : 0);
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
  }
  if (!writer->Write(header_msg) || loaded.status != pdfv1::LOAD_STATUS_OK) {
    return grpc::Status::OK;
  }
  bool client_ok = true;

  // Document-level families.
  if (WantFamily(*request, pdfv1::PDF_FAMILY_DOC_METADATA)) {
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
    client_ok = writer->Write(msg);
  }
  if (client_ok && loaded.doc->is_encrypted() &&
      WantFamily(*request, pdfv1::PDF_FAMILY_ENCRYPTION_INFO)) {
    pdfv1::ParseResponse msg;
    auto* enc = msg.mutable_encryption();
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
    client_ok = writer->Write(msg);
  }
  if (client_ok && WantFamily(*request, pdfv1::PDF_FAMILY_OUTLINE)) {
    std::unique_ptr<poppler::toc> toc(loaded.doc->create_toc());
    if (toc != nullptr && toc->root() != nullptr &&
        !toc->root()->children().empty()) {
      pdfv1::ParseResponse msg;
      auto* chunk = msg.mutable_outline();
      for (const poppler::toc_item* item : toc->root()->children()) {
        FillOutlineItem(item, chunk->add_roots(), 0);
      }
      client_ok = writer->Write(msg);
    }
  }
  if (client_ok && loaded.doc->has_embedded_files() &&
      WantFamily(*request, pdfv1::PDF_FAMILY_ATTACHMENTS)) {
    for (poppler::embedded_file* file : loaded.doc->embedded_files()) {
      if (file == nullptr || !file->is_valid()) continue;
      pdfv1::ParseResponse msg;
      auto* meta = msg.mutable_attachment();
      meta->set_name(file->name());
      std::string desc = ToUtf8(file->description());
      if (!desc.empty()) meta->set_description(desc);
      if (file->size() >= 0) {
        meta->set_size_bytes(static_cast<uint64_t>(file->size()));
      }
      if (!file->mime_type().empty()) meta->set_mime_type(file->mime_type());
      if (request->options().include_attachment_data()) {
        poppler::byte_array data = file->data();
        meta->set_data(std::string(data.begin(), data.end()));
      }
      client_ok = writer->Write(msg);
      if (!client_ok) break;
    }
  }

  // Font table from the document surface.
  FontInterner fonts;
  if (client_ok && WantFamily(*request, pdfv1::PDF_FAMILY_FONTS)) {
    pdfv1::ParseResponse msg;
    auto* chunk = msg.mutable_fonts();
    for (const poppler::font_info& info : loaded.doc->fonts()) {
      if (info.name().empty()) continue;
      bool is_new = false;
      uint32_t id = fonts.Intern(info.name(), &is_new);
      if (!is_new) continue;
      auto* ref = chunk->add_fonts();
      ref->set_font_id(id);
      ref->set_base_name(info.name());
      ref->set_kind(MapFontKind(info.type()));
      ref->set_embedded(info.is_embedded());
    }
    if (chunk->fonts_size() > 0) client_ok = writer->Write(msg);
  }

  int begin = 0;
  int end = page_count;
  if (request->has_pages()) {
    begin =
        std::min<int>(static_cast<int>(request->pages().begin()), page_count);
    end = std::min<int>(static_cast<int>(request->pages().end()), page_count);
  }

  std::map<pdfv1::PdfFamily, uint64_t> counts;
  counts[pdfv1::PDF_FAMILY_PAGE_INVENTORY] = static_cast<uint64_t>(page_count);
  const bool want_text = WantFamily(*request, pdfv1::PDF_FAMILY_TEXT_CELLS);
  for (int i = begin; client_ok && i < end; ++i) {
    poppler::page* page = pages[static_cast<size_t>(i)].get();
    if (page == nullptr) continue;
    pdfv1::ParseResponse page_msg;
    auto* chunk = page_msg.mutable_page();
    chunk->set_page_index(static_cast<uint32_t>(i));
    if (want_text) {
      const bool quarter_turn = IsQuarterTurn(*page);
      const auto rect = page->page_rect();
      const double page_height =
          quarter_turn ? rect.width() : rect.height();
      const auto boxes =
          page->text_list(poppler::page::text_list_include_font);
      for (const auto& box : boxes) {
        const auto b = box.bbox();
        auto* cell = chunk->add_text_cells();
        cell->set_text(ToUtf8(box.text()));
        // poppler reports top-left-origin boxes; the contract wants the
        // PDF bottom-left convention.
        auto* bbox = cell->mutable_bbox();
        bbox->set_x0(b.x());
        bbox->set_y0(page_height - (b.y() + b.height()));
        bbox->set_x1(b.x() + b.width());
        bbox->set_y1(page_height - b.y());
        auto* quad = cell->mutable_quad();
        quad->set_x0(bbox->x0());
        quad->set_y0(bbox->y0());
        quad->set_x1(bbox->x1());
        quad->set_y1(bbox->y0());
        quad->set_x2(bbox->x1());
        quad->set_y2(bbox->y1());
        quad->set_x3(bbox->x0());
        quad->set_y3(bbox->y1());
        if (box.has_font_info()) {
          cell->set_font_size(box.get_font_size());
          const std::string name = box.get_font_name();
          if (!name.empty()) {
            bool is_new = false;
            uint32_t id = fonts.Intern(name, &is_new);
            cell->set_font_id(id);
            if (is_new) {
              pdfv1::ParseResponse fonts_msg;
              auto* ref = fonts_msg.mutable_fonts()->add_fonts();
              ref->set_font_id(id);
              ref->set_base_name(name);
              ++counts[pdfv1::PDF_FAMILY_FONTS];
              client_ok = writer->Write(fonts_msg);
              if (!client_ok) break;
            }
          }
        }
        ++counts[pdfv1::PDF_FAMILY_TEXT_CELLS];
      }
    }
    if (!client_ok) break;
    client_ok = writer->Write(page_msg);
  }
  if (!client_ok) return grpc::Status::OK;

  pdfv1::ParseResponse trailer_msg;
  auto* trailer = trailer_msg.mutable_trailer();
  for (const auto& [family, count] : counts) {
    auto* entry = trailer->add_counts();
    entry->set_family(family);
    entry->set_count(count);
  }
  writer->Write(trailer_msg);
  return grpc::Status::OK;
}

grpc::Status PopplerServiceImpl::Render(
    grpc::ServerContext* /*context*/, const pdfv1::RenderRequest* request,
    grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  if (request->dpi() <= 0.0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dpi must be positive");
  }
  const ResolvedBytes resolved = ResolveDocumentBytes(request->document(), cache_);
  if (resolved.invalid_argument) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, resolved.detail);
  }
  PopplerGate gate;
  LoadedDocument loaded;
  if (resolved.status == pdfv1::LOAD_STATUS_OK) {
    LoadDocument(*resolved.bytes, request->document(), &loaded);
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
    writer->Write(head_msg);
    return grpc::Status::OK;
  }

  const int page_count = loaded.doc->pages();
  int begin = 0;
  int end = page_count;
  if (request->has_pages()) {
    begin =
        std::min<int>(static_cast<int>(request->pages().begin()), page_count);
    end = std::min<int>(static_cast<int>(request->pages().end()), page_count);
  }

  poppler::page_renderer renderer;
  renderer.set_image_format(poppler::image::format_bgr24);
  for (int i = begin; i < end; ++i) {
    std::unique_ptr<poppler::page> page(loaded.doc->create_page(i));
    if (page == nullptr) continue;
    const poppler::image image =
        renderer.render_page(page.get(), request->dpi(), request->dpi());
    if (!image.is_valid()) continue;
    pdfv1::RenderResponse msg;
    auto* raster = msg.mutable_raster();
    raster->set_page_index(static_cast<uint32_t>(i));
    raster->set_width_px(static_cast<uint32_t>(image.width()));
    raster->set_height_px(static_cast<uint32_t>(image.height()));
    raster->set_stride_bytes(static_cast<uint32_t>(image.bytes_per_row()));
    raster->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    raster->set_dpi(request->dpi());
    raster->set_pixels(image.const_data(),
                       static_cast<size_t>(image.bytes_per_row()) *
                           image.height());
    if (!writer->Write(msg)) return grpc::Status::OK;
  }
  return grpc::Status::OK;
}

}  // namespace grpc_poppler
