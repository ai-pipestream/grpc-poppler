// Contract test for grpc-poppler: the tier 0 floor plus the cpp-surface
// document families (metadata with XMP, outline, attachments, fonts) over
// the hello.pdf and rich.pdf fixtures.

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const std::string hello = ReadFile(dir + "/hello.pdf");
  const std::string rich = ReadFile(dir + "/rich.pdf");
  Check(!hello.empty() && !rich.empty(), "fixtures read");

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
    while (reader->Read(&msg)) {
      if (msg.has_page()) {
        for (const auto& cell : msg.page().text_cells()) {
          all_text += cell.text() + " ";
        }
        for (const auto& f : msg.page().form_fields()) form_fields.push_back(f);
      } else if (msg.has_doc_meta()) {
        title = msg.doc_meta().title();
        xmp = msg.doc_meta().xmp_xml();
      } else if (msg.has_outline()) {
        outline_roots = msg.outline().roots_size();
      } else if (msg.has_attachment()) {
        ++attachments;
        attachment_data = msg.attachment().data();
      } else if (msg.has_fonts()) {
        for (const auto& f : msg.fonts().fonts()) {
          if (f.base_name().find("Helvetica") != std::string::npos) {
            font_named = true;
          }
        }
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

  server->Shutdown();
  if (failures == 0) {
    std::printf("poppler_contract: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "poppler_contract: %d check(s) failed\n", failures);
  return 1;
}
