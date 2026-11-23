// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"

namespace grpc_poppler {

// Takes over poppler's error callback for the life of the process. poppler
// has one callback for the whole process and installs it only when its
// first GlobalParamsIniter is made, so this holds an initializer of its own
// for good: the callback stays ours whatever poppler-cpp documents come and
// go. PopplerErrorScope calls it; the core API readers call it too, since
// it also keeps globalParams alive for them. Safe to call from any thread,
// any number of times.
void EnsurePopplerErrorCapture();

// What poppler reported on this thread while the scope is open.
//
// poppler tells of a damaged content stream, a stream that does not
// decode, an xref table it had to rebuild and the like only through its
// error callback, and returns whatever it could still read. A handler
// opens one scope before its first poppler call and takes what was
// reported after each piece of work, as typed warnings on the page and the
// family it was working on, so a partly read page is never handed out as a
// whole one.
//
// The messages of one piece of work are bounded: a message poppler repeats
// is kept once with the number of times it was said, and past
// kMaxDistinctMessages different messages the rest are counted, not kept.
// Whatever is still held when the scope closes goes to stderr, as poppler
// would have written it.
//
// A scope collects on the thread that opened it and must be closed on that
// thread. Scopes nest; the innermost collects.
class PopplerErrorScope {
 public:
  static constexpr size_t kMaxDistinctMessages = 16;
  static constexpr size_t kMaxMessageBytes = 512;

  PopplerErrorScope();
  ~PopplerErrorScope();
  PopplerErrorScope(const PopplerErrorScope&) = delete;
  PopplerErrorScope& operator=(const PopplerErrorScope&) = delete;

  // Moves what poppler reported since the last Take into warnings, each
  // stamped with the page and the family given. With no page and no
  // family the warnings are about the document as a whole; what such a
  // call takes is remembered as what loading the document said.
  void Take(std::optional<uint32_t> page_index,
            std::optional<ai::protomolt::parse::pdf::v1::PdfFamily> family,
            std::vector<ai::protomolt::parse::pdf::v1::ParseWarning>* warnings);

  // Take for work that opened the same bytes a second time (the core API
  // readers do): what loading the document already said is not said again.
  void TakeAfterReopen(
      std::optional<uint32_t> page_index,
      std::optional<ai::protomolt::parse::pdf::v1::PdfFamily> family,
      std::vector<ai::protomolt::parse::pdf::v1::ParseWarning>* warnings);

  // Called by the process callback; not for handlers.
  void Collect(int category, int64_t position, const char* message);

 private:
  struct Entry {
    int category = 0;
    int64_t position = -1;
    std::string message;
    uint64_t times = 0;
  };

  void TakeInto(
      std::optional<uint32_t> page_index,
      std::optional<ai::protomolt::parse::pdf::v1::PdfFamily> family,
      bool reopened,
      std::vector<ai::protomolt::parse::pdf::v1::ParseWarning>* warnings);
  bool SaidByLoad(const Entry& entry) const;

  PopplerErrorScope* outer_;
  std::vector<Entry> entries_;
  uint64_t unlisted_ = 0;
  // What a document-level Take took, for TakeAfterReopen.
  std::vector<Entry> load_entries_;
};

}  // namespace grpc_poppler
