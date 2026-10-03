// SPDX-License-Identifier: GPL-3.0-or-later

// Contract test for grpc-poppler: the tier 0 floor plus the cpp-surface
// document families (metadata with XMP, outline, attachments, fonts) over
// the hello.pdf and rich.pdf fixtures, the page frame on rotated and
// cropped pages (frames.pdf), names that are not UTF-8 in the file and a
// decode cap (encodings.pdf), attachments whose data cannot be had whole
// (damaged_attachments.pdf), pages poppler cannot load
// (missing_pages.pdf), page ranges, Render bounds, and the arm64 gate under
// a stalled client.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "poppler_attachments.h"
#include "poppler_service_impl.h"
#include "sha256.h"

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// Bounds a call whose regression could otherwise hang the test: a client
// that stops reading a stream (say, at a message it cannot parse) while the
// server still writes waits on its status forever once flow control stops
// the server.
void SetDeadline(grpc::ClientContext* ctx) {
  ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));
}

bool Near(double a, double b) { return std::abs(a - b) < 0.01; }

bool SameBox(const pdfv1::BoundingBox& a, const pdfv1::BoundingBox& b) {
  return Near(a.x0(), b.x0()) && Near(a.y0(), b.y0()) &&
         Near(a.x1(), b.x1()) && Near(a.y1(), b.y1());
}

// The quad of text read left to right in page space: lower-left,
// lower-right, upper-right, upper-left of its box.
bool QuadReadsLeftToRight(const pdfv1::Quad& q, const pdfv1::BoundingBox& b) {
  return Near(q.x0(), b.x0()) && Near(q.y0(), b.y0()) &&
         Near(q.x1(), b.x1()) && Near(q.y1(), b.y0()) &&
         Near(q.x2(), b.x1()) && Near(q.y2(), b.y1()) &&
         Near(q.x3(), b.x0()) && Near(q.y3(), b.y1());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const std::string hello = ReadFile(dir + "/hello.pdf");
  const std::string rich = ReadFile(dir + "/rich.pdf");
  const std::string frames = ReadFile(dir + "/frames.pdf");
  const std::string encodings = ReadFile(dir + "/encodings.pdf");
  const std::string missing_pages = ReadFile(dir + "/missing_pages.pdf");
  const std::string damaged_attachments =
      ReadFile(dir + "/damaged_attachments.pdf");
  Check(!hello.empty() && !rich.empty() && !frames.empty() &&
            !encodings.empty() && !missing_pages.empty() &&
            !damaged_attachments.empty(),
        "fixtures read");

  grpc_poppler::PopplerServiceImpl service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  Check(server != nullptr && port != 0, "server started");
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = pdfv1::PdfBackendService::NewStub(channel);

  // A second service with tight resource limits, to reach them with small
  // fixtures.
  grpc_poppler::ResourceLimits tight_limits;
  tight_limits.max_attachment_bytes = 4096;
  tight_limits.max_render_dpi = 150;
  tight_limits.max_raster_pixels = 612 * 792;  // hello.pdf at 72 dpi
  grpc_poppler::PopplerServiceImpl tight_service({8, 1u << 30}, tight_limits);
  grpc::ServerBuilder tight_builder;
  int tight_port = 0;
  tight_builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &tight_port);
  tight_builder.RegisterService(&tight_service);
  std::unique_ptr<grpc::Server> tight_server = tight_builder.BuildAndStart();
  Check(tight_server != nullptr && tight_port != 0, "tight-limit server up");
  // No receive limit on this client: a raster the service should have
  // refused must arrive and fail the check, not trip the client's default
  // 4 MiB limit, which is RESOURCE_EXHAUSTED too.
  grpc::ChannelArguments tight_args;
  tight_args.SetMaxReceiveMessageSize(-1);
  auto tight_stub = pdfv1::PdfBackendService::NewStub(grpc::CreateCustomChannel(
      "127.0.0.1:" + std::to_string(tight_port),
      grpc::InsecureChannelCredentials(), tight_args));

  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(hello);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "Probe RPC OK");
    const auto& caps = response.capabilities();
    Check(caps.backend_name() == "grpc-poppler", "backend name reported");
    Check(caps.load_status() == pdfv1::LOAD_STATUS_OK, "hello.pdf loads");
    Check(caps.page_count() == 1, "one page counted");
    Check(caps.families_size() == pdfv1::PdfFamily_MAX,
          "a verdict for each family");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ServiceInfoRequest request;
    pdfv1::ServiceInfoResponse response;
    Check(stub->GetServiceInfo(&ctx, request, &response).ok(),
          "GetServiceInfo RPC OK");
    Check(response.backend_name() == "grpc-poppler",
          "GetServiceInfo backend name matches Probe");
    Check(!response.engine_version().empty(), "engine version reported");
    Check(!response.build_version().empty(), "build version reported");
    Check(response.has_ui() && response.ui().path() == "/ui/poppler",
          "UiInfo advertisement present");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data("not a pdf");
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "non-PDF Probe OK");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_NOT_PDF,
          "non-PDF bytes report LOAD_STATUS_NOT_PDF");
  }

  // Parse rich.pdf.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(rich);
    request.mutable_options()->set_include_attachment_data(true);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;

    Check(reader->Read(&msg) && msg.has_header(), "header first");
    Check(msg.header().pages_size() == 1, "inventory lists the page");

    std::string all_text;
    std::string title;
    std::string xmp;
    int outline_roots = 0;
    int attachments = 0;
    std::string attachment_data;
    bool font_named = false;
    std::vector<pdfv1::FormField> form_fields;
    std::map<int, uint64_t> counts;
    // What the stream carried, family by family, to hold the trailer to.
    std::map<int, uint64_t> received;
    received[pdfv1::PDF_FAMILY_PAGE_INVENTORY] =
        static_cast<uint64_t>(msg.header().pages_size());
    std::function<uint64_t(const pdfv1::OutlineNode&)> outline_nodes =
        [&outline_nodes](const pdfv1::OutlineNode& node) -> uint64_t {
      uint64_t n = 1;
      for (const auto& child : node.children()) n += outline_nodes(child);
      return n;
    };
    while (reader->Read(&msg)) {
      if (msg.has_page()) {
        for (const auto& cell : msg.page().text_cells()) {
          all_text += cell.text() + " ";
        }
        for (const auto& f : msg.page().form_fields()) form_fields.push_back(f);
        received[pdfv1::PDF_FAMILY_TEXT_CELLS] += msg.page().text_cells_size();
        received[pdfv1::PDF_FAMILY_FORM_FIELDS] += msg.page().form_fields_size();
      } else if (msg.has_doc_meta()) {
        title = msg.doc_meta().title();
        xmp = msg.doc_meta().xmp_xml();
        ++received[pdfv1::PDF_FAMILY_DOC_METADATA];
      } else if (msg.has_outline()) {
        outline_roots = msg.outline().roots_size();
        for (const auto& root : msg.outline().roots()) {
          received[pdfv1::PDF_FAMILY_OUTLINE] += outline_nodes(root);
        }
      } else if (msg.has_attachment()) {
        ++attachments;
        attachment_data = msg.attachment().data();
        ++received[pdfv1::PDF_FAMILY_ATTACHMENTS];
      } else if (msg.has_fonts()) {
        for (const auto& f : msg.fonts().fonts()) {
          if (f.base_name().find("Helvetica") != std::string::npos) {
            font_named = true;
          }
        }
        received[pdfv1::PDF_FAMILY_FONTS] += msg.fonts().fonts_size();
      } else if (msg.has_trailer()) {
        for (const auto& c : msg.trailer().counts()) {
          counts[c.family()] = c.count();
        }
      }
    }
    Check(reader->Finish().ok(), "Parse finished OK");
    Check(all_text.find("Tagged Hello") != std::string::npos,
          "cells carry the page text");
    Check(title == "Rich Fixture", "info title extracted");
    Check(outline_roots == 2, "outline items extracted");
    Check(attachments == 1, "attachment listed");
    Check(attachment_data == "id,total\n1,999.99\n",
          "attachment bytes round-tripped");
    Check(font_named, "font table names Helvetica");
    Check(counts[pdfv1::PDF_FAMILY_TEXT_CELLS] >= 1, "trailer counts cells");
    // The trailer totals everything emitted on the stream, the
    // document-level families and the font table included.
    for (int f = pdfv1::PdfFamily_MIN + 1; f <= pdfv1::PdfFamily_MAX; ++f) {
      Check(counts[f] == received[f],
            ("the trailer counts " +
             pdfv1::PdfFamily_Name(static_cast<pdfv1::PdfFamily>(f)) +
             " as received")
                .c_str());
    }
    Check(received[pdfv1::PDF_FAMILY_DOC_METADATA] == 1 &&
              received[pdfv1::PDF_FAMILY_OUTLINE] == 2 &&
              received[pdfv1::PDF_FAMILY_ATTACHMENTS] == 1 &&
              received[pdfv1::PDF_FAMILY_FONTS] >= 2,
          "rich.pdf carries every document-level family");

    // AcroForm widgets, read through poppler's core API, in /Annots order.
    Check(form_fields.size() == 2, "both form field widgets extracted");
    Check(counts[pdfv1::PDF_FAMILY_FORM_FIELDS] == 2, "trailer counts widgets");
    if (form_fields.size() == 2) {
      const auto& text = form_fields[0];
      Check(text.kind() == pdfv1::FORM_FIELD_KIND_TEXT, "text field kind");
      Check(text.name() == "customer_name", "text field name");
      Check(text.value() == "Jordan Example", "text field value");
      Check(text.alternate_name() == "Customer name", "text field tooltip");
      Check(text.has_flags() && text.flags() == 0 && !text.read_only(),
            "an absent /Ff is the empty mask");
      Check(!text.has_appearance_state(), "text widget has no /AS");
      Check(text.rect().x0() == 300 && text.rect().y0() == 300 &&
                text.rect().x1() == 450 && text.rect().y1() == 320,
            "text widget rect in page space");
      // /FT and /Ff (ReadOnly) come from the parent field; /AS from the
      // widget annotation.
      const auto& box = form_fields[1];
      Check(box.kind() == pdfv1::FORM_FIELD_KIND_CHECK_BOX,
            "check box kind inherited from the parent field");
      Check(box.name() == "agree", "check box takes the parent's name");
      Check(box.has_flags() && box.flags() == 1, "/Ff inherited from the parent");
      Check(box.read_only(), "read-only follows the inherited /Ff");
      Check(box.appearance_state() == "/Yes", "/AS keeps the leading slash");
      Check(box.value() == "Yes", "button value is the state name");
      Check(box.alternate_name() == "I agree", "check box tooltip inherited");
    }
  }

  // Attachments are read through poppler's core API (encodings.pdf, from
  // test/fixtures/make_encodings_pdf.py). Names are PDF text strings and
  // arrive as UTF-8 whatever their encoding in the file; the MIME type, a
  // PDF name, is made valid UTF-8, since the client cannot parse a message
  // whose string field is not and would lose the whole stream.
  auto parse_attachments = [](pdfv1::PdfBackendService::Stub& client,
                               const std::string& document,
                               std::map<std::string, pdfv1::AttachmentMeta>* found,
                               std::vector<pdfv1::ParseWarning>* warnings) {
    grpc::ClientContext ctx;
    SetDeadline(&ctx);
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(document);
    request.add_families(pdfv1::PDF_FAMILY_ATTACHMENTS);
    request.mutable_options()->set_include_attachment_data(true);
    auto reader = client.Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    while (reader->Read(&msg)) {
      if (msg.has_attachment()) (*found)[msg.attachment().name()] = msg.attachment();
      if (msg.has_trailer()) {
        warnings->assign(msg.trailer().warnings().begin(),
                         msg.trailer().warnings().end());
      }
    }
    return reader->Finish().ok();
  };
  const std::string resume_name = "r\xc3\xa9sum\xc3\xa9.txt";
  const std::string cafe_name = "caf\xc3\xa9.csv";
  {
    std::map<std::string, pdfv1::AttachmentMeta> found;
    std::vector<pdfv1::ParseWarning> warnings;
    Check(parse_attachments(*stub, encodings, &found, &warnings),
          "encodings.pdf attachments stream finishes OK");
    Check(found.size() == 3, "all three attachments listed");
    Check(found.count(resume_name) == 1, "a UTF-16 /UF name is decoded");
    if (found.count(resume_name) == 1) {
      const auto& resume = found[resume_name];
      Check(resume.mime_type() == "text/\xc3\xa9",
            "a MIME name that is not UTF-8 arrives as valid UTF-8");
      Check(resume.size_bytes() == 11 && resume.data() == "plain text\n",
            "the UTF-16-named attachment carries its bytes");
    }
    Check(found.count(cafe_name) == 1 &&
              found[cafe_name].data() == "a,b\n1,2\n",
          "a PDFDocEncoding /F name is decoded");
    Check(found.count("zeros.bin") == 1 &&
              found["zeros.bin"].data() == std::string(65536, '\0'),
          "an attachment under the cap carries all its inflated bytes");
    // zeros.bin declares /Params /Size 16 and inflates to 65536 bytes; the
    // mismatch is the one sign poppler gives of a damaged stream, so the
    // trailer names it. The others decode to their declared sizes.
    Check(warnings.size() == 1 &&
              warnings[0].family() == pdfv1::PDF_FAMILY_ATTACHMENTS &&
              warnings[0].message().find("zeros.bin") != std::string::npos &&
              warnings[0].message().find("/Size 16") != std::string::npos,
          "the trailer warns only about the size mismatch");
  }
  // Attachments whose data cannot be had whole
  // (test/fixtures/make_damaged_attachments_pdf.py) are still listed, and
  // the trailer says what went wrong with each.
  {
    std::map<std::string, pdfv1::AttachmentMeta> found;
    std::vector<pdfv1::ParseWarning> warnings;
    Check(parse_attachments(*stub, damaged_attachments, &found, &warnings),
          "damaged_attachments.pdf attachments stream finishes OK");
    Check(found.size() == 2, "both damaged attachments are listed");
    Check(found.count("lost.txt") == 1 && !found["lost.txt"].has_data(),
          "an attachment without an embedded stream has no data");
    auto warned = [&warnings](const std::string& name, const std::string& why) {
      for (const auto& warning : warnings) {
        if (warning.family() == pdfv1::PDF_FAMILY_ATTACHMENTS &&
            warning.message().find(name) != std::string::npos &&
            warning.message().find(why) != std::string::npos) {
          return true;
        }
      }
      return false;
    };
    Check(warnings.size() == 2, "one warning for each damaged attachment");
    Check(warned("garbage.bin", "/Size 5"),
          "a stream that decodes short of its declared size is warned about");
    Check(warned("lost.txt", "no readable embedded file stream"),
          "an attachment without an embedded stream is warned about");
  }
  // When poppler's core API cannot open the bytes, ReadAttachments says so
  // rather than reporting a document with no attachments; Parse turns that
  // into a trailer warning.
  {
    int emitted = 0;
    const grpc_poppler::AttachmentsRead read = grpc_poppler::ReadAttachments(
        "%PDF-1.7\nnot a document\n", std::nullopt, std::nullopt,
        [&emitted](pdfv1::AttachmentMeta&&, grpc_poppler::AttachmentData) {
          ++emitted;
          return true;
        });
    Check(read == grpc_poppler::AttachmentsRead::kDocumentUnreadable &&
              emitted == 0,
          "an unreadable document is reported, not listed as empty");
  }
  // Under a 4 KiB cap the attachment that inflates to 64 KiB is listed
  // without its data and the trailer says why; the others are unaffected.
  {
    std::map<std::string, pdfv1::AttachmentMeta> found;
    std::vector<pdfv1::ParseWarning> warnings;
    Check(parse_attachments(*tight_stub, encodings, &found, &warnings),
          "capped attachments stream finishes OK");
    Check(found.count("zeros.bin") == 1 && !found["zeros.bin"].has_data(),
          "an attachment over the cap is listed without data");
    Check(found.count(resume_name) == 1 && found[resume_name].has_data(),
          "an attachment under the cap keeps its data");
    Check(warnings.size() == 1 &&
              warnings[0].family() == pdfv1::PDF_FAMILY_ATTACHMENTS &&
              warnings[0].message().find("zeros.bin") != std::string::npos,
          "the trailer warns about the omitted data");
  }

  // Only requested families stream. A metadata-only request gets no page
  // chunks and no font table. A text-only request gets the document font
  // table's entries its cells cite, since a font_id must name a FontRef on
  // the stream, and no others.
  {
    // What a parse of rich.pdf for some families carried.
    struct Carried {
      std::map<pdfv1::ParseResponse::PayloadCase, int> messages;
      int cells = 0;
      // Cells whose font_id names no FontRef received before them.
      int dangling_font_ids = 0;
      bool ok = false;
    };
    auto carried = [&rich](pdfv1::PdfBackendService::Stub& client,
                           std::vector<pdfv1::PdfFamily> families) {
      grpc::ClientContext ctx;
      SetDeadline(&ctx);
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(rich);
      for (const auto family : families) request.add_families(family);
      auto reader = client.Parse(&ctx, request);
      pdfv1::ParseResponse msg;
      Carried out;
      std::set<uint32_t> font_ids;
      while (reader->Read(&msg)) {
        ++out.messages[msg.payload_case()];
        if (msg.has_fonts()) {
          for (const auto& font : msg.fonts().fonts()) {
            font_ids.insert(font.font_id());
          }
        }
        if (msg.has_page()) {
          out.cells += msg.page().text_cells_size();
          for (const auto& cell : msg.page().text_cells()) {
            if (cell.has_font_id() && font_ids.count(cell.font_id()) == 0) {
              ++out.dangling_font_ids;
            }
          }
        }
      }
      out.ok = reader->Finish().ok();
      return out;
    };
    Carried meta_only = carried(*stub, {pdfv1::PDF_FAMILY_DOC_METADATA});
    Check(meta_only.ok &&
              meta_only.messages[pdfv1::ParseResponse::kHeader] == 1 &&
              meta_only.messages[pdfv1::ParseResponse::kDocMeta] == 1 &&
              meta_only.messages[pdfv1::ParseResponse::kTrailer] == 1,
          "a metadata-only parse carries the header, metadata and trailer");
    Check(meta_only.messages[pdfv1::ParseResponse::kPage] == 0 &&
              meta_only.messages[pdfv1::ParseResponse::kFonts] == 0 &&
              meta_only.messages[pdfv1::ParseResponse::kAttachment] == 0,
          "a metadata-only parse sends no page chunks, fonts or attachments");
    Carried text_only = carried(*stub, {pdfv1::PDF_FAMILY_TEXT_CELLS});
    Check(text_only.ok && text_only.messages[pdfv1::ParseResponse::kPage] == 1 &&
              text_only.cells > 0,
          "a text-only parse carries the page's cells");
    Check(text_only.messages[pdfv1::ParseResponse::kFonts] >= 1 &&
              text_only.dangling_font_ids == 0,
          "a text-only parse sends each FontRef its cells cite, ahead of them");
  }

  // Font names and widget state names are PDF names, raw bytes in the
  // file. encodings.pdf names its font in GBK and gives a check box the
  // state #E9tat; both arrive as valid UTF-8 (the bytes of an ill-formed
  // sequence read as Latin-1), so the whole stream still parses.
  {
    grpc::ClientContext ctx;
    SetDeadline(&ctx);
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(encodings);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    std::map<uint32_t, std::string> font_names;
    std::vector<pdfv1::TextCell> cells;
    std::vector<pdfv1::FormField> fields;
    while (reader->Read(&msg)) {
      if (msg.has_fonts()) {
        for (const auto& font : msg.fonts().fonts()) {
          font_names[font.font_id()] = font.base_name();
        }
      } else if (msg.has_page()) {
        for (const auto& cell : msg.page().text_cells()) cells.push_back(cell);
        for (const auto& f : msg.page().form_fields()) fields.push_back(f);
      }
    }
    Check(reader->Finish().ok(), "encodings.pdf parses to the end");
    const std::string simsun_latin1 = "\xc3\x8b\xc3\x8e\xc3\x8c\xc3\xa5";
    bool font_listed = false;
    for (const auto& [id, name] : font_names) {
      if (name == simsun_latin1) font_listed = true;
    }
    Check(font_listed, "a GBK font name arrives as valid UTF-8");
    Check(cells.size() == 1 && cells[0].text() == "Hello" &&
              cells[0].has_font_id() &&
              font_names[cells[0].font_id()] == simsun_latin1,
          "the cell drawn in that font points at it");
    Check(fields.size() == 1 &&
              fields[0].appearance_state() == "/\xc3\xa9tat" &&
              fields[0].value() == "\xc3\xa9tat",
          "a widget state name that is not UTF-8 arrives as valid UTF-8");
  }

  // Page frames. frames.pdf draws one word with its baseline at (100, 700)
  // in user space under every /Rotate and with offset CropBoxes
  // (test/fixtures/make_frames_pdf.py). Geometry comes back in the
  // contract's page space, user space before /Rotate with the CropBox
  // origin included, so every page reports the upright page's box.
  {
    grpc::ClientContext ctx;
    SetDeadline(&ctx);
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(frames);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    std::vector<pdfv1::PageInfo> infos;
    std::map<uint32_t, std::vector<pdfv1::TextCell>> cells;
    std::map<uint32_t, std::vector<pdfv1::FormField>> fields;
    while (reader->Read(&msg)) {
      if (msg.has_header()) {
        infos.assign(msg.header().pages().begin(), msg.header().pages().end());
      } else if (msg.has_page()) {
        const uint32_t index = msg.page().page_index();
        for (const auto& cell : msg.page().text_cells()) {
          cells[index].push_back(cell);
        }
        for (const auto& field : msg.page().form_fields()) {
          fields[index].push_back(field);
        }
      }
    }
    Check(reader->Finish().ok(), "frames.pdf parses");
    Check(infos.size() == 10, "frames.pdf inventories ten pages");

    // rotation_degrees is the page's /Rotate (-90 is the same turn as
    // 270); the size is the CropBox's, turned with the page; the boxes are
    // reported as stored.
    const int32_t rotations[10] = {0, 90, 180, 270, 270, 0, 90, 180, 270, 0};
    for (size_t i = 0; i < infos.size() && i < 10; ++i) {
      Check(infos[i].rotation_degrees() == rotations[i],
            ("page " + std::to_string(i) + " reports its /Rotate").c_str());
    }
    if (infos.size() == 10) {
      Check(Near(infos[1].width_pts(), 792) && Near(infos[1].height_pts(), 612),
            "a quarter-turned page reports its turned size");
      Check(Near(infos[2].width_pts(), 612) && Near(infos[2].height_pts(), 792),
            "an upside-down page keeps its size");
      const auto& crop = infos[5].crop_box();
      Check(Near(crop.x0(), 36) && Near(crop.y0(), 36) &&
                Near(crop.x1(), 576) && Near(crop.y1(), 756),
            "the CropBox is reported as stored");
      Check(Near(infos[5].width_pts(), 540) && Near(infos[5].height_pts(), 720),
            "a cropped page reports the CropBox size");
      Check(Near(infos[6].width_pts(), 740) && Near(infos[6].height_pts(), 510),
            "a cropped, quarter-turned page reports the turned CropBox size");
      Check(Near(infos[6].media_box().x1(), 612) &&
                Near(infos[6].media_box().y1(), 792),
            "the MediaBox is reported as stored");
    }

    auto word = [&cells](uint32_t page) -> const pdfv1::TextCell* {
      for (const auto& cell : cells[page]) {
        if (cell.text() == "Frame") return &cell;
      }
      return nullptr;
    };
    // Helvetica's metrics give the word's user-space box: its advance
    // widths (F 611, r 333, a 556, m 833, e 556, 2.889 em) at 24 pt from
    // x 100, and the font's descent (-207) and ascent (718) about the
    // baseline at y 700.
    const double word_length = 2.889 * 24;
    const double descent = 0.207 * 24;
    const double ascent = 0.718 * 24;
    const pdfv1::TextCell* upright = word(0);
    Check(upright != nullptr, "the upright page carries the word");
    if (upright != nullptr) {
      const auto& b = upright->bbox();
      Check(Near(b.x0(), 100) && Near(b.y0(), 700 - descent) &&
                Near(b.x1(), 100 + word_length) && Near(b.y1(), 700 + ascent),
            "the upright word's box is its user-space box");
      Check(QuadReadsLeftToRight(upright->quad(), b),
            "the upright word's quad reads left to right");
    }
    for (uint32_t page = 1; page <= 8; ++page) {
      const pdfv1::TextCell* cell = word(page);
      const std::string name = "page " + std::to_string(page);
      Check(cell != nullptr, (name + " carries the word").c_str());
      if (cell == nullptr || upright == nullptr) continue;
      Check(SameBox(cell->bbox(), upright->bbox()),
            (name + " reports the upright page's box").c_str());
      Check(QuadReadsLeftToRight(cell->quad(), cell->bbox()),
            (name + " reports a quad that reads left to right").c_str());
    }

    // The widget rect is the stored /Rect, in the same space as the text:
    // the word drawn inside the field lies inside its rect.
    Check(fields[6].size() == 1, "the widget on the turned, cropped page");
    const pdfv1::TextCell* boxed = word(6);
    if (fields[6].size() == 1 && boxed != nullptr) {
      const auto& r = fields[6][0].rect();
      Check(Near(r.x0(), 90) && Near(r.y0(), 690) && Near(r.x1(), 260) &&
                Near(r.y1(), 730),
            "the widget rect is its /Rect");
      const auto& b = boxed->bbox();
      Check(b.x0() >= r.x0() && b.x1() <= r.x1() && b.y0() >= r.y0() &&
                b.y1() <= r.y1(),
            "the word lies inside the widget it was drawn in");
    }

    // A word turned a quarter counterclockwise reads up the page: its
    // baseline starts at (300, 400) and its lower edge is on the right, so
    // the quad starts at the box's lower-right corner and runs up.
    const pdfv1::TextCell* upward = word(9);
    Check(upward != nullptr, "the upward word is extracted");
    if (upward != nullptr) {
      const auto& b = upward->bbox();
      const auto& q = upward->quad();
      // Turned a quarter counterclockwise, the ascent reaches left of the
      // baseline at x 300 and the descent right of it.
      Check(Near(b.x0(), 300 - ascent) && Near(b.y0(), 400) &&
                Near(b.x1(), 300 + descent) && Near(b.y1(), 400 + word_length),
            "the upward word's box is its user-space box");
      Check(Near(q.x0(), b.x1()) && Near(q.y0(), b.y0()) &&
                Near(q.x1(), b.x1()) && Near(q.y1(), b.y1()) &&
                Near(q.x2(), b.x0()) && Near(q.y2(), b.y1()) &&
                Near(q.x3(), b.x0()) && Near(q.y3(), b.y0()),
            "the upward word's quad follows its reading direction");
    }
  }

  // missing_pages.pdf counts three pages and holds one
  // (test/fixtures/make_missing_pages_pdf.py), so poppler cannot load pages
  // 1 and 2. Parse leaves them out and names each in a trailer warning;
  // Render renders page 0 and then fails naming page 1.
  {
    grpc::ClientContext ctx;
    SetDeadline(&ctx);
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(missing_pages);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    int inventory = -1;
    std::vector<uint32_t> chunks;
    std::vector<uint32_t> warned_pages;
    while (reader->Read(&msg)) {
      if (msg.has_header()) inventory = msg.header().pages_size();
      if (msg.has_page()) chunks.push_back(msg.page().page_index());
      if (msg.has_trailer()) {
        for (const auto& warning : msg.trailer().warnings()) {
          if (warning.has_page_index()) {
            warned_pages.push_back(warning.page_index());
          }
        }
      }
    }
    Check(reader->Finish().ok(), "missing_pages.pdf parses");
    Check(inventory == 1 && chunks == std::vector<uint32_t>{0},
          "only the loadable page is inventoried and chunked");
    Check(warned_pages == std::vector<uint32_t>({1, 2}),
          "the trailer warns about each page poppler cannot load");
  }
  {
    grpc::ClientContext ctx;
    SetDeadline(&ctx);
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(missing_pages);
    request.set_dpi(36.0);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    std::vector<uint32_t> rasters;
    while (reader->Read(&msg)) {
      if (msg.has_raster()) rasters.push_back(msg.raster().page_index());
    }
    const grpc::Status status = reader->Finish();
    Check(rasters == std::vector<uint32_t>{0},
          "the loadable page renders before the failure");
    Check(status.error_code() == grpc::StatusCode::INTERNAL &&
              status.error_message().find("page 1") != std::string::npos,
          "a page poppler cannot load fails the Render, naming the page");
  }

  // Render hello.pdf at 72 DPI: BGR24, the same surface gRParse consumes.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(hello);
    request.set_dpi(72.0);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg), "render produced a raster");
    const auto& raster = msg.raster();
    Check(raster.width_px() == 612 && raster.height_px() == 792,
          "raster is Letter at 72 DPI");
    Check(raster.pixel_format() == pdfv1::PIXEL_FORMAT_BGR8, "raster is BGR8");
    bool has_ink = false;
    for (unsigned char b : raster.pixels()) {
      if (b != 0xFF) {
        has_ink = true;
        break;
      }
    }
    Check(has_ink, "raster has non-white pixels");
    Check(reader->Finish().ok(), "render finished OK");
  }

  // Render bounds. dpi must be a finite positive number no higher than the
  // service's maximum (1200 by default), and a page whose raster would
  // pass the pixel limit is refused with RESOURCE_EXHAUSTED before splash
  // allocates it. The tight service allows 150 dpi and exactly the pixels
  // of hello.pdf at 72 dpi.
  auto render_hello = [&hello](pdfv1::PdfBackendService::Stub& client,
                               double dpi, size_t* rasters) {
    grpc::ClientContext ctx;
    SetDeadline(&ctx);
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(hello);
    request.set_dpi(dpi);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    auto reader = client.Render(&ctx, request);
    pdfv1::RenderResponse msg;
    *rasters = 0;
    while (reader->Read(&msg)) {
      if (msg.has_raster()) ++*rasters;
    }
    return reader->Finish().error_code();
  };
  for (const double dpi : {0.0, -72.0, std::nan(""),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(), 1200.5,
                           1e9}) {
    size_t rasters = 0;
    Check(render_hello(*stub, dpi, &rasters) ==
                  grpc::StatusCode::INVALID_ARGUMENT &&
              rasters == 0,
          ("dpi " + std::to_string(dpi) + " is INVALID_ARGUMENT").c_str());
  }
  {
    size_t rasters = 0;
    Check(render_hello(*tight_stub, 72.0, &rasters) == grpc::StatusCode::OK &&
              rasters == 1,
          "a raster exactly at the pixel limit renders");
    Check(render_hello(*tight_stub, 73.0, &rasters) ==
                  grpc::StatusCode::RESOURCE_EXHAUSTED &&
              rasters == 0,
          "a raster past the pixel limit is RESOURCE_EXHAUSTED");
    Check(render_hello(*tight_stub, 150.0, &rasters) ==
                  grpc::StatusCode::RESOURCE_EXHAUSTED &&
              rasters == 0,
          "the dpi maximum itself still meets the pixel limit");
    Check(render_hello(*tight_stub, 150.5, &rasters) ==
                  grpc::StatusCode::INVALID_ARGUMENT &&
              rasters == 0,
          "a dpi above the service's maximum is INVALID_ARGUMENT");
  }

  // A client that stops reading must not stall the service. On arm64, where
  // poppler calls run one at a time behind a process-wide gate, the gate
  // is released while a write waits on the client. A render of frames.pdf
  // at 300 dpi (25 MB a page) is left unread after its first raster, and a
  // probe on a connection of its own must still be answered; the stalled
  // render then ends cancelled.
  {
    grpc::ChannelArguments render_args;
    render_args.SetMaxReceiveMessageSize(-1);
    auto render_stub =
        pdfv1::PdfBackendService::NewStub(grpc::CreateCustomChannel(
            "127.0.0.1:" + std::to_string(port),
            grpc::InsecureChannelCredentials(), render_args));
    grpc::ClientContext render_ctx;
    SetDeadline(&render_ctx);
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(frames);
    request.set_dpi(300.0);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    auto stalled = render_stub->Render(&render_ctx, request);
    pdfv1::RenderResponse raster_msg;
    Check(stalled->Read(&raster_msg) && raster_msg.has_raster(),
          "the render to be stalled sends its first raster");
    // Time for the server to render the next page and block writing it.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    grpc::ChannelArguments probe_args;
    probe_args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    auto probe_stub =
        pdfv1::PdfBackendService::NewStub(grpc::CreateCustomChannel(
            "127.0.0.1:" + std::to_string(port),
            grpc::InsecureChannelCredentials(), probe_args));
    grpc::ClientContext probe_ctx;
    probe_ctx.set_deadline(std::chrono::system_clock::now() +
                           std::chrono::seconds(20));
    pdfv1::ProbeRequest probe;
    probe.mutable_document()->set_data(hello);
    pdfv1::ProbeResponse probe_response;
    Check(probe_stub->Probe(&probe_ctx, probe, &probe_response).ok() &&
              probe_response.capabilities().load_status() ==
                  pdfv1::LOAD_STATUS_OK,
          "a probe is answered while another client stalls a render");

    render_ctx.TryCancel();
    while (stalled->Read(&raster_msg)) {
    }
    Check(stalled->Finish().error_code() == grpc::StatusCode::CANCELLED,
          "the stalled render ends cancelled");
  }

  // PageRange is zero-based and half-open, and a set range needs end
  // greater than begin.
  {
    struct BadRange {
      uint32_t begin;
      uint32_t end;
      std::string name;
    };
    const std::vector<BadRange> bad_ranges = {
        {4294967295u, 1u, "begin 2^32-1, end 1"},
        {1u, 1u, "end equal to begin"},
        {3u, 2u, "end below begin"},
    };
    for (const BadRange& bad : bad_ranges) {
      {
        grpc::ClientContext ctx;
        SetDeadline(&ctx);
        pdfv1::ParseRequest request;
        request.mutable_document()->set_data(hello);
        request.mutable_pages()->set_begin(bad.begin);
        request.mutable_pages()->set_end(bad.end);
        auto reader = stub->Parse(&ctx, request);
        pdfv1::ParseResponse msg;
        Check(!reader->Read(&msg),
              ("parse sends nothing for " + bad.name).c_str());
        // Drain whatever a regression sends: Finish waits for an unread
        // stream.
        while (reader->Read(&msg)) {
        }
        Check(reader->Finish().error_code() ==
                  grpc::StatusCode::INVALID_ARGUMENT,
              ("parse range " + bad.name + " is INVALID_ARGUMENT").c_str());
      }
      {
        grpc::ClientContext ctx;
        SetDeadline(&ctx);
        pdfv1::RenderRequest request;
        request.mutable_document()->set_data(hello);
        request.set_dpi(72.0);
        request.mutable_pages()->set_begin(bad.begin);
        request.mutable_pages()->set_end(bad.end);
        auto reader = stub->Render(&ctx, request);
        pdfv1::RenderResponse msg;
        Check(!reader->Read(&msg),
              ("render sends nothing for " + bad.name).c_str());
        // Drain whatever a regression sends: Finish waits for an unread
        // stream.
        while (reader->Read(&msg)) {
        }
        Check(reader->Finish().error_code() ==
                  grpc::StatusCode::INVALID_ARGUMENT,
              ("render range " + bad.name + " is INVALID_ARGUMENT").c_str());
      }
    }
  }
  // An end past the document is clamped to it, and a begin past it selects
  // no page. A begin of 2^31 or more is a valid range too: it used to wrap
  // to a negative index and crash the process.
  {
    struct GoodRange {
      uint32_t begin;
      uint32_t end;
      size_t pages;
      std::string name;
    };
    const std::vector<GoodRange> good_ranges = {
        {0u, 4294967295u, 1, "end 2^32-1"},
        {0u, 1u, 1, "the one page"},
        {5u, 9u, 0, "past the last page"},
        {2147483647u, 4294967295u, 0, "begin 2^31-1"},
        {2147483648u, 4294967295u, 0, "begin 2^31"},
        {4294967294u, 4294967295u, 0, "begin 2^32-2"},
    };
    for (const GoodRange& good : good_ranges) {
      {
        grpc::ClientContext ctx;
        SetDeadline(&ctx);
        pdfv1::ParseRequest request;
        request.mutable_document()->set_data(hello);
        request.mutable_pages()->set_begin(good.begin);
        request.mutable_pages()->set_end(good.end);
        auto reader = stub->Parse(&ctx, request);
        pdfv1::ParseResponse msg;
        size_t pages = 0;
        bool saw_trailer = false;
        while (reader->Read(&msg)) {
          if (msg.has_page()) ++pages;
          if (msg.has_trailer()) saw_trailer = true;
        }
        Check(reader->Finish().ok() && saw_trailer,
              ("parse range " + good.name + " finishes OK").c_str());
        Check(pages == good.pages,
              ("parse range " + good.name + " selects its pages").c_str());
      }
      {
        grpc::ClientContext ctx;
        SetDeadline(&ctx);
        pdfv1::RenderRequest request;
        request.mutable_document()->set_data(hello);
        request.set_dpi(36.0);
        request.mutable_pages()->set_begin(good.begin);
        request.mutable_pages()->set_end(good.end);
        auto reader = stub->Render(&ctx, request);
        pdfv1::RenderResponse msg;
        size_t rasters = 0;
        while (reader->Read(&msg)) {
          if (msg.has_raster()) ++rasters;
        }
        Check(reader->Finish().ok(),
              ("render range " + good.name + " finishes OK").c_str());
        Check(rasters == good.pages,
              ("render range " + good.name + " selects its pages").c_str());
      }
    }
  }

  // The content-addressed handshake (PdfDocument.sha256).
  const std::string hello_hash = grpc_poppler::Sha256Hex(hello);
  const std::string rich_hash = grpc_poppler::Sha256Hex(rich);
  Check(hello_hash.size() == 64 && hello_hash != rich_hash,
        "fixture hashes computed");

  // A first call addressed by hash misses with BYTES_REQUIRED, on every
  // surface, and never as a gRPC error.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(),
          "probe by hash is not a gRPC error");
    Check(response.capabilities().load_status() ==
              pdfv1::LOAD_STATUS_BYTES_REQUIRED,
          "probe by hash misses with BYTES_REQUIRED");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    Check(reader->Read(&msg) && msg.has_header(),
          "parse by hash miss still sends the header");
    Check(msg.header().capabilities().load_status() ==
              pdfv1::LOAD_STATUS_BYTES_REQUIRED,
          "parse header carries BYTES_REQUIRED");
    Check(!reader->Read(&msg), "stream ends after the failed header");
    Check(reader->Finish().ok(), "parse by hash miss finished OK");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg) && msg.has_head(),
          "render by hash miss sends a head message");
    Check(msg.head().load_status() == pdfv1::LOAD_STATUS_BYTES_REQUIRED,
          "render head carries BYTES_REQUIRED");
    Check(!reader->Read(&msg), "render stream ends after the head");
    Check(reader->Finish().ok(), "render by hash miss finished OK");
  }

  // Empty data with no hash is INVALID_ARGUMENT on every RPC.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).error_code() ==
              grpc::StatusCode::INVALID_ARGUMENT,
          "empty data without sha256 is INVALID_ARGUMENT");
  }

  // data plus a hash that does not match is HASH_MISMATCH.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(hello);
    request.mutable_document()->set_sha256(rich_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(),
          "hash mismatch is not a gRPC error");
    Check(response.capabilities().load_status() ==
              pdfv1::LOAD_STATUS_HASH_MISMATCH,
          "mismatched hash reports HASH_MISMATCH");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(hello);
    request.mutable_document()->set_sha256(rich_hash);
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg) && msg.has_head(),
          "render hash mismatch sends a head message");
    Check(msg.head().load_status() == pdfv1::LOAD_STATUS_HASH_MISMATCH,
          "render head carries HASH_MISMATCH");
    Check(!reader->Read(&msg), "render stream ends after the mismatch head");
  }

  // The retry with bytes succeeds and caches the document.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(hello);
    request.mutable_document()->set_sha256(hello_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "retry with bytes OK");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "retry with bytes loads");
  }

  // Every RPC now works by hash alone, no bytes on the wire.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "cached probe OK");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "cached probe loads");
    Check(response.capabilities().page_count() == 1,
          "cached probe counts the page");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    Check(reader->Read(&msg) && msg.has_header(), "cached parse header");
    Check(msg.header().capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "cached parse loads");
    Check(msg.header().pages_size() == 1, "cached parse inventories the page");
    bool saw_trailer = false;
    while (reader->Read(&msg)) {
      if (msg.has_trailer()) saw_trailer = true;
    }
    Check(saw_trailer, "cached parse ran to the trailer");
    Check(reader->Finish().ok(), "cached parse finished OK");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg) && msg.has_raster(),
          "cached render produced a raster");
    Check(msg.raster().width_px() == 612, "cached raster is Letter");
    Check(reader->Finish().ok(), "cached render finished OK");
  }

  // A capacity-one cache evicts the older document.
  {
    grpc_poppler::PopplerServiceImpl tiny_service({1, 1u << 30});
    grpc::ServerBuilder tiny_builder;
    int tiny_port = 0;
    tiny_builder.AddListeningPort("127.0.0.1:0",
                                  grpc::InsecureServerCredentials(),
                                  &tiny_port);
    tiny_builder.RegisterService(&tiny_service);
    std::unique_ptr<grpc::Server> tiny_server = tiny_builder.BuildAndStart();
    Check(tiny_server != nullptr && tiny_port != 0, "tiny-cache server up");
    auto tiny_stub = pdfv1::PdfBackendService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(tiny_port),
                            grpc::InsecureChannelCredentials()));
    for (const auto& item : {std::pair{&hello, &hello_hash},
                             std::pair{&rich, &rich_hash}}) {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_data(*item.first);
      request.mutable_document()->set_sha256(*item.second);
      pdfv1::ProbeResponse response;
      Check(tiny_stub->Probe(&ctx, request, &response).ok() &&
                response.capabilities().load_status() ==
                    pdfv1::LOAD_STATUS_OK,
            "tiny cache stores each upload");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_sha256(hello_hash);
      pdfv1::ProbeResponse response;
      Check(tiny_stub->Probe(&ctx, request, &response).ok(),
            "evicted probe is not a gRPC error");
      Check(response.capabilities().load_status() ==
                pdfv1::LOAD_STATUS_BYTES_REQUIRED,
            "the older document was evicted");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_sha256(rich_hash);
      pdfv1::ProbeResponse response;
      Check(tiny_stub->Probe(&ctx, request, &response).ok() &&
                response.capabilities().load_status() ==
                    pdfv1::LOAD_STATUS_OK,
            "the newest document survives eviction");
    }
    tiny_server->Shutdown();
  }

  tight_server->Shutdown();
  server->Shutdown();
  if (failures == 0) {
    std::printf("poppler_contract: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "poppler_contract: %d check(s) failed\n", failures);
  return 1;
}
