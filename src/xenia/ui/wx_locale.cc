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

#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/translation.h>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_string(ui_locale, "",
              "UI locale as an ISO code (e.g. \"en\", \"de\", \"ja\"). "
              "Empty selects the system default.",
              "UI");

namespace xe {
namespace ui {

namespace {

// Must outlive any wxGetTranslation calls.
std::unique_ptr<wxLocale> g_locale;

}  // namespace

void InitializeWxLocale() {
  wxFileName locale_path(wxStandardPaths::Get().GetExecutablePath());
  locale_path.SetFullName("");
  locale_path.AppendDir("locale");
  wxLocale::AddCatalogLookupPathPrefix(locale_path.GetFullPath());

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
  g_locale->AddCatalog("wxstd");
  if (!g_locale->AddCatalog("xenia")) {
    XELOGI("No xenia translation catalog found for {}.",
           g_locale->GetCanonicalName().ToStdString(wxConvUTF8));
  }
}

}  // namespace ui
}  // namespace xe
