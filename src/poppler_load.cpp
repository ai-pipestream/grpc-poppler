// SPDX-License-Identifier: GPL-3.0-or-later

#include "poppler_load.h"

#include <memory>

// poppler's core headers, installed by its ENABLE_UNSTABLE_API_ABI_HEADERS
// option; poppler.pc puts <includedir>/poppler on the include path.
#include <ErrorCodes.h>
#include <Object.h>
#include <PDFDoc.h>
#include <Stream.h>
#include <goo/GooString.h>

#include "poppler_errors.h"

namespace grpc_poppler {

static_assert(kCoreErrorNone == errNone && kCoreErrorBadCatalog == errBadCatalog &&
                  kCoreErrorDamaged == errDamaged &&
                  kCoreErrorEncrypted == errEncrypted,
              "poppler's load error codes moved");

int CoreLoadErrorCode(const std::string& data,
                      const std::optional<std::string>& password) {
  EnsurePopplerErrorCapture();
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
  return doc.isOk() ? errNone : doc.getErrorCode();
}

const char* CoreLoadErrorWords(int code) {
  switch (code) {
    case errNone:
      return "no error";
    case errOpenFile:
      return "could not open the file";
    case errBadCatalog:
      return "could not read the page catalog";
    case errDamaged:
      return "damaged file";
    case errEncrypted:
      return "encrypted, not authorized";
    case errFileIO:
      return "file I/O error";
    default:
      return "unexpected code";
  }
}

}  // namespace grpc_poppler
