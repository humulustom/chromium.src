// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/enterprise_reporting/extension_info.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/extensions/manifest_handlers/app_launch_info.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension_set.h"
#include "extensions/common/manifest_url_handlers.h"
#include "extensions/common/permissions/permissions_data.h"

namespace enterprise_reporting {

namespace {

enterprise_management::Extension_ExtensionType GetExtensionType(
    extensions::Manifest::Type extension_type) {
  switch (extension_type) {
    case extensions::Manifest::TYPE_UNKNOWN:
    case extensions::Manifest::TYPE_SHARED_MODULE:
      return enterprise_management::Extension_ExtensionType_TYPE_UNKNOWN;
    case extensions::Manifest::TYPE_EXTENSION:
      return enterprise_management::Extension_ExtensionType_TYPE_EXTENSION;
    case extensions::Manifest::TYPE_THEME:
      return enterprise_management::Extension_ExtensionType_TYPE_THEME;
    case extensions::Manifest::TYPE_USER_SCRIPT:
      return enterprise_management::Extension_ExtensionType_TYPE_USER_SCRIPT;
    case extensions::Manifest::TYPE_HOSTED_APP:
      return enterprise_management::Extension_ExtensionType_TYPE_HOSTED_APP;
    case extensions::Manifest::TYPE_LEGACY_PACKAGED_APP:
      return enterprise_management::
          Extension_ExtensionType_TYPE_LEGACY_PACKAGED_APP;
    case extensions::Manifest::TYPE_PLATFORM_APP:
    case extensions::Manifest::TYPE_NWJS_APP:
      return enterprise_management::Extension_ExtensionType_TYPE_PLATFORM_APP;
    case extensions::Manifest::TYPE_LOGIN_SCREEN_EXTENSION:
      return enterprise_management::
          Extension_ExtensionType_TYPE_LOGIN_SCREEN_EXTENSION;
    case extensions::Manifest::NUM_LOAD_TYPES:
      NOTREACHED();
      return enterprise_management::Extension_ExtensionType_TYPE_UNKNOWN;
  }
}

enterprise_management::Extension_InstallType GetExtensionInstallType(
    extensions::Manifest::Location extension_location) {
  switch (extension_location) {
    case extensions::Manifest::INTERNAL:
      return enterprise_management::Extension_InstallType_TYPE_NORMAL;
    case extensions::Manifest::UNPACKED:
    case extensions::Manifest::COMMAND_LINE:
      return enterprise_management::Extension_InstallType_TYPE_DEVELOPMENT;
    case extensions::Manifest::EXTERNAL_PREF:
    case extensions::Manifest::EXTERNAL_REGISTRY:
    case extensions::Manifest::EXTERNAL_PREF_DOWNLOAD:
      return enterprise_management::Extension_InstallType_TYPE_SIDELOAD;
    case extensions::Manifest::EXTERNAL_POLICY:
    case extensions::Manifest::EXTERNAL_POLICY_DOWNLOAD:
      return enterprise_management::Extension_InstallType_TYPE_ADMIN;
    case extensions::Manifest::NUM_LOCATIONS:
      NOTREACHED();
      FALLTHROUGH;
    case extensions::Manifest::INVALID_LOCATION:
    case extensions::Manifest::COMPONENT:
    case extensions::Manifest::EXTERNAL_COMPONENT:
      return enterprise_management::Extension_InstallType_TYPE_OTHER;
  }
}

void AddPermission(const extensions::Extension* extension,
                   enterprise_management::Extension* extension_info) {
  for (const std::string& permission :
       extension->permissions_data()->active_permissions().GetAPIsAsStrings()) {
    extension_info->add_permissions(permission);
  }
}

void AddHostPermission(const extensions::Extension* extension,
                       enterprise_management::Extension* extension_info) {
  for (const auto& url :
       extension->permissions_data()->active_permissions().explicit_hosts()) {
    extension_info->add_host_permissions(url.GetAsString());
  }
}

void AddExtensions(const extensions::ExtensionSet& extensions,
                   enterprise_management::ChromeUserProfileInfo* profile_info,
                   bool enabled) {
  for (const auto& extension : extensions) {
    // Skip the component extension.
    if (!extension->ShouldExposeViaManagementAPI())
      continue;

    auto* extension_info = profile_info->add_extensions();
    extension_info->set_id(extension->id());
    extension_info->set_version(extension->VersionString());
    extension_info->set_name(extension->name());
    extension_info->set_description(extension->description());
    extension_info->set_app_type(GetExtensionType(extension->GetType()));
    extension_info->set_homepage_url(
        extensions::ManifestURL::GetHomepageURL(extension.get()).spec());
    extension_info->set_install_type(
        GetExtensionInstallType(extension->location()));
    extension_info->set_enabled(enabled);
    AddPermission(extension.get(), extension_info);
    AddHostPermission(extension.get(), extension_info);
    extension_info->set_from_webstore(extension->from_webstore());
  }
}

}  // namespace

void AppendExtensionInfoIntoProfileReport(
    Profile* profile,
    enterprise_management::ChromeUserProfileInfo* profile_info) {
  auto* registry = extensions::ExtensionRegistry::Get(profile);
  AddExtensions(registry->enabled_extensions(), profile_info, true);
  AddExtensions(registry->disabled_extensions(), profile_info, false);
  AddExtensions(registry->terminated_extensions(), profile_info, false);
}

}  // namespace enterprise_reporting
