// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/extensions/chrome_extensions_renderer_client.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "content/nw/src/nw_content.h"

#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "base/metrics/histogram_functions.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/extensions/extension_constants.h"
#include "chrome/common/extensions/extension_metrics.h"
#include "chrome/common/url_constants.h"
#include "chrome/renderer/chrome_render_thread_observer.h"
#include "chrome/renderer/extensions/chrome_extensions_dispatcher_delegate.h"
#include "chrome/renderer/extensions/extension_process_policy.h"
#include "chrome/renderer/extensions/renderer_permissions_policy_delegate.h"
#include "chrome/renderer/extensions/resource_request_policy.h"
#include "chrome/renderer/media/cast_ipc_dispatcher.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/content_switches.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_thread.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_set.h"
#include "extensions/common/extensions_client.h"
#include "extensions/common/manifest_handlers/background_info.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/common/switches.h"
#include "extensions/renderer/dispatcher.h"
#include "extensions/renderer/extension_frame_helper.h"
#include "extensions/renderer/extensions_render_frame_observer.h"
#include "extensions/renderer/extensions_renderer_client.h"
#include "extensions/renderer/guest_view/extensions_guest_view_container.h"
#include "extensions/renderer/guest_view/extensions_guest_view_container_dispatcher.h"
#include "extensions/renderer/guest_view/mime_handler_view/mime_handler_view_container.h"
#include "extensions/renderer/guest_view/mime_handler_view/mime_handler_view_container_manager.h"
#include "extensions/renderer/renderer_extension_registry.h"
#include "extensions/renderer/script_context.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cookies/site_for_cookies.h"
#include "services/metrics/public/cpp/mojo_ukm_recorder.h"
#include "services/metrics/public/cpp/ukm_builders.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_plugin_params.h"
#include "url/origin.h"

using extensions::Extension;

namespace {

bool IsStandaloneExtensionProcess() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      extensions::switches::kExtensionProcess);
}

void IsGuestViewApiAvailableToScriptContext(
    bool* api_is_available,
    extensions::ScriptContext* context) {
  if (context->GetAvailability("guestViewInternal").is_available()) {
    *api_is_available = true;
  }
}

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class GoogleDocsExtensionAvailablity {
  kAvailableRegular = 0,
  kNotAvailableRegular = 1,
  kAvailableIncognito = 2,
  kNotAvailableIncognito = 3,
  kMaxValue = kNotAvailableIncognito
};

bool ExtensionHasAccessToUrl(const Extension* extension,
                             int tab_id,
                             const GURL& url) {
  return extension->permissions_data()->GetPageAccess(url, tab_id, nullptr) ==
             extensions::PermissionsData::PageAccess::kAllowed ||
         extension->permissions_data()->GetContentScriptAccess(url, tab_id,
                                                               nullptr) ==
             extensions::PermissionsData::PageAccess::kAllowed;
}

// Returns true if the frame is navigating to an URL either into or out of an
// extension app's extent.
bool CrossesExtensionExtents(blink::WebLocalFrame* frame,
                             const GURL& new_url,
                             bool is_extension_url,
                             bool is_initial_navigation) {
  DCHECK(!frame->Parent());
  GURL old_url(frame->GetDocument().Url());

  extensions::RendererExtensionRegistry* extension_registry =
      extensions::RendererExtensionRegistry::Get();

  // If old_url is still empty and this is an initial navigation, then this is
  // a window.open operation.  We should look at the opener URL.  Note that the
  // opener is a local frame in this case.
  if (is_initial_navigation && old_url.is_empty() && frame->Opener()) {
    blink::WebLocalFrame* opener_frame = frame->Opener()->ToWebLocalFrame();

    // We want to compare against the URL that determines the type of
    // process.  Use the URL of the opener's local frame root, which will
    // correctly handle any site isolation modes (e.g. --site-per-process).
    blink::WebLocalFrame* local_root = opener_frame->LocalRoot();
    old_url = local_root->GetDocument().Url();

    // If we're about to open a normal web page from a same-origin opener stuck
    // in an extension process (other than the Chrome Web Store), we want to
    // keep it in process to allow the opener to script it.
    blink::WebDocument opener_document = opener_frame->GetDocument();
    blink::WebSecurityOrigin opener_origin =
        opener_document.GetSecurityOrigin();
    bool opener_is_extension_url =
        !opener_origin.IsOpaque() && extension_registry->GetExtensionOrAppByURL(
                                         opener_document.Url()) != nullptr;
    const Extension* opener_top_extension =
        extension_registry->GetExtensionOrAppByURL(old_url);
    bool opener_is_web_store =
        opener_top_extension &&
        opener_top_extension->id() == extensions::kWebStoreAppId;
    if (!is_extension_url && !opener_is_extension_url && !opener_is_web_store &&
        IsStandaloneExtensionProcess() &&
        opener_origin.CanRequest(blink::WebURL(new_url)))
      return false;
  }

  return extensions::CrossesExtensionProcessBoundary(
      *extension_registry->GetMainThreadExtensionSet(), old_url, new_url);
}

}  // namespace

ChromeExtensionsRendererClient::ChromeExtensionsRendererClient() {}

ChromeExtensionsRendererClient::~ChromeExtensionsRendererClient() {}

// static
ChromeExtensionsRendererClient* ChromeExtensionsRendererClient::GetInstance() {
  static base::LazyInstance<ChromeExtensionsRendererClient>::Leaky client =
      LAZY_INSTANCE_INITIALIZER;
  return client.Pointer();
}

bool ChromeExtensionsRendererClient::IsIncognitoProcess() const {
  return ChromeRenderThreadObserver::is_incognito_process();
}

int ChromeExtensionsRendererClient::GetLowestIsolatedWorldId() const {
  return ISOLATED_WORLD_ID_EXTENSIONS;
}

extensions::Dispatcher* ChromeExtensionsRendererClient::GetDispatcher() {
  return extension_dispatcher_.get();
}

void ChromeExtensionsRendererClient::OnExtensionLoaded(
    const extensions::Extension& extension) {
  resource_request_policy_->OnExtensionLoaded(extension);
}

void ChromeExtensionsRendererClient::OnExtensionUnloaded(
    const extensions::ExtensionId& extension_id) {
  resource_request_policy_->OnExtensionUnloaded(extension_id);
}

bool ChromeExtensionsRendererClient::ExtensionAPIEnabledForServiceWorkerScript(
    const GURL& scope,
    const GURL& script_url) const {
  if (!script_url.SchemeIs(extensions::kExtensionScheme))
    return false;

  if (!extensions::ExtensionsClient::Get()
           ->ExtensionAPIEnabledInExtensionServiceWorkers())
    return false;

  const Extension* extension =
      extensions::RendererExtensionRegistry::Get()->GetExtensionOrAppByURL(
          script_url);

  if (!extension ||
      !extensions::BackgroundInfo::IsServiceWorkerBased(extension))
    return false;

  if (scope != extension->url())
    return false;

  const std::string& sw_script =
      extensions::BackgroundInfo::GetBackgroundServiceWorkerScript(extension);

  return extension->GetResourceURL(sw_script) == script_url;
}

void ChromeExtensionsRendererClient::RenderThreadStarted() {
  content::RenderThread* thread = content::RenderThread::Get();
  // ChromeRenderViewTest::SetUp() creates its own ExtensionDispatcher and
  // injects it using SetExtensionDispatcher(). Don't overwrite it.
  if (!extension_dispatcher_) {
    extension_dispatcher_ = std::make_unique<extensions::Dispatcher>(
        std::make_unique<ChromeExtensionsDispatcherDelegate>());
    nw::ExtensionDispatcherCreated(extension_dispatcher_.get());
  }
  extension_dispatcher_->OnRenderThreadStarted(thread);
  permissions_policy_delegate_.reset(
      new extensions::RendererPermissionsPolicyDelegate(
          extension_dispatcher_.get()));
  resource_request_policy_.reset(
      new extensions::ResourceRequestPolicy(extension_dispatcher_.get()));
  guest_view_container_dispatcher_.reset(
      new extensions::ExtensionsGuestViewContainerDispatcher());

  thread->AddObserver(extension_dispatcher_.get());
  thread->AddObserver(guest_view_container_dispatcher_.get());
  thread->AddFilter(new CastIPCDispatcher(thread->GetIOTaskRunner()));
}

void ChromeExtensionsRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame,
    service_manager::BinderRegistry* registry) {
  new extensions::ExtensionsRenderFrameObserver(render_frame, registry);
  new extensions::ExtensionFrameHelper(render_frame,
                                       extension_dispatcher_.get());
  extension_dispatcher_->OnRenderFrameCreated(render_frame);
}

bool ChromeExtensionsRendererClient::OverrideCreatePlugin(
    content::RenderFrame* render_frame,
    const blink::WebPluginParams& params) {
  if (params.mime_type.Utf8() != content::kBrowserPluginMimeType)
    return true;

  bool guest_view_api_available = false;
  extension_dispatcher_->script_context_set_iterator()->ForEach(
      render_frame, base::Bind(&IsGuestViewApiAvailableToScriptContext,
                               &guest_view_api_available));
  return !guest_view_api_available;
}

bool ChromeExtensionsRendererClient::AllowPopup() {
  extensions::ScriptContext* current_context =
      extension_dispatcher_->script_context_set().GetCurrent();
  if (!current_context || !current_context->extension())
    return false;

  // See http://crbug.com/117446 for the subtlety of this check.
  switch (current_context->context_type()) {
    case extensions::Feature::UNSPECIFIED_CONTEXT:
    case extensions::Feature::WEB_PAGE_CONTEXT:
    case extensions::Feature::UNBLESSED_EXTENSION_CONTEXT:
    case extensions::Feature::WEBUI_CONTEXT:
    case extensions::Feature::LOCK_SCREEN_EXTENSION_CONTEXT:
      return false;
    case extensions::Feature::BLESSED_EXTENSION_CONTEXT:
      return !current_context->IsForServiceWorker();
    case extensions::Feature::CONTENT_SCRIPT_CONTEXT:
      return true;
    case extensions::Feature::BLESSED_WEB_PAGE_CONTEXT:
      return !current_context->web_frame()->Parent();
    default:
      NOTREACHED();
      return false;
  }
}

void ChromeExtensionsRendererClient::WillSendRequest(
    blink::WebLocalFrame* frame,
    ui::PageTransition transition_type,
    const blink::WebURL& url,
    const net::SiteForCookies& site_for_cookies,
    const url::Origin* initiator_origin,
    GURL* new_url,
    bool* attach_same_site_cookies) {
  std::string extension_id;
  GURL request_url(url);
  if (initiator_origin &&
      initiator_origin->scheme() == extensions::kExtensionScheme) {
    extension_id = initiator_origin->host();
  } else {
    if (site_for_cookies.scheme() == extensions::kExtensionScheme) {
      extension_id = site_for_cookies.registrable_domain();
    }
  }

  if (!extension_id.empty()) {
    const extensions::RendererExtensionRegistry* extension_registry =
        extensions::RendererExtensionRegistry::Get();
    const Extension* extension = extension_registry->GetByID(extension_id);
    if (extension) {
      int tab_id = extensions::ExtensionFrameHelper::Get(
                       content::RenderFrame::FromWebFrame(frame))
                       ->tab_id();
      bool extension_has_access_to_request_url =
          ExtensionHasAccessToUrl(extension, tab_id, request_url);

      bool initiator_ok = true;
      // In the case where the site_for_cookies is an extension URL, we also
      // want to check that the initiator and the requested URL are same-site,
      // and that the extension has permission for both the requested URL and
      // the initiator origin.
      // Ideally we would walk up the frame tree and check that each ancestor is
      // first-party to the main frame (treating the extension as "first-party"
      // to any URLs it has permission for). But for now we make do with just
      // checking the direct initiator of the request.
      // We also want to check same-siteness between the initiator and the
      // requested URL, because setting |attach_same_site_cookies| to true
      // causes Strict cookies to be attached, and having the initiator be
      // same-site to the request URL is a requirement for Strict cookies
      // (see net::cookie_util::ComputeSameSiteContext).
      if (initiator_origin &&
          initiator_origin->scheme() != extensions::kExtensionScheme) {
        initiator_ok =
            ExtensionHasAccessToUrl(extension, tab_id,
                                    initiator_origin->GetURL()) &&
            net::registry_controlled_domains::SameDomainOrHost(
                request_url, *initiator_origin,
                net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
      }

      *attach_same_site_cookies =
          extension_has_access_to_request_url && initiator_ok;
    } else {
      // If there is no extension installed for the origin, it may be from a
      // recently uninstalled extension.  The tabs of such extensions are
      // automatically closed, but subframes and content scripts may stick
      // around. Fail such requests without killing the process.
      *new_url = GURL(chrome::kExtensionInvalidRequestURL);
    }
  }

  if (url.ProtocolIs(extensions::kExtensionScheme) &&
      !resource_request_policy_->CanRequestResource(GURL(url), frame,
                                                    transition_type)) {
    *new_url = GURL(chrome::kExtensionInvalidRequestURL);
  }

  // TODO(https://crbug.com/588766): Remove metrics after bug is fixed.
  if (url.ProtocolIs(extensions::kExtensionScheme) &&
      request_url.host_piece() == extension_misc::kDocsOfflineExtensionId) {
    if (!ukm_recorder_) {
      mojo::PendingRemote<ukm::mojom::UkmRecorderInterface> recorder;
      content::RenderThread::Get()->BindHostReceiver(
          recorder.InitWithNewPipeAndPassReceiver());
      ukm_recorder_ =
          std::make_unique<ukm::MojoUkmRecorder>(std::move(recorder));
    }

    const ukm::SourceId source_id = frame->GetDocument().GetUkmSourceId();
    ukm::builders::GoogleDocsOfflineExtension(source_id)
        .SetResourceRequested(true)
        .Record(ukm_recorder_.get());

    bool is_available = extensions::RendererExtensionRegistry::Get()->GetByID(
                            extension_misc::kDocsOfflineExtensionId) != nullptr;
    bool is_incognito = IsIncognitoProcess();
    GoogleDocsExtensionAvailablity vote;
    if (is_incognito) {
      vote = is_available
                 ? GoogleDocsExtensionAvailablity::kAvailableIncognito
                 : GoogleDocsExtensionAvailablity::kNotAvailableIncognito;
    } else {
      vote = is_available
                 ? GoogleDocsExtensionAvailablity::kAvailableRegular
                 : GoogleDocsExtensionAvailablity::kNotAvailableRegular;
    }
    base::UmaHistogramEnumeration(
        "Extensions.GoogleDocOffline.AvailabilityOnResourceRequest", vote);
  }
}

void ChromeExtensionsRendererClient::SetExtensionDispatcherForTest(
    std::unique_ptr<extensions::Dispatcher> extension_dispatcher) {
  extension_dispatcher_ = std::move(extension_dispatcher);
  permissions_policy_delegate_.reset(
      new extensions::RendererPermissionsPolicyDelegate(
          extension_dispatcher_.get()));
}

extensions::Dispatcher*
ChromeExtensionsRendererClient::GetExtensionDispatcherForTest() {
  return extension_dispatcher();
}

// static
bool ChromeExtensionsRendererClient::ShouldFork(blink::WebLocalFrame* frame,
                                                const GURL& url,
                                                bool is_initial_navigation,
                                                bool is_server_redirect) {
  const extensions::RendererExtensionRegistry* extension_registry =
      extensions::RendererExtensionRegistry::Get();

  // Determine if the new URL is an extension (excluding bookmark apps).
  const Extension* new_url_extension = extensions::GetNonBookmarkAppExtension(
      *extension_registry->GetMainThreadExtensionSet(), url);
  bool is_extension_url = !!new_url_extension;

  // If the navigation would cross an app extent boundary, we also need
  // to defer to the browser to ensure process isolation.  This is not necessary
  // for server redirects, which will be transferred to a new process by the
  // browser process when they are ready to commit.  It is necessary for client
  // redirects, which won't be transferred in the same way.
  if (!is_server_redirect &&
      CrossesExtensionExtents(frame, url, is_extension_url,
                              is_initial_navigation)) {
    const Extension* extension =
        extension_registry->GetExtensionOrAppByURL(url);
    if (extension && extension->is_app()) {
      extensions::RecordAppLaunchType(
          extension_misc::APP_LAUNCH_CONTENT_NAVIGATION, extension->GetType());
    }
    return true;
  }

  return false;
}

// static
content::BrowserPluginDelegate*
ChromeExtensionsRendererClient::CreateBrowserPluginDelegate(
    content::RenderFrame* render_frame,
    const content::WebPluginInfo& info,
    const std::string& mime_type,
    const GURL& original_url) {
  if (mime_type == content::kBrowserPluginMimeType)
    return new extensions::ExtensionsGuestViewContainer(render_frame);
  return new extensions::MimeHandlerViewContainer(render_frame, info, mime_type,
                                                  original_url);
}

// static
void ChromeExtensionsRendererClient::DidBlockMimeHandlerViewForDisallowedPlugin(
    const blink::WebElement& plugin_element) {
  extensions::MimeHandlerViewContainerManager::Get(
      content::RenderFrame::FromWebFrame(
          plugin_element.GetDocument().GetFrame()),
      true /* create_if_does_not_exist */)
      ->DidBlockMimeHandlerViewForDisallowedPlugin(plugin_element);
}

// static
bool ChromeExtensionsRendererClient::MaybeCreateMimeHandlerView(
    const blink::WebElement& plugin_element,
    const GURL& resource_url,
    const std::string& mime_type,
    const content::WebPluginInfo& plugin_info) {
  return extensions::MimeHandlerViewContainerManager::Get(
             content::RenderFrame::FromWebFrame(
                 plugin_element.GetDocument().GetFrame()),
             true /* create_if_does_not_exist */)
      ->CreateFrameContainer(plugin_element, resource_url, mime_type,
                             plugin_info);
}

v8::Local<v8::Object> ChromeExtensionsRendererClient::GetScriptableObject(
    const blink::WebElement& plugin_element,
    v8::Isolate* isolate) {
  // If there is a MimeHandlerView that can provide the scriptable object then
  // MaybeCreateMimeHandlerView must have been called before and a container
  // manager should exist.
  auto* container_manager = extensions::MimeHandlerViewContainerManager::Get(
      content::RenderFrame::FromWebFrame(
          plugin_element.GetDocument().GetFrame()),
      false /* create_if_does_not_exist */);
  if (container_manager)
    return container_manager->GetScriptableObject(plugin_element, isolate);
  return v8::Local<v8::Object>();
}

// static
blink::WebFrame* ChromeExtensionsRendererClient::FindFrame(
    blink::WebLocalFrame* relative_to_frame,
    const std::string& name) {
  content::RenderFrame* result = extensions::ExtensionFrameHelper::FindFrame(
      content::RenderFrame::FromWebFrame(relative_to_frame), name);
  return result ? result->GetWebFrame() : nullptr;
}

void ChromeExtensionsRendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
  extension_dispatcher_->RunScriptsAtDocumentStart(render_frame);
}

void ChromeExtensionsRendererClient::RunScriptsAtDocumentEnd(
    content::RenderFrame* render_frame) {
  extension_dispatcher_->RunScriptsAtDocumentEnd(render_frame);
}

void ChromeExtensionsRendererClient::RunScriptsAtDocumentIdle(
    content::RenderFrame* render_frame) {
  extension_dispatcher_->RunScriptsAtDocumentIdle(render_frame);
}
