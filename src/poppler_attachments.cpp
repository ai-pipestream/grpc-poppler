// SPDX-License-Identifier: GPL-3.0-or-later

#include "poppler_attachments.h"

#include <memory>
#include <utility>

// poppler's core headers, installed by its ENABLE_UNSTABLE_API_ABI_HEADERS
// option; poppler.pc puts <includedir>/poppler on the include path.
#include <Catalog.h>
#include <Error.h>
#include <FileSpec.h>
#include <GlobalParams.h>
#include <Object.h>
#include <PDFDoc.h>
#include <Stream.h>
#include <goo/GooString.h>

#include "utf8.h"

namespace grpc_poppler {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

std::string TextString(const GooString* text) {
  if (text == nullptr) return {};
  return PdfTextStringToUtf8(text->toStr());
}

// Decodes an embedded file stream into *out. False, with *out cleared, once
// the bytes pass max_bytes; reading stops there. Once a stream has
// rewound, poppler's decoders report damage only by ending it, so a true
// return does not prove the bytes whole.
bool ReadCapped(Stream* stream, uint64_t max_bytes, std::string* out) {
  unsigned char block[64 * 1024];
  for (;;) {
    const int read = stream->doGetChars(static_cast<int>(sizeof(block)), block);
    if (read <= 0) return true;
    if (out->size() + static_cast<uint64_t>(read) > max_bytes) {
      out->clear();
      out->shrink_to_fit();
      return false;
    }
    out->append(reinterpret_cast<const char*>(block),
                static_cast<size_t>(read));
  }
}

}  // namespace

AttachmentsRead ReadAttachments(
    const std::string& data, const std::optional<std::string>& password,
    std::optional<uint64_t> max_data_bytes,
    const std::function<bool(pdfv1::AttachmentMeta&&, AttachmentData)>& emit) {
  // poppler-cpp documents derive from this reference-counted initializer;
  // holding one here keeps globalParams alive while the core document is
  // open. Should this be the first holder, diagnostics stay quiet.
  GlobalParamsIniter params([](ErrorCategory, Goffset, const char*) {});
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
  if (!doc.isOk()) return AttachmentsRead::kDocumentUnreadable;
  Catalog* catalog = doc.getCatalog();
  const int files = catalog->numEmbeddedFiles();
  for (int i = 0; i < files; ++i) {
    std::unique_ptr<FileSpec> spec = catalog->embeddedFile(i);
    if (spec == nullptr || !spec->isOk()) continue;
    pdfv1::AttachmentMeta meta;
    meta.set_name(TextString(spec->getFileName()));
    if (std::string description = TextString(spec->getDescription());
        !description.empty()) {
      meta.set_description(std::move(description));
    }
    AttachmentData outcome = max_data_bytes.has_value()
                                 ? AttachmentData::kUnavailable
                                 : AttachmentData::kNotRequested;
    EmbFile* file = spec->getEmbeddedFile();
    if (file != nullptr && file->isOk()) {
      if (file->size() >= 0) {
        meta.set_size_bytes(static_cast<uint64_t>(file->size()));
      }
      if (const GooString* mime = file->mimeType();
          mime != nullptr && !mime->toStr().empty()) {
        meta.set_mime_type(ValidUtf8(mime->toStr()));
      }
      Stream* stream = file->stream();
      if (max_data_bytes.has_value() && stream != nullptr) {
        // rewind() is where a FlateDecode stream reads its zlib header; one
        // it does not recognise fails here, before a byte is decoded.
        if (!stream->rewind()) {
          outcome = AttachmentData::kUndecodable;
        } else if (std::string bytes;
                   ReadCapped(stream, *max_data_bytes, &bytes)) {
          meta.set_data(std::move(bytes));
          outcome = AttachmentData::kIncluded;
        } else {
          outcome = AttachmentData::kOverLimit;
        }
      }
    }
    if (!emit(std::move(meta), outcome)) return AttachmentsRead::kStopped;
  }
  return AttachmentsRead::kDone;
}

}  // namespace grpc_poppler
