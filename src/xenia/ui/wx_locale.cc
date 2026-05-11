/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/wx_locale.h"

#include <memory>
#include <string>
#include <unordered_map>

#include <wx/arrstr.h>
#include <wx/buffer.h>
#include <wx/intl.h>
#include <wx/string.h>
#include <wx/translation.h>

#include "xenia/base/cvar.h"
#include "xenia/base/embedded_bundle.h"
#include "xenia/base/logging.h"

#include "embedded_bundle_locale.h"

DEFINE_string(ui_locale, "",
              "UI locale as an ISO code (e.g. \"en\", \"de\", \"ja\"). "
              "Empty selects the system default.",
              "UI");

namespace xe {
namespace ui {

namespace {

// Decompresses the embedded locale bundle once and serves wxTranslations
// from it, replacing the on-disk wxFileTranslationsLoader. Entry names in
// the bundle are "<lang>/<domain>.mo" so a (domain, lang) lookup is a
// straight hash-map hit.
class EmbeddedTranslationsLoader final : public wxTranslationsLoader {
 public:
  EmbeddedTranslationsLoader()
      : bundle_(embedded_bundle_locale::kBundleData,
                embedded_bundle_locale::kBundleSize) {
    if (!bundle_.ok()) {
      return;
    }
    bundle_.ForEach([this](std::string_view name, std::string_view data) {
      auto slash = name.find('/');
      if (slash == std::string_view::npos) {
        return;
      }
      std::string lang(name.substr(0, slash));
      std::string_view rest = name.substr(slash + 1);
      const std::string_view kSuffix = ".mo";
      if (rest.size() <= kSuffix.size() ||
          rest.compare(rest.size() - kSuffix.size(), kSuffix.size(), kSuffix) !=
              0) {
        return;
      }
      std::string domain(rest.substr(0, rest.size() - kSuffix.size()));
      catalogs_[Key(domain, lang)] = data;
      languages_[domain].push_back(lang);
    });
  }

  wxMsgCatalog* LoadCatalog(const wxString& domain,
                            const wxString& lang) override {
    // wx hands us lang as "de_DE"; entries are keyed by the bare ISO code,
    // so retry on the prefix when the full tag misses.
    std::string d = domain.utf8_string();
    std::string l = lang.utf8_string();
    auto it = catalogs_.find(Key(d, l));
    if (it == catalogs_.end()) {
      auto underscore = l.find('_');
      if (underscore != std::string::npos) {
        it = catalogs_.find(Key(d, l.substr(0, underscore)));
      }
    }
    if (it == catalogs_.end()) {
      return nullptr;
    }
    auto buf = wxScopedCharBuffer::CreateNonOwned(it->second.data(),
                                                  it->second.size());
    return wxMsgCatalog::CreateFromData(buf, domain);
  }

  wxArrayString GetAvailableTranslations(
      const wxString& domain) const override {
    wxArrayString out;
    auto it = languages_.find(domain.utf8_string());
    if (it != languages_.end()) {
      for (const auto& lang : it->second) {
        out.Add(wxString::FromUTF8(lang));
      }
    }
    return out;
  }

 private:
  static std::string Key(const std::string& domain, const std::string& lang) {
    return lang + "\x1f" + domain;
  }

  EmbeddedBundle bundle_;
  std::unordered_map<std::string, std::string_view> catalogs_;
  std::unordered_map<std::string, std::vector<std::string>> languages_;
};

std::unique_ptr<wxLocale> g_locale;

}  // namespace

void InitializeWxLocale() {
  int lang_id = wxLANGUAGE_DEFAULT;
  if (!cvars::ui_locale.empty()) {
    const wxLanguageInfo* info =
        wxLocale::FindLanguageInfo(wxString::FromUTF8(cvars::ui_locale));
    if (info) {
      lang_id = info->Language;
    } else {
      XELOGW("Unknown ui_locale \"{}\"; falling back to system default.",
             cvars::ui_locale);
    }
  }

  g_locale = std::make_unique<wxLocale>();
  if (!g_locale->Init(lang_id, wxLOCALE_DONT_LOAD_DEFAULT)) {
    XELOGW("wxLocale::Init failed for language id {}.", lang_id);
  }

  // wxLocale::Init created a wxTranslations and installed the default
  // file-based loader. Swap it for the embedded-bundle one before any
  // AddCatalog call resolves a domain.
  if (auto* trans = wxTranslations::Get()) {
    trans->SetLoader(new EmbeddedTranslationsLoader());
    if (!trans->AddCatalog("xenia")) {
      XELOGI("No xenia translation catalog found for {}.",
             g_locale->GetCanonicalName().ToStdString(wxConvUTF8));
    }
  }
}

std::string GetActiveLocaleCanonicalName() {
  if (!g_locale) {
    return {};
  }
  return g_locale->GetCanonicalName().ToStdString(wxConvUTF8);
}

}  // namespace ui
}  // namespace xe
