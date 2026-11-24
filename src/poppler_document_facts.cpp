// SPDX-License-Identifier: GPL-3.0-or-later

#include "poppler_document_facts.h"

#include <cstdio>
#include <memory>
#include <utility>

// poppler's core headers, installed by its ENABLE_UNSTABLE_API_ABI_HEADERS
// option; poppler.pc puts <includedir>/poppler on the include path.
#include <Catalog.h>
#include <CharTypes.h>
#include <Dict.h>
#include <Link.h>
#include <Object.h>
#include <Outline.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Stream.h>
#include <XRef.h>
#include <goo/GooString.h>

#include "poppler_errors.h"
#include "utf8.h"

namespace grpc_poppler {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// An outline title, UCS-4 as poppler decodes it, as UTF-8. A code point
// that is not a Unicode scalar value becomes U+FFFD.
std::string Utf8Of(const std::vector<Unicode>& text) {
  std::string out;
  for (Unicode c : text) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// The info keys DocMeta has typed text fields for; poppler-cpp reads those.
bool IsStandardTextKey(const std::string& key) {
  for (const char* standard :
       {"Title", "Author", "Subject", "Keywords", "Creator", "Producer",
        "CreationDate", "ModDate"}) {
    if (key == standard) return true;
  }
  return false;
}

// A real as the shortest text that reads back as it.
std::string FormatReal(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.15g", value);
  return text;
}

// A destination as PageDestination: the page as a zero-based index, the
// position relative to that page's CropBox (the contract's page space).
// False when the destination names no page of the document.
bool FillDestination(PDFDoc& doc, const LinkDest& dest,
                     pdfv1::PageDestination* out) {
  if (!dest.isOk()) return false;
  const int page_number =
      dest.isPageRef() ? doc.findPage(dest.getPageRef()) : dest.getPageNum();
  if (page_number < 1 || page_number > doc.getNumPages()) return false;
  out->set_page_index(static_cast<uint32_t>(page_number - 1));
  double cx = 0.0;
  double cy = 0.0;
  if (Page* page = doc.getPage(page_number); page != nullptr) {
    const PDFRectangle& crop = page->getCropBox();
    cx = crop.x1;
    cy = crop.y1;
  }
  switch (dest.getKind()) {
    case destXYZ:
      if (dest.getChangeLeft()) out->set_x(dest.getLeft() - cx);
      if (dest.getChangeTop()) out->set_y(dest.getTop() - cy);
      if (dest.getChangeZoom() && dest.getZoom() > 0.0) {
        out->set_zoom(dest.getZoom());
      }
      break;
    case destFitH:
    case destFitBH:
      if (dest.getChangeTop()) out->set_y(dest.getTop() - cy);
      break;
    case destFitV:
    case destFitBV:
      if (dest.getChangeLeft()) out->set_x(dest.getLeft() - cx);
      break;
    case destFitR:
      out->set_x(dest.getLeft() - cx);
      out->set_y(dest.getTop() - cy);
      break;
    default:
      break;
  }
  return true;
}

void FillOutline(PDFDoc& doc, const std::vector<OutlineItem*>* items,
                 int depth, DocumentFacts* facts,
                 google::protobuf::RepeatedPtrField<pdfv1::OutlineNode>* out) {
  if (items == nullptr) return;
  for (OutlineItem* item : *items) {
    if (item == nullptr) continue;
    pdfv1::OutlineNode* node = out->Add();
    node->set_title(Utf8Of(item->getTitle()));
    ++facts->outline_nodes;
    if (const LinkAction* action = item->getAction();
        action != nullptr && action->isOk()) {
      if (action->getKind() == actionGoTo) {
        const auto* go_to = static_cast<const LinkGoTo*>(action);
        pdfv1::PageDestination destination;
        bool found = false;
        if (const LinkDest* dest = go_to->getDest(); dest != nullptr) {
          found = FillDestination(doc, *dest, &destination);
        } else if (const GooString* name = go_to->getNamedDest();
                   name != nullptr) {
          if (std::unique_ptr<LinkDest> named = doc.findDest(name);
              named != nullptr) {
            found = FillDestination(doc, *named, &destination);
          }
        }
        if (found) *node->mutable_destination() = std::move(destination);
      } else if (action->getKind() == actionURI) {
        node->set_uri(ValidUtf8(static_cast<const LinkURI*>(action)->getURI()));
      }
    }
    if (!item->hasKids()) continue;
    if (depth + 1 >= kMaxOutlineDepth) {
      facts->outline_cut = true;
      continue;
    }
    item->open();
    FillOutline(doc, item->getKids(), depth + 1, facts,
                node->mutable_children());
  }
}

}  // namespace

DocumentFacts ReadDocumentFacts(const std::string& data,
                                const std::optional<std::string>& password,
                                bool want_outline) {
  DocumentFacts facts;
  // Keeps globalParams alive for the core document, and sends what poppler
  // reports to the caller's PopplerErrorScope (src/poppler_errors.h).
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
  if (!doc.isOk()) return facts;
  facts.readable = true;
  Catalog* catalog = doc.getCatalog();
  facts.tagged =
      catalog != nullptr && (catalog->getMarkInfo() & Catalog::markInfoMarked) != 0;

  XRef* xref = doc.getXRef();
  if (xref != nullptr && xref->isEncrypted()) {
    EncryptionFacts& encryption = facts.encryption.emplace();
    const Object stored = xref->getTrailerDict()->dictLookup("Encrypt");
    if (stored.isDict()) {
      if (const Object filter = stored.dictLookup("Filter"); filter.isName()) {
        encryption.filter = ValidUtf8(filter.getName());
      }
      if (const Object revision = stored.dictLookup("R");
          revision.isInt() && revision.getInt() >= 0) {
        encryption.revision = static_cast<uint32_t>(revision.getInt());
      }
      if (const Object permissions = stored.dictLookup("P");
          permissions.isInt()) {
        encryption.permissions_raw = static_cast<uint32_t>(permissions.getInt());
      }
    }
    unsigned char* key = nullptr;
    CryptAlgorithm algorithm = cryptNone;
    int key_bytes = 0;
    xref->getEncryptionParameters(&key, &algorithm, &key_bytes);
    if (key_bytes > 0) {
      encryption.key_length_bits = static_cast<uint32_t>(key_bytes) * 8;
    }
  }

  const Object info = doc.getDocInfo();
  if (info.isDict()) {
    for (int i = 0; i < info.dictGetLength(); ++i) {
      const std::string key = info.getDict()->getKey(i);
      if (IsStandardTextKey(key)) continue;
      const Object stored = info.dictGetVal(i);
      std::string value;
      if (stored.isString()) {
        value = PdfTextStringToUtf8(stored.getString());
      } else if (stored.isName()) {
        value = ValidUtf8(stored.getName());
      } else if (stored.isInt()) {
        value = std::to_string(stored.getInt());
      } else if (stored.isInt64()) {
        value = std::to_string(stored.getInt64());
      } else if (stored.isReal()) {
        value = FormatReal(stored.getReal());
      } else if (stored.isBool()) {
        value = stored.getBool() ? "true" : "false";
      } else {
        if (!stored.isNull()) facts.info_not_carried.push_back(ValidUtf8(key));
        continue;
      }
      if (key == "Trapped") {
        if (!value.empty()) facts.trapped = std::move(value);
      } else {
        facts.custom_info.emplace_back(ValidUtf8(key), std::move(value));
      }
    }
  }

  if (want_outline) {
    if (Outline* outline = doc.getOutline(); outline != nullptr) {
      google::protobuf::RepeatedPtrField<pdfv1::OutlineNode> roots;
      FillOutline(doc, outline->getItems(), 0, &facts, &roots);
      for (pdfv1::OutlineNode& root : roots) {
        facts.outline.push_back(std::move(root));
      }
    }
  }
  return facts;
}

}  // namespace grpc_poppler
