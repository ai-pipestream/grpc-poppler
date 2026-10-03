#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

namespace grpc_poppler {

// What became of one attachment's bytes.
enum class AttachmentData {
  // The request did not ask for them.
  kNotRequested,
  // AttachmentMeta.data holds them.
  kIncluded,
  // They decode to more than the cap; data is left unset.
  kOverLimit,
  // The file spec carries no readable embedded stream; data is left unset.
  kUnavailable,
};

// Visits the document's embedded files (the EmbeddedFiles name tree, in
// the order poppler-cpp's document::embedded_files() lists them), calling
// emit once per valid file spec and stopping as soon as it returns false;
// the return value is false exactly then.
//
// This reads poppler's core API, through a core document opened over the
// same bytes, because the cpp wrapper hands out neither the raw name
// strings nor the stream: embedded_file::name() cuts a UTF-16 name at its
// first NUL byte, and embedded_file::data() inflates the whole stream into
// memory before returning it. Here names and descriptions are decoded as
// PDF text strings (PDFDocEncoding, or UTF-16 behind a byte order mark),
// the MIME type (the stream's /Subtype name) is made valid UTF-8, and with
// max_data_bytes set the bytes are decoded block by block and dropped once
// they pass the cap, so a stream that inflates without bound costs at most
// the cap plus one block.
bool ReadAttachments(
    const std::string& data, const std::optional<std::string>& password,
    std::optional<uint64_t> max_data_bytes,
    const std::function<bool(ai::protomolt::parse::pdf::v1::AttachmentMeta&&,
                             AttachmentData)>& emit);

}  // namespace grpc_poppler
