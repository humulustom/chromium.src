// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/content_settings_agent_impl.h"

#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/feature_list.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/client_hints/client_hints.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/ssl_insecure_content.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings.mojom.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_utils.h"
#include "content/public/child/child_thread.h"
#include "content/public/common/client_hints.mojom.h"
#include "content/public/common/origin_util.h"
#include "content/public/common/previews_state.h"
#include "content/public/common/url_constants.h"
#include "content/public/renderer/document_state.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_view.h"
#include "extensions/buildflags/buildflags.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/common/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/url_conversion.h"
#include "third_party/blink/public/platform/web_client_hints_type.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_local_frame_client.h"
#include "third_party/blink/public/web/web_view.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/permissions/api_permission.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/renderer/dispatcher.h"
#include "extensions/renderer/renderer_extension_registry.h"
#endif

using blink::WebDocument;
using blink::WebFrame;
using blink::WebLocalFrame;
using blink::WebSecurityOrigin;
using blink::WebString;
using blink::WebURL;
using blink::WebView;
using content::DocumentState;

namespace {

GURL GetOriginOrURL(const WebFrame* frame) {
  url::Origin top_origin = url::Origin(frame->Top()->GetSecurityOrigin());
  // The |top_origin| is unique ("null") e.g., for file:// URLs. Use the
  // document URL as the primary URL in those cases.
  // TODO(alexmos): This is broken for --site-per-process, since top() can be a
  // WebRemoteFrame which does not have a document(), and the WebRemoteFrame's
  // URL is not replicated.  See https://crbug.com/628759.
  if (top_origin.opaque() && frame->Top()->IsWebLocalFrame())
    return frame->Top()->ToWebLocalFrame()->GetDocument().Url();
  return top_origin.GetURL();
}

// Allow passing both WebURL and GURL here, so that we can early return without
// allocating a new backing string if only the default rule matches.
template <typename URL>
ContentSetting GetContentSettingFromRules(
    const ContentSettingsForOneType& rules,
    const WebFrame* frame,
    const URL& secondary_url) {
  // If there is only one rule, it's the default rule and we don't need to match
  // the patterns.
  if (rules.size() == 1) {
    DCHECK(rules[0].primary_pattern == ContentSettingsPattern::Wildcard());
    DCHECK(rules[0].secondary_pattern == ContentSettingsPattern::Wildcard());
    return rules[0].GetContentSetting();
  }
  const GURL& primary_url = GetOriginOrURL(frame);
  const GURL& secondary_gurl = secondary_url;
  for (const auto& rule : rules) {
    if (rule.primary_pattern.Matches(primary_url) &&
        rule.secondary_pattern.Matches(secondary_gurl)) {
      return rule.GetContentSetting();
    }
  }
  NOTREACHED();
  return CONTENT_SETTING_DEFAULT;
}

bool IsScriptDisabledForPreview(content::RenderFrame* render_frame) {
  return render_frame->GetPreviewsState() & content::NOSCRIPT_ON;
}

bool IsFrameWithOpaqueOrigin(WebFrame* frame) {
  // Storage access is keyed off the top origin and the frame's origin.
  // It will be denied any opaque origins so have this method to return early
  // instead of making a Sync IPC call.
  return frame->GetSecurityOrigin().IsOpaque() ||
         frame->Top()->GetSecurityOrigin().IsOpaque();
}

}  // namespace

ContentSettingsAgentImpl::ContentSettingsAgentImpl(
    content::RenderFrame* render_frame,
    bool should_whitelist,
    service_manager::BinderRegistry* registry)
    : content::RenderFrameObserver(render_frame),
      content::RenderFrameObserverTracker<ContentSettingsAgentImpl>(
          render_frame),
      should_whitelist_(should_whitelist) {
  ClearBlockedContentSettings();
  render_frame->GetWebFrame()->SetContentSettingsClient(this);

  render_frame->GetAssociatedInterfaceRegistry()->AddInterface(
      base::Bind(&ContentSettingsAgentImpl::OnContentSettingsAgentRequest,
                 base::Unretained(this)));

  content::RenderFrame* main_frame =
      render_frame->GetRenderView()->GetMainRenderFrame();
  // TODO(nasko): The main frame is not guaranteed to be in the same process
  // with this frame with --site-per-process. This code needs to be updated
  // to handle this case. See https://crbug.com/496670.
  if (main_frame && main_frame != render_frame) {
    // Copy all the settings from the main render frame to avoid race conditions
    // when initializing this data. See https://crbug.com/333308.
    ContentSettingsAgentImpl* parent =
        ContentSettingsAgentImpl::Get(main_frame);
    allow_running_insecure_content_ = parent->allow_running_insecure_content_;
    temporarily_allowed_plugins_ = parent->temporarily_allowed_plugins_;
    is_interstitial_page_ = parent->is_interstitial_page_;
  }
}

ContentSettingsAgentImpl::~ContentSettingsAgentImpl() {}

chrome::mojom::ContentSettingsManager&
ContentSettingsAgentImpl::GetContentSettingsManager() {
  if (!content_settings_manager_)
    BindContentSettingsManager(&content_settings_manager_);
  return *content_settings_manager_;
}

#if BUILDFLAG(ENABLE_EXTENSIONS)
void ContentSettingsAgentImpl::SetExtensionDispatcher(
    extensions::Dispatcher* extension_dispatcher) {
  DCHECK(!extension_dispatcher_)
      << "SetExtensionDispatcher() should only be called once.";
  extension_dispatcher_ = extension_dispatcher;
}
#endif

void ContentSettingsAgentImpl::SetContentSettingRules(
    const RendererContentSettingRules* content_setting_rules) {
  content_setting_rules_ = content_setting_rules;
  UMA_HISTOGRAM_COUNTS_1M("ClientHints.CountRulesReceived",
                          content_setting_rules_->client_hints_rules.size());
}

const RendererContentSettingRules*
ContentSettingsAgentImpl::GetContentSettingRules() {
  return content_setting_rules_;
}

bool ContentSettingsAgentImpl::IsPluginTemporarilyAllowed(
    const std::string& identifier) {
  // If the empty string is in here, it means all plugins are allowed.
  // TODO(bauerb): Remove this once we only pass in explicit identifiers.
  return base::Contains(temporarily_allowed_plugins_, identifier) ||
         base::Contains(temporarily_allowed_plugins_, std::string());
}

void ContentSettingsAgentImpl::DidBlockContentType(
    ContentSettingsType settings_type) {
  bool newly_blocked = content_blocked_.insert(settings_type).second;
  if (newly_blocked)
    GetContentSettingsManager().OnContentBlocked(routing_id(), settings_type);
}

void ContentSettingsAgentImpl::BindContentSettingsManager(
    mojo::Remote<chrome::mojom::ContentSettingsManager>* manager) {
  DCHECK(!*manager);
  content::ChildThread::Get()->BindHostReceiver(
      manager->BindNewPipeAndPassReceiver());
}

bool ContentSettingsAgentImpl::OnMessageReceived(const IPC::Message& message) {
  // Don't swallow LoadBlockedPlugins messages, as they're sent to every
  // blocked plugin.
  IPC_BEGIN_MESSAGE_MAP(ContentSettingsAgentImpl, message)
    IPC_MESSAGE_HANDLER(ChromeViewMsg_LoadBlockedPlugins, OnLoadBlockedPlugins)
  IPC_END_MESSAGE_MAP()
  return false;
}

void ContentSettingsAgentImpl::DidCommitProvisionalLoad(
    bool is_same_document_navigation,
    ui::PageTransition transition) {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (frame->Parent())
    return;  // Not a top-level navigation.

  if (!is_same_document_navigation) {
    // Clear "block" flags for the new page. This needs to happen before any of
    // |allowScript()|, |allowScriptFromSource()|, |allowImage()|, or
    // |allowPlugins()| is called for the new page so that these functions can
    // correctly detect that a piece of content flipped from "not blocked" to
    // "blocked".
    ClearBlockedContentSettings();
    temporarily_allowed_plugins_.clear();

    // The BrowserInterfaceBroker is reset on navigation, so we will need to
    // re-acquire the ContentSettingsManager.
    content_settings_manager_.reset();
  }

  GURL url = frame->GetDocument().Url();
  // If we start failing this DCHECK, please makes sure we don't regress
  // this bug: http://code.google.com/p/chromium/issues/detail?id=79304
  DCHECK(frame->GetDocument().GetSecurityOrigin().ToString() == "null" ||
         !url.SchemeIs(url::kDataScheme));
}

void ContentSettingsAgentImpl::OnDestruct() {
  delete this;
}

void ContentSettingsAgentImpl::SetAllowRunningInsecureContent() {
  allow_running_insecure_content_ = true;

  // Reload if we are the main frame.
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!frame->Parent())
    frame->StartReload(blink::WebFrameLoadType::kReload);
}

void ContentSettingsAgentImpl::SetAsInterstitial() {
  is_interstitial_page_ = true;
}

void ContentSettingsAgentImpl::SetDisabledMixedContentUpgrades() {
  mixed_content_autoupgrades_disabled_ = true;
}

void ContentSettingsAgentImpl::OnContentSettingsAgentRequest(
    mojo::PendingAssociatedReceiver<chrome::mojom::ContentSettingsAgent>
        receiver) {
  receivers_.Add(this, std::move(receiver));
}

bool ContentSettingsAgentImpl::AllowDatabase() {
  return AllowStorageAccess(
      chrome::mojom::ContentSettingsManager::StorageType::DATABASE);
}

void ContentSettingsAgentImpl::RequestFileSystemAccessAsync(
    base::OnceCallback<void(bool)> callback) {
  WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (IsFrameWithOpaqueOrigin(frame)) {
    std::move(callback).Run(false);
    return;
  }

  GetContentSettingsManager().AllowStorageAccess(
      routing_id(),
      chrome::mojom::ContentSettingsManager::StorageType::FILE_SYSTEM,
      frame->GetSecurityOrigin(),
      frame->GetDocument().SiteForCookies().RepresentativeUrl(),
      frame->GetDocument().TopFrameOrigin(), std::move(callback));
}

bool ContentSettingsAgentImpl::AllowImage(bool enabled_per_settings,
                                          const WebURL& image_url) {
  bool allow = enabled_per_settings;
  if (enabled_per_settings) {
    if (is_interstitial_page_)
      return true;

    if (IsWhitelistedForContentSettings())
      return true;

    if (content_setting_rules_) {
      allow = GetContentSettingFromRules(content_setting_rules_->image_rules,
                                         render_frame()->GetWebFrame(),
                                         image_url) != CONTENT_SETTING_BLOCK;
    }
  }
  if (!allow)
    DidBlockContentType(ContentSettingsType::IMAGES);
  return allow;
}

bool ContentSettingsAgentImpl::AllowIndexedDB() {
  return AllowStorageAccess(
      chrome::mojom::ContentSettingsManager::StorageType::INDEXED_DB);
}

bool ContentSettingsAgentImpl::AllowCacheStorage() {
  return AllowStorageAccess(
      chrome::mojom::ContentSettingsManager::StorageType::CACHE);
}

bool ContentSettingsAgentImpl::AllowWebLocks() {
  return AllowStorageAccess(
      chrome::mojom::ContentSettingsManager::StorageType::WEB_LOCKS);
}

bool ContentSettingsAgentImpl::AllowScript(bool enabled_per_settings) {
  if (!enabled_per_settings)
    return false;
  if (IsScriptDisabledForPreview(render_frame()))
    return false;
  if (is_interstitial_page_)
    return true;

  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  const auto it = cached_script_permissions_.find(frame);
  if (it != cached_script_permissions_.end())
    return it->second;

  // Evaluate the content setting rules before
  // IsWhitelistedForContentSettings(); if there is only the default rule
  // allowing all scripts, it's quicker this way.
  bool allow = true;
  if (content_setting_rules_) {
    ContentSetting setting = GetContentSettingFromRules(
        content_setting_rules_->script_rules, frame,
        url::Origin(frame->GetDocument().GetSecurityOrigin()).GetURL());
    allow = setting != CONTENT_SETTING_BLOCK;
  }
  allow = allow || IsWhitelistedForContentSettings();

  cached_script_permissions_[frame] = allow;
  return allow;
}

bool ContentSettingsAgentImpl::AllowScriptFromSource(
    bool enabled_per_settings,
    const blink::WebURL& script_url) {
  if (!enabled_per_settings)
    return false;
  if (IsScriptDisabledForPreview(render_frame()))
    return false;
  if (is_interstitial_page_)
    return true;

  bool allow = true;
  if (content_setting_rules_) {
    ContentSetting setting =
        GetContentSettingFromRules(content_setting_rules_->script_rules,
                                   render_frame()->GetWebFrame(), script_url);
    allow = setting != CONTENT_SETTING_BLOCK;
  }
  return allow || IsWhitelistedForContentSettings();
}

bool ContentSettingsAgentImpl::AllowStorage(bool local) {
  WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (IsFrameWithOpaqueOrigin(frame))
    return false;

  StoragePermissionsKey key(
      url::Origin(frame->GetDocument().GetSecurityOrigin()).GetURL(), local);
  const auto permissions = cached_storage_permissions_.find(key);
  if (permissions != cached_storage_permissions_.end())
    return permissions->second;

  bool result = false;
  GetContentSettingsManager().AllowStorageAccess(
      routing_id(),
      local
          ? chrome::mojom::ContentSettingsManager::StorageType::LOCAL_STORAGE
          : chrome::mojom::ContentSettingsManager::StorageType::SESSION_STORAGE,
      frame->GetSecurityOrigin(),
      frame->GetDocument().SiteForCookies().RepresentativeUrl(),
      frame->GetDocument().TopFrameOrigin(), &result);
  cached_storage_permissions_[key] = result;
  return result;
}

bool ContentSettingsAgentImpl::AllowReadFromClipboard(bool default_value) {
  bool allowed = default_value;
#if BUILDFLAG(ENABLE_EXTENSIONS)
  extensions::ScriptContext* current_context =
      extension_dispatcher_->script_context_set().GetCurrent();
  if (current_context) {
    allowed |= current_context->HasAPIPermission(
        extensions::APIPermission::kClipboardRead);
  }
#endif
  return allowed;
}

bool ContentSettingsAgentImpl::AllowWriteToClipboard(bool default_value) {
  bool allowed = default_value;
#if BUILDFLAG(ENABLE_EXTENSIONS)
  // All blessed extension pages could historically write to the clipboard, so
  // preserve that for compatibility.
  extensions::ScriptContext* current_context =
      extension_dispatcher_->script_context_set().GetCurrent();
  if (current_context) {
    if (current_context->effective_context_type() ==
            extensions::Feature::BLESSED_EXTENSION_CONTEXT &&
        !current_context->IsForServiceWorker()) {
      allowed = true;
    } else {
      allowed |= current_context->HasAPIPermission(
          extensions::APIPermission::kClipboardWrite);
    }
  }
#endif
  return allowed;
}

bool ContentSettingsAgentImpl::AllowMutationEvents(bool default_value) {
  return default_value;
}

bool ContentSettingsAgentImpl::AllowRunningInsecureContent(
    bool allowed_per_settings,
    const blink::WebURL& resource_url) {
  bool allow = allowed_per_settings;

  if (base::FeatureList::IsEnabled(features::kMixedContentSiteSetting)) {
    if (content_setting_rules_) {
      auto setting = GetContentSettingFromRules(
          content_setting_rules_->mixed_content_rules,
          render_frame()->GetWebFrame(), GURL());
      allow |= (setting == CONTENT_SETTING_ALLOW);
    }
  } else {
    allow |= allow_running_insecure_content_;
    if (!allow) {
      DidBlockContentType(ContentSettingsType::MIXEDSCRIPT);
    }
  }

  // Note: this implementation is a mirror of
  // Browser::ShouldAllowRunningInsecureContent.
  FilteredReportInsecureContentRan(GURL(resource_url));

  return allow;
}

bool ContentSettingsAgentImpl::AllowPopupsAndRedirects(bool default_value) {
  if (!content_setting_rules_)
    return default_value;
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  return GetContentSettingFromRules(
             content_setting_rules_->popup_redirect_rules, frame,
             url::Origin(frame->GetDocument().GetSecurityOrigin()).GetURL()) ==
         CONTENT_SETTING_ALLOW;
}

void ContentSettingsAgentImpl::PassiveInsecureContentFound(
    const blink::WebURL& resource_url) {
  // Note: this implementation is a mirror of
  // Browser::PassiveInsecureContentFound.
  ReportInsecureContent(SslInsecureContentType::DISPLAY);
  FilteredReportInsecureContentDisplayed(GURL(resource_url));
}

void ContentSettingsAgentImpl::PersistClientHints(
    const blink::WebEnabledClientHints& enabled_client_hints,
    base::TimeDelta duration,
    const blink::WebURL& url) {
  if (duration <= base::TimeDelta())
    return;

  const GURL primary_url(url);
  const url::Origin primary_origin = url::Origin::Create(primary_url);
  if (!content::IsOriginSecure(primary_url))
    return;

  // TODO(tbansal): crbug.com/735518. Determine if the value should be
  // merged or overridden. Also, determine if the merger should happen on the
  // browser side or the renderer. If the value needs to be overridden,
  // this method should not return early if |update_count| is 0.
  std::vector<::blink::mojom::WebClientHintsType> client_hints;
  static constexpr size_t kWebClientHintsCount =
      static_cast<size_t>(blink::mojom::WebClientHintsType::kMaxValue) + 1;
  client_hints.reserve(kWebClientHintsCount);

  for (size_t i = 0; i < kWebClientHintsCount; ++i) {
    if (enabled_client_hints.IsEnabled(
            static_cast<blink::mojom::WebClientHintsType>(i))) {
      client_hints.push_back(static_cast<blink::mojom::WebClientHintsType>(i));
    }
  }
  size_t update_count = client_hints.size();
  if (update_count == 0)
    return;

  UMA_HISTOGRAM_CUSTOM_TIMES(
      "ClientHints.PersistDuration", duration, base::TimeDelta::FromSeconds(1),
      // TODO(crbug.com/949034): Rename and fix this histogram to have some
      // intended max value. We throw away the 32 most-significant bits of the
      // 64-bit time delta in milliseconds. Before it happened silently in
      // histogram.cc, now it is explicit here. The previous value of 365 days
      // effectively turns into roughly 17 days when getting cast to int.
      base::TimeDelta::FromMilliseconds(
          static_cast<int>(base::TimeDelta::FromDays(365).InMilliseconds())),
      100);

  UMA_HISTOGRAM_COUNTS_100("ClientHints.UpdateSize", update_count);

  // Notify the embedder.
  mojo::AssociatedRemote<client_hints::mojom::ClientHints> host_observer;
  render_frame()->GetRemoteAssociatedInterfaces()->GetInterface(&host_observer);
  host_observer->PersistClientHints(primary_origin, std::move(client_hints),
                                    duration);
}

void ContentSettingsAgentImpl::GetAllowedClientHintsFromSource(
    const blink::WebURL& url,
    blink::WebEnabledClientHints* client_hints) const {
  if (!content_setting_rules_)
    return;

  if (content_setting_rules_->client_hints_rules.empty())
    return;

  client_hints::GetAllowedClientHintsFromSource(
      url, content_setting_rules_->client_hints_rules, client_hints);
}

bool ContentSettingsAgentImpl::ShouldAutoupgradeMixedContent() {
  if (mixed_content_autoupgrades_disabled_)
    return false;

  if (content_setting_rules_) {
    auto setting =
        GetContentSettingFromRules(content_setting_rules_->mixed_content_rules,
                                   render_frame()->GetWebFrame(), GURL());
    return setting != CONTENT_SETTING_ALLOW;
  }
  return false;
}

void ContentSettingsAgentImpl::DidNotAllowPlugins() {
  DidBlockContentType(ContentSettingsType::PLUGINS);
}

void ContentSettingsAgentImpl::DidNotAllowScript() {
  DidBlockContentType(ContentSettingsType::JAVASCRIPT);
}

void ContentSettingsAgentImpl::OnLoadBlockedPlugins(
    const std::string& identifier) {
  temporarily_allowed_plugins_.insert(identifier);
}

void ContentSettingsAgentImpl::ClearBlockedContentSettings() {
  content_blocked_.clear();
  cached_storage_permissions_.clear();
  cached_script_permissions_.clear();
}

bool ContentSettingsAgentImpl::IsPlatformApp() {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  WebSecurityOrigin origin = frame->GetDocument().GetSecurityOrigin();
  const extensions::Extension* extension = GetExtension(origin);
  return extension && extension->is_platform_app();
#else
  return false;
#endif
}

#if BUILDFLAG(ENABLE_EXTENSIONS)
const extensions::Extension* ContentSettingsAgentImpl::GetExtension(
    const WebSecurityOrigin& origin) const {
  if (origin.Protocol().Ascii() != extensions::kExtensionScheme)
    return nullptr;

  const std::string extension_id = origin.Host().Utf8().data();
  if (!extension_dispatcher_->IsExtensionActive(extension_id))
    return nullptr;

  return extensions::RendererExtensionRegistry::Get()->GetByID(extension_id);
}
#endif

// static
bool ContentSettingsAgentImpl::IsWhitelistedForContentSettings() const {
  if (should_whitelist_)
    return true;

  // Whitelist ftp directory listings, as they require JavaScript to function
  // properly.
  if (render_frame()->IsFTPDirectoryListing())
    return true;

  const WebDocument& document = render_frame()->GetWebFrame()->GetDocument();
  return IsWhitelistedForContentSettings(document.GetSecurityOrigin(),
                                         document.Url());
}

bool ContentSettingsAgentImpl::IsWhitelistedForContentSettings(
    const WebSecurityOrigin& origin,
    const WebURL& document_url) {
  if (document_url.GetString() == content::kUnreachableWebDataURL)
    return true;

  if (origin.IsOpaque())
    return false;  // Uninitialized document?

  blink::WebString protocol = origin.Protocol();

  if (protocol == content::kChromeUIScheme)
    return true;  // Browser UI elements should still work.

  if (protocol == content::kChromeDevToolsScheme)
    return true;  // DevTools UI elements should still work.

#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (protocol == extensions::kExtensionScheme)
    return true;
#endif

  // If the scheme is file:, an empty file name indicates a directory listing,
  // which requires JavaScript to function properly.
  if (protocol == url::kFileScheme &&
      document_url.ProtocolIs(url::kFileScheme)) {
    return GURL(document_url).ExtractFileName().empty();
  }
  return false;
}

bool ContentSettingsAgentImpl::AllowStorageAccess(
    chrome::mojom::ContentSettingsManager::StorageType storage_type) {
  WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (IsFrameWithOpaqueOrigin(frame))
    return false;

  bool result = false;
  GetContentSettingsManager().AllowStorageAccess(
      routing_id(), storage_type, frame->GetSecurityOrigin(),
      frame->GetDocument().SiteForCookies().RepresentativeUrl(),
      frame->GetDocument().TopFrameOrigin(), &result);
  return result;
}
