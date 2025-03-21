// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/web_app.h"

#include <algorithm>
#include <ios>
#include <ostream>
#include <tuple>

#include "base/logging.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "third_party/blink/public/common/manifest/manifest_util.h"
#include "ui/gfx/color_utils.h"

namespace {

std::string ColorToString(base::Optional<SkColor> color) {
  return color.has_value() ? color_utils::SkColorToRgbaString(color.value())
                           : "none";
}

}  // namespace

namespace web_app {

WebApp::WebApp(const AppId& app_id)
    : app_id_(app_id),
      display_mode_(DisplayMode::kUndefined),
      user_display_mode_(DisplayMode::kUndefined) {}

WebApp::~WebApp() = default;

WebApp::WebApp(const WebApp& web_app) = default;

WebApp& WebApp::operator=(WebApp&& web_app) = default;

void WebApp::AddSource(Source::Type source) {
  sources_[source] = true;
}

void WebApp::RemoveSource(Source::Type source) {
  sources_[source] = false;
}

bool WebApp::HasAnySources() const {
  return sources_.any();
}

bool WebApp::HasOnlySource(Source::Type source) const {
  Sources specified_sources;
  specified_sources[source] = true;
  return HasAnySpecifiedSourcesAndNoOtherSources(specified_sources);
}

bool WebApp::IsSynced() const {
  return sources_[Source::kSync];
}

bool WebApp::IsDefaultApp() const {
  return sources_[Source::kDefault];
}

bool WebApp::IsSystemApp() const {
  return sources_[Source::kSystem];
}

bool WebApp::CanUserUninstallExternalApp() const {
  Sources specified_sources;
  specified_sources[Source::kDefault] = true;
  specified_sources[Source::kSync] = true;
  specified_sources[Source::kWebAppStore] = true;
  return HasAnySpecifiedSourcesAndNoOtherSources(specified_sources);
}

bool WebApp::HasAnySpecifiedSourcesAndNoOtherSources(
    Sources specified_sources) const {
  bool has_any_specified_sources = (sources_ & specified_sources).any();
  bool has_no_other_sources = (sources_ & ~specified_sources).none();
  return has_any_specified_sources && has_no_other_sources;
}

bool WebApp::WasInstalledByUser() const {
  return sources_[Source::kSync] || sources_[Source::kWebAppStore];
}

Source::Type WebApp::GetHighestPrioritySource() const {
  // Enumerators in Source enum are declaretd in the order of priority.
  // Top priority sources are declared first.
  for (int i = Source::kMinValue; i <= Source::kMaxValue; ++i) {
    auto source = static_cast<Source::Type>(i);
    if (sources_[source])
      return source;
  }

  NOTREACHED();
  return Source::kMaxValue;
}

void WebApp::SetName(const std::string& name) {
  name_ = name;
}

void WebApp::SetDescription(const std::string& description) {
  description_ = description;
}

void WebApp::SetLaunchUrl(const GURL& launch_url) {
  DCHECK(!launch_url.is_empty() && launch_url.is_valid());
  launch_url_ = launch_url;
}

void WebApp::SetScope(const GURL& scope) {
  DCHECK(scope.is_empty() || scope.is_valid());
  scope_ = scope;
}

void WebApp::SetThemeColor(base::Optional<SkColor> theme_color) {
  theme_color_ = theme_color;
}

void WebApp::SetDisplayMode(DisplayMode display_mode) {
  DCHECK_NE(DisplayMode::kUndefined, display_mode);
  display_mode_ = display_mode;
}

void WebApp::SetUserDisplayMode(DisplayMode user_display_mode) {
  switch (user_display_mode) {
    case DisplayMode::kBrowser:
      user_display_mode_ = DisplayMode::kBrowser;
      break;
    case DisplayMode::kUndefined:
    case DisplayMode::kMinimalUi:
    case DisplayMode::kFullscreen:
      NOTREACHED();
      FALLTHROUGH;
    case DisplayMode::kStandalone:
      user_display_mode_ = DisplayMode::kStandalone;
      break;
  }
}

void WebApp::SetIsLocallyInstalled(bool is_locally_installed) {
  is_locally_installed_ = is_locally_installed;
}

void WebApp::SetIsInSyncInstall(bool is_in_sync_install) {
  is_in_sync_install_ = is_in_sync_install;
}

void WebApp::SetIconInfos(std::vector<WebApplicationIconInfo> icon_infos) {
  icon_infos_ = std::move(icon_infos);
}

void WebApp::SetDownloadedIconSizes(std::vector<SquareSizePx> sizes) {
  std::sort(sizes.begin(), sizes.end());
  downloaded_icon_sizes_ = std::move(sizes);
}

void WebApp::SetFileHandlers(FileHandlers file_handlers) {
  file_handlers_ = std::move(file_handlers);
}

void WebApp::SetSyncData(SyncData sync_data) {
  sync_data_ = std::move(sync_data);
}

WebApp::SyncData::SyncData() = default;

WebApp::SyncData::~SyncData() = default;

WebApp::SyncData::SyncData(const SyncData& sync_data) = default;

WebApp::SyncData& WebApp::SyncData::operator=(SyncData&& sync_data) = default;

WebApp::FileHandlerAccept::FileHandlerAccept() = default;
WebApp::FileHandlerAccept::~FileHandlerAccept() = default;
WebApp::FileHandlerAccept::FileHandlerAccept(
    const FileHandlerAccept& file_handler_accept) = default;
WebApp::FileHandlerAccept& WebApp::FileHandlerAccept::operator=(
    FileHandlerAccept&& file_handler_accept) = default;

WebApp::FileHandler::FileHandler() = default;
WebApp::FileHandler::~FileHandler() = default;
WebApp::FileHandler::FileHandler(const FileHandler& file_handler) = default;
WebApp::FileHandler& WebApp::FileHandler::operator=(
    FileHandler&& file_handler) = default;

std::ostream& operator<<(std::ostream& out, const WebApp::SyncData& sync_data) {
  return out << "theme_color: " << ColorToString(sync_data.theme_color)
             << " name: " << sync_data.name;
}

std::ostream& operator<<(std::ostream& out,
                         const WebApp::FileHandlerAccept& file_handler_accept) {
  out << "mimetype: " << file_handler_accept.mimetype << " file_extensions:";
  for (const auto& file_extension : file_handler_accept.file_extensions)
    out << " " << file_extension;
  return out;
}

std::ostream& operator<<(std::ostream& out,
                         const WebApp::FileHandler& file_handler) {
  out << "action: " << file_handler.action;
  for (const auto& accept_entry : file_handler.accept)
    out << " accept: " << accept_entry;
  return out;
}

std::ostream& operator<<(std::ostream& out, const WebApp& app) {
  const std::string display_mode =
      blink::DisplayModeToString(app.display_mode_);
  const std::string user_display_mode =
      blink::DisplayModeToString(app.user_display_mode_);
  const bool is_locally_installed = app.is_locally_installed_;
  const bool is_in_sync_install = app.is_in_sync_install_;

  out << "app_id: " << app.app_id_ << std::endl
      << "  name: " << app.name_ << std::endl
      << "  launch_url: " << app.launch_url_ << std::endl
      << "  scope: " << app.scope_ << std::endl
      << "  theme_color: " << ColorToString(app.theme_color_) << std::endl
      << "  display_mode: " << display_mode << std::endl
      << "  user_display_mode: " << user_display_mode << std::endl
      << "  sources: " << app.sources_.to_string() << std::endl
      << "  is_locally_installed: " << is_locally_installed << std::endl
      << "  is_in_sync_install: " << is_in_sync_install << std::endl
      << "  sync_data: " << app.sync_data_ << std::endl
      << "  description: " << app.description_ << std::endl;
  for (const WebApplicationIconInfo& icon : app.icon_infos_)
    out << "  icon_url: " << icon << std::endl;
  for (SquareSizePx size : app.downloaded_icon_sizes_)
    out << "  icon_size_on_disk: " << size << std::endl;
  for (const WebApp::FileHandler& file_handler : app.file_handlers_)
    out << "  file_handler: " << file_handler << std::endl;

  return out;
}

bool operator==(const WebApp::FileHandlerAccept& file_handler_accept1,
                const WebApp::FileHandlerAccept& file_handler_accept2) {
  return std::tie(file_handler_accept1.mimetype,
                  file_handler_accept1.file_extensions) ==
         std::tie(file_handler_accept2.mimetype,
                  file_handler_accept2.file_extensions);
}

bool operator==(const WebApp::FileHandler& file_handler1,
                const WebApp::FileHandler& file_handler2) {
  return std::tie(file_handler1.action, file_handler1.accept) ==
         std::tie(file_handler2.action, file_handler2.accept);
}

bool operator==(const WebApp::SyncData& sync_data1,
                const WebApp::SyncData& sync_data2) {
  return std::tie(sync_data1.name, sync_data1.theme_color) ==
         std::tie(sync_data2.name, sync_data2.theme_color);
}

bool operator!=(const WebApp::FileHandlerAccept& file_handler_accept1,
                const WebApp::FileHandlerAccept& file_handler_accept2) {
  return !(file_handler_accept1 == file_handler_accept2);
}

bool operator!=(const WebApp::FileHandler& file_handler1,
                const WebApp::FileHandler& file_handler2) {
  return !(file_handler1 == file_handler2);
}

bool operator!=(const WebApp::SyncData& sync_data1,
                const WebApp::SyncData& sync_data2) {
  return !(sync_data1 == sync_data2);
}

bool operator==(const WebApp& app1, const WebApp& app2) {
  return std::tie(app1.app_id_, app1.sources_, app1.name_, app1.launch_url_,
                  app1.description_, app1.scope_, app1.theme_color_,
                  app1.icon_infos_, app1.downloaded_icon_sizes_,
                  app1.display_mode_, app1.user_display_mode_,
                  app1.is_locally_installed_, app1.is_in_sync_install_,
                  app1.file_handlers_, app1.sync_data_) ==
         std::tie(app2.app_id_, app2.sources_, app2.name_, app2.launch_url_,
                  app2.description_, app2.scope_, app2.theme_color_,
                  app2.icon_infos_, app2.downloaded_icon_sizes_,
                  app2.display_mode_, app2.user_display_mode_,
                  app2.is_locally_installed_, app2.is_in_sync_install_,
                  app2.file_handlers_, app2.sync_data_);
}

bool operator!=(const WebApp& app1, const WebApp& app2) {
  return !(app1 == app2);
}

}  // namespace web_app
