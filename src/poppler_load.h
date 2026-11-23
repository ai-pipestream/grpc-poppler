// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>

namespace grpc_poppler {

// poppler's own verdict on a document poppler-cpp did not open: the
// PDFDoc::getErrorCode() of a core document opened over the same bytes
// with the same password (ErrorCodes.h: 0 none, 2 bad catalog, 3 damaged,
// 4 encrypted and not authorized, 10 file I/O). poppler-cpp keeps only
// "locked or not" of it, which is not enough to type a load failure.
// What poppler reports while opening goes to the caller's
// PopplerErrorScope, as it did for the poppler-cpp open.
int CoreLoadErrorCode(const std::string& data,
                      const std::optional<std::string>& password);

// The codes of poppler's ErrorCodes.h that type a load failure. That
// header defines them as macros, so they are repeated here rather than
// pulled into every includer; poppler_load.cpp checks them against it.
inline constexpr int kCoreErrorNone = 0;
inline constexpr int kCoreErrorBadCatalog = 2;
inline constexpr int kCoreErrorDamaged = 3;
inline constexpr int kCoreErrorEncrypted = 4;

// The code in words, for a load_detail.
const char* CoreLoadErrorWords(int code);

}  // namespace grpc_poppler
