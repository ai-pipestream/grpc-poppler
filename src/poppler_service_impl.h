#pragma once

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grpc_poppler {

// PdfBackendService over poppler-cpp: the extraction-quality reference and
// the GPL differential leg. Mirrors the exact poppler-cpp usage of
// gRParse's in-process path for the tier 0 floor (text boxes with fonts,
// BGR24 rasters, quarter-turn geometry) and adds the document-level
// families the cpp API carries: info keys and XMP, permissions, the
// outline, embedded files, and the document font table.
class PopplerServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
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
};

}  // namespace grpc_poppler
