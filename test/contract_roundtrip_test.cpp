// Contract test for grpc-poppler: the tier 0 floor plus the cpp-surface
// document families (metadata with XMP, outline, attachments, fonts) over
// the hello.pdf and rich.pdf fixtures.

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "poppler_service_impl.h"

namespace pdfv1 = ai::pipestream::parse::pdf::v1;

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
    std::map<int, uint64_t> counts;
    while (reader->Read(&msg)) {
      if (msg.has_page()) {
        for (const auto& cell : msg.page().text_cells()) {
          all_text += cell.text() + " ";
        }
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

  server->Shutdown();
  if (failures == 0) {
    std::printf("poppler_contract: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "poppler_contract: %d check(s) failed\n", failures);
  return 1;
}
