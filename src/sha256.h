#pragma once

#include <string>

namespace grpc_poppler {

// Lowercase hex SHA-256 of data, the content-address key of the document
// handshake (PdfDocument.sha256). Backed by the boringssl crypto target the
// gRPC build already compiles; no new dependency.
std::string Sha256Hex(const std::string& data);

}  // namespace grpc_poppler
