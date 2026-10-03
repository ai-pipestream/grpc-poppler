// SPDX-License-Identifier: GPL-3.0-or-later

#include "poppler_forms.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

// poppler's core headers, installed by its ENABLE_UNSTABLE_API_ABI_HEADERS
// option; poppler.pc puts <includedir>/poppler on the include path.
#include <Error.h>
#include <Form.h>
#include <GlobalParams.h>
#include <Object.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Stream.h>
#include <goo/GooString.h>

#include "utf8.h"

namespace grpc_poppler {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// ISO 32000-1 field flag bit 1 (table 221).
constexpr uint32_t kFieldFlagReadOnly = 1u << 0;

std::string TextString(const GooString* text) {
  if (text == nullptr) return {};
  return PdfTextStringToUtf8(text->toStr());
}

// A field value as text: a text string decoded from PDFDocEncoding or
// UTF-16, a name without its slash (the state name, as PDFium reports a
// button value, made valid UTF-8 since a PDF name is raw bytes), the first
// entry of a multi-selection array.
std::optional<std::string> ValueText(const Object& value) {
  if (value.isString()) return PdfTextStringToUtf8(value.getString());
  if (value.isName()) return ValidUtf8(value.getName());
  if (value.isArray()) {
    for (int i = 0; i < value.arrayGetLength(); ++i) {
      const Object entry = value.arrayGet(i, 0);
      if (entry.isString()) return PdfTextStringToUtf8(entry.getString());
    }
  }
  return std::nullopt;
}

pdfv1::FormFieldKind Kind(FormWidget* widget) {
  switch (widget->getType()) {
    case formButton:
      switch (static_cast<FormWidgetButton*>(widget)->getButtonType()) {
        case formButtonCheck:
          return pdfv1::FORM_FIELD_KIND_CHECK_BOX;
        case formButtonPush:
          return pdfv1::FORM_FIELD_KIND_PUSH_BUTTON;
        case formButtonRadio:
          return pdfv1::FORM_FIELD_KIND_RADIO_BUTTON;
      }
      return pdfv1::FORM_FIELD_KIND_UNSPECIFIED;
    case formText:
      return pdfv1::FORM_FIELD_KIND_TEXT;
    case formChoice:
      return static_cast<FormWidgetChoice*>(widget)->isCombo()
                 ? pdfv1::FORM_FIELD_KIND_COMBO_BOX
                 : pdfv1::FORM_FIELD_KIND_LIST_BOX;
    case formSignature:
      return pdfv1::FORM_FIELD_KIND_SIGNATURE;
    default:
      return pdfv1::FORM_FIELD_KIND_UNSPECIFIED;
  }
}

void FillField(FormWidget* widget, pdfv1::FormField* field) {
  field->set_kind(Kind(widget));
  field->set_name(TextString(widget->getFullyQualifiedName()));

  Object* object = widget->getObj();
  if (object == nullptr || !object->isDict()) return;
  Dict* dict = object->getDict();

  // Form::fieldLookup walks the /Parent chain (with a loop guard), so
  // each of these is the inherited entry, the value docling-core reports.
  // An absent /Ff is the empty mask (the spec default, and what PDFium and
  // the qpdf-based engine report), so the three backends agree.
  const Object flags = Form::fieldLookup(dict, "Ff");
  const uint32_t mask = flags.isInt() ? static_cast<uint32_t>(flags.getInt()) : 0;
  field->set_flags(mask);
  field->set_read_only((mask & kFieldFlagReadOnly) != 0);
  if (auto value = ValueText(Form::fieldLookup(dict, "V"))) {
    if (!value->empty()) field->set_value(std::move(*value));
  }
  if (auto value = ValueText(Form::fieldLookup(dict, "DV"))) {
    if (!value->empty()) field->set_default_value(std::move(*value));
  }
  const Object tooltip = Form::fieldLookup(dict, "TU");
  if (tooltip.isString()) {
    std::string text = PdfTextStringToUtf8(tooltip.getString());
    if (!text.empty()) field->set_alternate_name(std::move(text));
  }
  if (widget->getType() == formChoice) {
    auto* choice = static_cast<FormWidgetChoice*>(widget);
    const int choices = static_cast<int>(choice->getChoices().size());
    for (int i = 0; i < choices; ++i) {
      field->add_options(TextString(choice->getChoice(i)));
    }
  }

  // /AS belongs to the widget annotation and is never inherited.
  const Object state = object->dictLookup("AS");
  if (state.isName()) {
    field->set_appearance_state("/" + ValidUtf8(state.getName()));
  }

  double x1 = 0.0;
  double y1 = 0.0;
  double x2 = 0.0;
  double y2 = 0.0;
  widget->getRect(&x1, &y1, &x2, &y2);
  auto* rect = field->mutable_rect();
  rect->set_x0(std::min(x1, x2));
  rect->set_y0(std::min(y1, y2));
  rect->set_x1(std::max(x1, x2));
  rect->set_y1(std::max(y1, y2));
}

}  // namespace

std::map<int, std::vector<pdfv1::FormField>> ReadFormFields(
    const std::string& data, const std::optional<std::string>& password, int begin,
    int end) {
  std::map<int, std::vector<pdfv1::FormField>> out;
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
  if (!doc.isOk()) return out;
  const int pages = doc.getNumPages();
  for (int index = std::max(begin, 0); index < end && index < pages; ++index) {
    Page* page = doc.getPage(index + 1);
    if (page == nullptr) continue;
    std::unique_ptr<FormPageWidgets> widgets = page->getFormWidgets();
    if (widgets == nullptr) continue;
    for (int w = 0; w < widgets->getNumWidgets(); ++w) {
      FormWidget* widget = widgets->getWidget(w);
      if (widget == nullptr) continue;
      FillField(widget, &out[index].emplace_back());
    }
  }
  return out;
}

}  // namespace grpc_poppler
