#pragma once

#include <cstdint>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "document_cache.h"

namespace grpc_poppler {

// Bounds on how much one request can make the service decode. The defaults
// keep every message inside the 520 MiB limit the server runs with
// (src/main.cpp); tests pass tighter ones.
struct ResourceLimits {
  // Most bytes one attachment may decode to. A larger attachment is sent
  // without its data, and the trailer carries a ParseWarning.
  uint64_t max_attachment_bytes = 256ull * 1024 * 1024;
};

// PdfBackendService over poppler-cpp: the extraction-quality reference and
// the GPL differential leg. Mirrors the exact poppler-cpp usage of
// gRParse's in-process path for the tier 0 floor (text boxes with fonts,
// BGR24 rasters, page geometry), with the text boxes mapped into the
// contract's page space (user space before /Rotate), and adds the
// document-level families the cpp API carries: info keys and XMP,
// permissions, the outline, and the document font table. Embedded files
// and AcroForm widgets come from poppler's core API, which the cpp wrapper
// does not expose (src/poppler_attachments.h, src/poppler_forms.h).
//
// The document handshake (PdfDocument.sha256) is served from an in-process
// byte cache: this service is single-process, so the cache lives beside the
// RPC surface.
class PopplerServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
  // Cache limits from GRPC_POPPLER_CACHE_MAX_DOCUMENTS /
  // GRPC_POPPLER_CACHE_MAX_BYTES.
  PopplerServiceImpl() : cache_(DocumentCache::LimitsFromEnv()) {}
  // Explicit cache limits and resource bounds, for tests.
  explicit PopplerServiceImpl(DocumentCache::Limits cache_limits,
                              ResourceLimits limits = ResourceLimits())
      : cache_(cache_limits), limits_(limits) {}

  grpc::Status Probe(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ProbeRequest* request,
      ai::protomolt::parse::pdf::v1::ProbeResponse* response) override;

  grpc::Status Parse(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ParseRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::ParseResponse>*
          writer) override;

  grpc::Status Render(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::RenderRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::RenderResponse>*
          writer) override;

  grpc::Status GetServiceInfo(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ServiceInfoRequest* request,
      ai::protomolt::parse::pdf::v1::ServiceInfoResponse* response) override;

 private:
  DocumentCache cache_;
  const ResourceLimits limits_;
};

}  // namespace grpc_poppler
