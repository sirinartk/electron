// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/session_preferences.h"

#include <string>

namespace electron {

// static
int SessionPreferences::kLocatorKey = 0;

SessionPreferences::SessionPreferences(content::BrowserContext* context) {
  context->SetUserData(&kLocatorKey, base::WrapUnique(this));
}

SessionPreferences::~SessionPreferences() {}

// static
SessionPreferences* SessionPreferences::FromBrowserContext(
    content::BrowserContext* context) {
  return static_cast<SessionPreferences*>(context->GetUserData(&kLocatorKey));
}

// static
std::vector<base::FilePath::StringType> SessionPreferences::GetPreloads(
    content::BrowserContext* context) {
  std::vector<std::string> result;

  if (auto* self = FromBrowserContext(context)) {
    for (const auto& preload : self->preloads()) {
      if (!base::FilePath(preload).IsAbsolute()) {
        LOG(ERROR) << "preload script must have absolute path: " << preload;
        continue;
      }
#if defined(OS_WIN)
      result.emplace_back(base::UTF16ToUTF8(preload));
#else
      result.emplace_back(preload);
#endif
    }
  }

  return result;
}

}  // namespace electron
