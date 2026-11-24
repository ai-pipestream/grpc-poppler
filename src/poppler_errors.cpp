// SPDX-License-Identifier: GPL-3.0-or-later

#include "poppler_errors.h"

#include <cstdio>
#include <mutex>
#include <utility>

// poppler's core headers, installed by its ENABLE_UNSTABLE_API_ABI_HEADERS
// option; poppler.pc puts <includedir>/poppler on the include path.
#include <Error.h>
#include <GlobalParams.h>

#include "utf8.h"

namespace grpc_poppler {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// The scope collecting on this thread, if any.
thread_local PopplerErrorScope* current_scope = nullptr;

// poppler's ErrorCategory in words, for a warning's message.
const char* CategoryWords(int category) {
  switch (category) {
    case errSyntaxWarning:
      return "syntax warning";
    case errSyntaxError:
      return "syntax error";
    case errConfig:
      return "configuration error";
    case errCommandLine:
      return "command line error";
    case errIO:
      return "I/O error";
    case errNotAllowed:
      return "permission error";
    case errUnimplemented:
      return "unimplemented feature";
    case errInternal:
      return "internal error";
    default:
      return "error";
  }
}

// One line as poppler's own handler would put it, for stderr.
void WriteToStderr(int category, int64_t position, const std::string& message,
                   uint64_t times) {
  std::string line = std::string("poppler ") + CategoryWords(category);
  if (position >= 0) line += " (" + std::to_string(position) + ")";
  line += ": " + message;
  if (times > 1) line += " (" + std::to_string(times) + " times)";
  std::fprintf(stderr, "%s\n", line.c_str());
}

void OnPopplerError(ErrorCategory category, Goffset position,
                    const char* message) {
  if (current_scope != nullptr) {
    current_scope->Collect(static_cast<int>(category),
                           static_cast<int64_t>(position), message);
    return;
  }
  // poppler work outside any request's scope: nobody to tell but the log.
  WriteToStderr(static_cast<int>(category), static_cast<int64_t>(position),
                message != nullptr ? message : "", 1);
}

}  // namespace

void EnsurePopplerErrorCapture() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Never freed: it must outlive every poppler object, including any
    // that static destructors still hold at exit.
    new GlobalParamsIniter(OnPopplerError);
    // The initializer installs its callback only when it is the first one
    // alive; this covers a process that made a poppler document before.
    setErrorCallback(OnPopplerError);
  });
}

PopplerErrorScope::PopplerErrorScope() : outer_(current_scope) {
  EnsurePopplerErrorCapture();
  current_scope = this;
}

PopplerErrorScope::~PopplerErrorScope() {
  current_scope = outer_;
  for (const Entry& entry : entries_) {
    WriteToStderr(entry.category, entry.position, entry.message, entry.times);
  }
  if (unlisted_ > 0) {
    std::fprintf(stderr, "poppler reported %llu more message(s)\n",
                 static_cast<unsigned long long>(unlisted_));
  }
}

void PopplerErrorScope::Collect(int category, int64_t position,
                                const char* message) {
  // poppler has already replaced every byte outside printable ASCII, so
  // the text is valid UTF-8; ValidUtf8 keeps that true should it stop.
  std::string text = ValidUtf8(message != nullptr ? message : "");
  if (text.size() > kMaxMessageBytes) {
    text.resize(kMaxMessageBytes);
    text = ValidUtf8(text) + "...";
  }
  for (Entry& entry : entries_) {
    if (entry.category == category && entry.message == text) {
      ++entry.times;
      return;
    }
  }
  if (entries_.size() >= kMaxDistinctMessages) {
    ++unlisted_;
    return;
  }
  Entry& entry = entries_.emplace_back();
  entry.category = category;
  entry.position = position;
  entry.message = std::move(text);
  entry.times = 1;
}

const std::string* PopplerErrorScope::Find(
    std::initializer_list<std::string_view> prefixes) const {
  for (const Entry& entry : entries_) {
    if (prefixes.size() == 0) return &entry.message;
    for (const std::string_view prefix : prefixes) {
      if (entry.message.compare(0, prefix.size(), prefix) == 0) {
        return &entry.message;
      }
    }
  }
  return nullptr;
}

bool PopplerErrorScope::SaidByLoad(const Entry& entry) const {
  for (const Entry& said : load_entries_) {
    if (said.category == entry.category && said.position == entry.position &&
        said.message == entry.message) {
      return true;
    }
  }
  return false;
}

void PopplerErrorScope::Take(std::optional<uint32_t> page_index,
                             std::optional<pdfv1::PdfFamily> family,
                             std::vector<pdfv1::ParseWarning>* warnings) {
  TakeInto(page_index, family, false, warnings);
}

void PopplerErrorScope::TakeAfterReopen(
    std::optional<uint32_t> page_index, std::optional<pdfv1::PdfFamily> family,
    std::vector<pdfv1::ParseWarning>* warnings) {
  TakeInto(page_index, family, true, warnings);
}

void PopplerErrorScope::TakeInto(std::optional<uint32_t> page_index,
                                 std::optional<pdfv1::PdfFamily> family,
                                 bool reopened,
                                 std::vector<pdfv1::ParseWarning>* warnings) {
  const auto stamp = [&](pdfv1::ParseWarning* warning) {
    if (page_index.has_value()) warning->set_page_index(*page_index);
    if (family.has_value()) warning->set_family(*family);
  };
  for (Entry& entry : entries_) {
    if (reopened && SaidByLoad(entry)) continue;
    pdfv1::ParseWarning& warning = warnings->emplace_back();
    stamp(&warning);
    std::string message = std::string("poppler ") + CategoryWords(entry.category);
    if (entry.position >= 0) {
      message += " at stream position " + std::to_string(entry.position);
    }
    message += ": " + entry.message;
    if (entry.times > 1) {
      message += " (" + std::to_string(entry.times) +
                 " times; the position is the first)";
    }
    warning.set_message(std::move(message));
  }
  if (unlisted_ > 0) {
    pdfv1::ParseWarning& warning = warnings->emplace_back();
    stamp(&warning);
    warning.set_message("poppler reported " + std::to_string(unlisted_) +
                        " more message(s) here, past the " +
                        std::to_string(kMaxDistinctMessages) +
                        " different ones listed");
  }
  if (!page_index.has_value() && !family.has_value()) {
    for (Entry& entry : entries_) load_entries_.push_back(std::move(entry));
  }
  entries_.clear();
  unlisted_ = 0;
}

}  // namespace grpc_poppler
