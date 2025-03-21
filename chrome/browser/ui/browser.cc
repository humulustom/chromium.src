// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/browser.h"

#include "chrome/browser/web_applications/components/web_app_helpers.h"

#include <stddef.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "extensions/common/extension_messages.h"
#include "extensions/browser/app_window/app_window.h"
#include "content/nw/src/nw_content.h"
#include "content/nw/src/nw_base.h"
#include "content/public/common/content_switches.h"
#include "ui/display/display.h"
#include "chrome/browser/extensions/extension_tab_util.h"

#include "base/base_paths.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/user_metrics.h"
#include "base/optional.h"
#include "base/process/process_info.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/app_mode/app_mode_utils.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/background/background_contents.h"
#include "chrome/browser/background/background_contents_service.h"
#include "chrome/browser/background/background_contents_service_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/content_settings/mixed_content_settings_tab_helper.h"
#include "chrome/browser/content_settings/sound_content_setting_observer.h"
#include "chrome/browser/content_settings/tab_specific_content_settings.h"
#include "chrome/browser/custom_handlers/protocol_handler_registry.h"
#include "chrome/browser/custom_handlers/protocol_handler_registry_factory.h"
#include "chrome/browser/custom_handlers/register_protocol_handler_permission_request.h"
#include "chrome/browser/defaults.h"
#include "chrome/browser/devtools/devtools_toggle_action.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/extensions/api/tabs/tabs_event_router.h"
#include "chrome/browser/extensions/api/tabs/tabs_windows_api.h"
#include "chrome/browser/extensions/browser_extension_window_controller.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/extension_ui_util.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/extensions/tab_helper.h"
#include "chrome/browser/file_select_helper.h"
#include "chrome/browser/first_run/first_run.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/notifications/notification_ui_manager.h"
#include "chrome/browser/permissions/permission_request_manager.h"
#include "chrome/browser/picture_in_picture/picture_in_picture_window_manager.h"
#include "chrome/browser/policy/developer_tools_policy_handler.h"
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/printing/background_printing_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/profiles/profile_metrics.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/repost_form_warning_controller.h"
#include "chrome/browser/resource_coordinator/tab_load_tracker.h"
#include "chrome/browser/resource_coordinator/tab_manager_web_contents_data.h"
#include "chrome/browser/search/search.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "chrome/browser/ssl/security_state_tab_helper.h"
#include "chrome/browser/subresource_filter/chrome_subresource_filter_client.h"
#include "chrome/browser/tab_contents/tab_util.h"
#include "chrome/browser/task_manager/web_contents_tags.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/ui/autofill/chrome_autofill_client.h"
#include "chrome/browser/ui/blocked_content/framebust_block_tab_helper.h"
#include "chrome/browser/ui/blocked_content/list_item_position.h"
#include "chrome/browser/ui/blocked_content/popup_blocker.h"
#include "chrome/browser/ui/blocked_content/popup_blocker_tab_helper.h"
#include "chrome/browser/ui/blocked_content/popup_tracker.h"
#include "chrome/browser/ui/bluetooth/bluetooth_chooser_controller.h"
#include "chrome/browser/ui/bluetooth/bluetooth_chooser_desktop.h"
#include "chrome/browser/ui/bluetooth/bluetooth_scanning_prompt_controller.h"
#include "chrome/browser/ui/bluetooth/bluetooth_scanning_prompt_desktop.h"
#include "chrome/browser/ui/bookmarks/bookmark_tab_helper.h"
#include "chrome/browser/ui/bookmarks/bookmark_utils.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_content_setting_bubble_model_delegate.h"
#include "chrome/browser/ui/browser_dialogs.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_instant_controller.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_live_tab_context.h"
#include "chrome/browser/ui/browser_location_bar_model_delegate.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_tab_strip_model_delegate.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_ui_prefs.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/chrome_bubble_manager.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/chrome_select_file_policy.h"
#include "chrome/browser/ui/color_chooser.h"
#include "chrome/browser/ui/exclusive_access/fullscreen_controller.h"
#include "chrome/browser/ui/exclusive_access/mouse_lock_controller.h"
#include "chrome/browser/ui/find_bar/find_bar.h"
#include "chrome/browser/ui/find_bar/find_bar_controller.h"
#include "chrome/browser/ui/global_error/global_error.h"
#include "chrome/browser/ui/global_error/global_error_service.h"
#include "chrome/browser/ui/global_error/global_error_service_factory.h"
#include "chrome/browser/ui/javascript_dialogs/javascript_dialog_tab_helper.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/page_action/page_action_icon_type.h"
#include "chrome/browser/ui/permission_bubble/chooser_bubble_delegate.h"
#include "chrome/browser/ui/search/search_tab_helper.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/status_bubble.h"
#include "chrome/browser/ui/sync/browser_synced_window_delegate.h"
#include "chrome/browser/ui/tab_contents/core_tab_helper.h"
#include "chrome/browser/ui/tab_dialogs.h"
#include "chrome/browser/ui/tab_helpers.h"
#include "chrome/browser/ui/tab_modal_confirm_dialog.h"
#include "chrome/browser/ui/tabs/tab_group.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_menu_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_utils.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/web_applications/app_browser_controller.h"
#include "chrome/browser/ui/webui/signin/login_ui_service.h"
#include "chrome/browser/ui/webui/signin/login_ui_service_factory.h"
#include "chrome/browser/ui/window_sizer/window_sizer.h"
#include "chrome/browser/vr/vr_tab_helper.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/custom_handlers/protocol_handler.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/ssl_insecure_content.h"
#include "chrome/common/url_constants.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/chromium_strings.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/bookmark_pref_names.h"
#include "components/bubble/bubble_controller.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/find_in_page/find_tab_helper.h"
#include "components/keep_alive_registry/keep_alive_registry.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/keep_alive_registry/scoped_keep_alive.h"
#include "components/omnibox/browser/location_bar_model_impl.h"
#include "components/page_load_metrics/browser/metrics_web_contents_observer.h"
#include "components/page_load_metrics/common/page_load_metrics.mojom.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/content/triggers/ad_redirect_trigger.h"
#include "components/search/search.h"
#include "components/security_state/content/content_utils.h"
#include "components/security_state/core/security_state.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/sessions/core/session_types.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/startup_metric_utils/browser/startup_metric_utils.h"
#include "components/subresource_filter/content/browser/content_subresource_filter_throttle_manager.h"
#include "components/translate/core/browser/language_state.h"
#include "components/user_manager/user_manager.h"
#include "components/viz/common/surfaces/surface_id.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "components/zoom/zoom_controller.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/interstitial_page.h"
#include "content/public/browser/invalidate_type.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/content_features.h"
#include "content/public/common/page_zoom.h"
#include "content/public/common/profiling.h"
#include "content/public/common/url_constants.h"
#include "content/public/common/webplugininfo.h"
#include "content/public/common/window_container_type.mojom-shared.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/guest_view/mime_handler_view/mime_handler_view_guest.h"
#include "extensions/buildflags/buildflags.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_handlers/background_info.h"
#include "net/base/filename_util.h"
#include "ppapi/buildflags/buildflags.h"
#include "third_party/blink/public/mojom/frame/blocked_navigation_types.mojom.h"
#include "third_party/blink/public/mojom/frame/fullscreen.mojom.h"
#include "third_party/blink/public/mojom/web_feature/web_feature.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/geometry/point.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"

#if defined(OS_WIN)
#include <shellapi.h>
#include <windows.h>

#include "chrome/browser/ui/view_ids.h"
#include "components/autofill/core/browser/autofill_ie_toolbar_import_win.h"
#include "ui/base/win/shell.h"
#endif  // defined(OS_WIN)

#if defined(OS_CHROMEOS)
#include "chrome/browser/ui/settings_window_manager_chromeos.h"
#include "components/session_manager/core/session_manager.h"
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "chrome/browser/extensions/extension_browser_window_helper.h"
#endif

#if BUILDFLAG(ENABLE_PLUGINS)
#include "chrome/browser/pepper_broker_infobar_delegate.h"
#include "chrome/browser/plugins/plugin_finder.h"
#include "chrome/browser/plugins/plugin_metadata.h"
#include "content/public/browser/plugin_service.h"
#endif

#if BUILDFLAG(ENABLE_PRINTING)
#include "components/printing/browser/print_composite_client.h"
#endif

using base::TimeDelta;
using base::UserMetricsAction;
using content::NativeWebKeyboardEvent;
using content::NavigationController;
using content::NavigationEntry;
using content::OpenURLParams;
using content::Referrer;
using content::RenderWidgetHostView;
using content::SiteInstance;
using content::WebContents;
using extensions::Extension;
using ui::WebDialogDelegate;
using web_modal::WebContentsModalDialogManager;

///////////////////////////////////////////////////////////////////////////////

namespace {

// How long we wait before updating the browser chrome while loading a page.
constexpr TimeDelta kUIUpdateCoalescingTime = TimeDelta::FromMilliseconds(200);

BrowserWindow* CreateBrowserWindow(std::unique_ptr<Browser> browser,
                                   bool user_gesture) {
  return BrowserWindow::CreateBrowserWindow(std::move(browser), user_gesture);
}

const extensions::Extension* GetExtensionForOrigin(
    Profile* profile,
    const GURL& security_origin) {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (!security_origin.SchemeIs(extensions::kExtensionScheme))
    return nullptr;

  const extensions::Extension* extension =
      extensions::ExtensionRegistry::Get(profile)->enabled_extensions().GetByID(
          security_origin.host());
  DCHECK(extension);
  return extension;
#else
  return nullptr;
#endif
}

// Returns whether a browser window can be created for the specified profile.
bool CanCreateBrowserForProfile(Profile* profile) {
  return IncognitoModePrefs::CanOpenBrowser(profile) &&
         (!profile->IsGuestSession() || profile->IsOffTheRecord()) &&
         profile->AllowsBrowserWindows();
}
bool IsOnKioskSplashScreen() {
#if defined(OS_CHROMEOS)
  session_manager::SessionManager* session_manager =
      session_manager::SessionManager::Get();
  if (!session_manager)
    return false;
  // We have to check this way because of CHECK() in UserManager::Get().
  if (!user_manager::UserManager::IsInitialized())
    return false;
  user_manager::UserManager* user_manager = user_manager::UserManager::Get();
  if (!user_manager->IsLoggedInAsAnyKioskApp())
    return false;
  if (session_manager->session_state() !=
      session_manager::SessionState::LOGIN_PRIMARY)
    return false;
  return true;
#else
  return false;
#endif
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// Browser, CreateParams:

Browser::CreateParams::~CreateParams() {}

Browser::CreateParams::CreateParams(Profile* profile, bool user_gesture)
    : CreateParams(TYPE_POPUP, profile, user_gesture) {}

Browser::CreateParams::CreateParams(Profile* profile, bool user_gesture, const gfx::Rect& bounds)
  : CreateParams(TYPE_POPUP, profile, user_gesture) {
  initial_bounds = bounds;
}

Browser::CreateParams::CreateParams(Type type,
                                    Profile* profile,
                                    bool user_gesture)
    : type(type == TYPE_DEVTOOLS ? TYPE_DEVTOOLS : TYPE_POPUP), profile(profile), user_gesture(user_gesture) {}

Browser::CreateParams::CreateParams(const CreateParams& other) = default;

// static
Browser::CreateParams Browser::CreateParams::CreateForAppBase(
    bool is_popup,
    const std::string& app_name,
    bool trusted_source,
    const gfx::Rect& window_bounds,
    Profile* profile,
    bool user_gesture) {
  DCHECK(!app_name.empty());

  CreateParams params(is_popup ? Type::TYPE_APP_POPUP : Type::TYPE_APP, profile,
                      user_gesture);
  params.app_name = app_name;
  params.trusted_source = trusted_source;
  params.initial_bounds = window_bounds;

  return params;
}

// static
Browser::CreateParams Browser::CreateParams::CreateForApp(
    const std::string& app_name,
    bool trusted_source,
    const gfx::Rect& window_bounds,
    Profile* profile,
    bool user_gesture) {
  return CreateForAppBase(false, app_name, trusted_source, window_bounds,
                          profile, user_gesture);
}

// static
Browser::CreateParams Browser::CreateParams::CreateForAppPopup(
    const std::string& app_name,
    bool trusted_source,
    const gfx::Rect& window_bounds,
    Profile* profile,
    bool user_gesture) {
  return CreateForAppBase(true, app_name, trusted_source, window_bounds,
                          profile, user_gesture);
}

// static
Browser::CreateParams Browser::CreateParams::CreateForDevTools(
    Profile* profile) {
  CreateParams params(TYPE_DEVTOOLS, profile, true);
  params.app_name = DevToolsWindow::kDevToolsApp;
  params.trusted_source = true;
  return params;
}

////////////////////////////////////////////////////////////////////////////////
// Browser, InterstitialObserver:

class Browser::InterstitialObserver : public content::WebContentsObserver {
 public:
  InterstitialObserver(Browser* browser, content::WebContents* web_contents)
      : WebContentsObserver(web_contents), browser_(browser) {}

  void DidAttachInterstitialPage() override {
    browser_->UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_TAB_STATE);
  }

  void DidDetachInterstitialPage() override {
    browser_->UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_TAB_STATE);
  }

 private:
  Browser* browser_;

  DISALLOW_COPY_AND_ASSIGN(InterstitialObserver);
};

void Browser::AddOnDidFinishFirstNavigationCallback(
    DidFinishFirstNavigationCallback callback) {
  on_did_finish_first_navigation_callbacks_.push_back(std::move(callback));
}

void Browser::OnDidFinishFirstNavigation() {
  if (did_finish_first_navigation_)
    return;
  did_finish_first_navigation_ = true;
  std::vector<DidFinishFirstNavigationCallback> callbacks;
  std::swap(callbacks, on_did_finish_first_navigation_callbacks_);
  for (auto&& callback : callbacks)
    std::move(callback).Run(true /* did_finish */);
}

///////////////////////////////////////////////////////////////////////////////
// Browser, Constructors, Creation, Showing:

// static
Browser* Browser::Create(const CreateParams& params) {
  if (!CanCreateBrowserForProfile(params.profile))
    return nullptr;
  // Disable browser creation on kiosk mode while loading.
  if (IsOnKioskSplashScreen())
    return nullptr;

  Browser* ret = new Browser(params);
  nw::CreateAppWindowHook(nullptr);
  return ret;
}

Browser::Browser(const CreateParams& params)
   :  nw_menu_(nullptr),
      create_params_(params),
      frameless_(params.frameless),
      alpha_enabled_(params.alpha_enabled),
      type_(params.type),
      profile_(params.profile),
      window_(nullptr),
      tab_strip_model_delegate_(
          std::make_unique<chrome::BrowserTabStripModelDelegate>(this)),
      tab_strip_model_(
          std::make_unique<TabStripModel>(tab_strip_model_delegate_.get(),
                                          params.profile)),
      app_name_(params.app_name),
      windows_key_(params.windows_key),
      is_trusted_source_(params.trusted_source),
      is_focus_mode_(params.is_focus_mode),
      session_id_(SessionID::NewUnique()),
      cancel_download_confirmation_state_(NOT_PROMPTED),
      override_bounds_(params.initial_bounds),
      initial_show_state_(params.initial_show_state),
      initial_workspace_(params.initial_workspace),
      initial_ontop_(params.always_on_top),
      initial_allvisible_(params.all_visible),
      initial_resizable_(params.resizable),
      initial_showintaskbar_(params.show_in_taskbar),
      initial_position_(params.position),
      title_override_(params.title),
      icon_override_(params.icon),
      is_session_restore_(params.is_session_restore),
      unload_controller_(this),
      content_setting_bubble_model_delegate_(
          new BrowserContentSettingBubbleModelDelegate(this)),
      location_bar_model_delegate_(new BrowserLocationBarModelDelegate(this)),
      live_tab_context_(new BrowserLiveTabContext(this)),
      synced_window_delegate_(new BrowserSyncedWindowDelegate(this)),
      app_controller_(
          web_app::AppBrowserController::MaybeCreateWebAppController(this)),
      bookmark_bar_state_(BookmarkBar::HIDDEN),
      command_controller_(new chrome::BrowserCommandController(this)),
      window_has_shown_(false)
#if BUILDFLAG(ENABLE_EXTENSIONS)
      ,
      extension_browser_window_helper_(
          std::make_unique<extensions::ExtensionBrowserWindowHelper>(this))
#endif
{
  // If this causes a crash then a window is being opened using a profile type
  // that is disallowed by policy. The crash prevents the disabled window type
  // from opening at all, but the path that triggered it should be fixed.
  CHECK(IncognitoModePrefs::CanOpenBrowser(profile_));
  CHECK(!profile_->IsGuestSession() || profile_->IsOffTheRecord())
      << "Only off the record browser may be opened in guest mode";
  CHECK(!profile_->IsSystemProfile())
      << "The system profile should never have a real browser.";

  content::g_support_transparency = !base::CommandLine::ForCurrentProcess()->HasSwitch(::switches::kDisableTransparency);
  if (content::g_support_transparency) {
    content::g_force_cpu_draw = base::CommandLine::ForCurrentProcess()->HasSwitch(::switches::kForceCpuDraw);
  }

  tab_strip_model_->AddObserver(this);

  location_bar_model_ = std::make_unique<LocationBarModelImpl>(
      location_bar_model_delegate_.get(), content::kMaxURLDisplayChars);

  registrar_.Add(this, chrome::NOTIFICATION_BROWSER_THEME_CHANGED,
                 content::Source<ThemeService>(
                     ThemeServiceFactory::GetForProfile(profile_)));

  profile_pref_registrar_.Init(profile_->GetPrefs());
  profile_pref_registrar_.Add(
      prefs::kDevToolsAvailability,
      base::BindRepeating(&Browser::OnDevToolsAvailabilityChanged,
                          base::Unretained(this)));
  profile_pref_registrar_.Add(
      bookmarks::prefs::kShowBookmarkBar,
      base::BindRepeating(&Browser::UpdateBookmarkBarState,
                          base::Unretained(this),
                          BOOKMARK_BAR_STATE_CHANGE_PREF_CHANGE));

  if (search::IsInstantExtendedAPIEnabled() && is_type_normal())
    instant_controller_ = std::make_unique<BrowserInstantController>(this);

  UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_INIT);

  ProfileMetrics::LogProfileLaunch(profile_);

  if (params.skip_window_init_for_testing)
    return;

  window_ = params.window ? params.window
                          : CreateBrowserWindow(std::unique_ptr<Browser>(this),
                                                params.user_gesture);

  if (!initial_showintaskbar_)
    window_->SetShowInTaskbar(false);

  if (app_controller_)
    app_controller_->UpdateCustomTabBarVisibility(false);

  // Create the extension window controller before sending notifications.
  extension_window_controller_.reset(
      new extensions::BrowserExtensionWindowController(this));

  SessionService* session_service =
      SessionServiceFactory::GetForProfileForSessionRestore(profile_);
  if (session_service)
    session_service->WindowOpened(this);

  // TODO(beng): move to ChromeBrowserMain:
  if (first_run::ShouldDoPersonalDataManagerFirstRun()) {
#if defined(OS_WIN)
    // Notify PDM that this is a first run.
    ImportAutofillDataWin(
        autofill::PersonalDataManagerFactory::GetForProfile(profile_));
#endif  // defined(OS_WIN)
  }

  exclusive_access_manager_.reset(
      new ExclusiveAccessManager(window_->GetExclusiveAccessContext()));

  BrowserList::AddBrowser(this);

  // SetIsInTabDragging() will set the fast resize bit for the web contents.
  // It will be reset after the drag ends.
  if (params.in_tab_dragging)
    SetIsInTabDragging(true);

  if (is_focus_mode_)
    focus_mode_start_time_ = base::TimeTicks::Now();
}

Browser::~Browser() {
  // Stop observing notifications and destroy the tab monitor before continuing
  // with destruction. Profile destruction will unload extensions and reentrant
  // calls to Browser:: should be avoided while it is being torn down.
  registrar_.RemoveAll();
  extension_browser_window_helper_.reset();

  // Like above, cancel delayed method calls into |this| to avoid re-entrancy.
  // This is necessary because ~TestingProfile (called below for incognito
  // |profile_|) spins a RunLoop.
  weak_factory_.InvalidateWeakPtrs();

  // The tab strip should not have any tabs at this point.
  DCHECK(tab_strip_model_->empty());
  bubble_manager_.reset();

  // Destroy the BrowserCommandController before removing the browser, so that
  // it doesn't act on any notifications that are sent as a result of removing
  // the browser.
  command_controller_.reset();
  BrowserList::RemoveBrowser(this);

  // If closing the window is going to trigger a shutdown, then we need to
  // schedule all active downloads to be cancelled. This needs to be after
  // removing |this| from BrowserList so that OkToClose...() can determine
  // whether there are any other windows open for the browser.
  int num_downloads;
  if (!browser_defaults::kBrowserAliveWithNoWindows &&
      OkToCloseWithInProgressDownloads(&num_downloads) ==
          DownloadCloseType::kBrowserShutdown) {
    DownloadCoreService::CancelAllDownloads();
  }

  SessionService* session_service =
      SessionServiceFactory::GetForProfile(profile_);
  if (session_service)
    session_service->WindowClosed(session_id_);

  sessions::TabRestoreService* tab_restore_service =
      TabRestoreServiceFactory::GetForProfile(profile());
  if (tab_restore_service)
    tab_restore_service->BrowserClosed(live_tab_context());

  profile_pref_registrar_.RemoveAll();

  // Destroy BrowserExtensionWindowController before the incognito profile
  // is destroyed to make sure the chrome.windows.onRemoved event is sent.
  extension_window_controller_.reset();

  // Destroy BrowserInstantController before the incongnito profile is destroyed
  // because the InstantController destructor depends on this profile.
  instant_controller_.reset();

  // The system incognito profile should not try be destroyed using
  // ProfileDestroyer::DestroyProfileWhenAppropriate(). This profile can be
  // used, at least, by the user manager window. This window is not a browser,
  // therefore, BrowserList::IsIncognitoSessionActiveForProfile(profile_)
  // returns false, while the user manager window is still opened.
  // This cannot be fixed in ProfileDestroyer::DestroyProfileWhenAppropriate(),
  // because the ProfileManager needs to be able to destroy all profiles when
  // it is destroyed. See crbug.com/527035
  //
  // A profile created with Profile::CreateOffTheRecordProfile() should not be
  // destroyed directly by Browser (e.g. for offscreen tabs,
  // https://crbug.com/664351).
  if (profile_->IsOffTheRecord() &&
      !profile_->IsIndependentOffTheRecordProfile() &&
      !BrowserList::IsIncognitoSessionInUse(profile_) &&
      !profile_->GetOriginalProfile()->IsSystemProfile()) {
    if (profile_->IsGuestSession()) {
// ChromeOS handles guest data independently.
#if !defined(OS_CHROMEOS)
      // Clear all browsing data once a Guest Session completes. The Guest
      // profile has BrowserContextKeyedServices that the Incognito profile
      // doesn't, so the ProfileDestroyer can't delete it properly.
      // TODO(mlerman): Delete the guest using an improved ProfileDestroyer.
      profiles::RemoveBrowsingDataForProfile(profile_->GetPath());
#endif
    } else {
#if BUILDFLAG(ENABLE_PRINT_PREVIEW)
      // The Printing Background Manager holds onto preview dialog WebContents
      // whose corresponding print jobs have not yet fully spooled. Make sure
      // these get destroyed before tearing down the incognito profile so that
      // their render frame hosts can exit in time - see crbug.com/579155
      g_browser_process->background_printing_manager()
          ->DeletePreviewContentsForBrowserContext(profile_);
#endif
      // An incognito profile is no longer needed, this indirectly frees
      // its cache and cookies once it gets destroyed at the appropriate time.
      ProfileDestroyer::DestroyProfileWhenAppropriate(profile_);
    }
  }

  // There may be pending file dialogs, we need to tell them that we've gone
  // away so they don't try and call back to us.
  if (select_file_dialog_.get())
    select_file_dialog_->ListenerDestroyed();

  if (is_focus_mode_) {
    auto duration = base::TimeTicks::Now() - focus_mode_start_time_;
    UMA_HISTOGRAM_CUSTOM_COUNTS("Session.TimeSpentInFocusMode",
                                duration.InSeconds(), 1,
                                base::TimeDelta::FromHours(24).InSeconds(), 50);
  }
}

bool Browser::NWCanClose(bool user_force) {
  WebContents* web_contents = tab_strip_model_->GetActiveWebContents();
  if (!web_contents)
    return true;
  const extensions::Extension* extension =
            extensions::ProcessManager::Get(profile_)
                ->GetExtensionForWebContents(web_contents);
  if (!extension)
    return true;
  //content::RenderFrameHost* rfh = web_contents->GetMainFrame();
  extensions::EventRouter* event_router = extensions::EventRouter::Get(profile_);
  std::string listener_extension_id;
  int instance_id = extensions::ExtensionTabUtil::GetWindowId(this);
  bool listening_to_close = event_router->
    ExtensionHasEventListener(extension->id(), extensions::api::windows::OnRemoving::kEventName,
                              extensions::ExtensionTabUtil::GetWindowId(this),
                              &listener_extension_id);
  if (listening_to_close) {
    std::unique_ptr<base::ListValue> args(new base::ListValue());
    args->AppendInteger(session_id().id());
    if (user_force)
      args->AppendString("quit");
    auto event =
      std::make_unique<extensions::Event>(extensions::events::UNKNOWN,
                                          extensions::api::windows::OnRemoving::kEventName,
                                          std::move(args), profile());
    event->filter_info = extensions::EventFilteringInfo();
    event->filter_info.window_exposed_by_default = true;
    event->filter_info.instance_id = instance_id;
    event_router->BroadcastEvent(std::move(event));
    return false;
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// Getters & Setters

ChromeBubbleManager* Browser::GetBubbleManager() {
  if (!bubble_manager_)
    bubble_manager_ =
        std::make_unique<ChromeBubbleManager>(tab_strip_model_.get());
  return bubble_manager_.get();
}

FindBarController* Browser::GetFindBarController() {
  if (!find_bar_controller_.get()) {
    find_bar_controller_ =
        std::make_unique<FindBarController>(window_->CreateFindBar());
    find_bar_controller_->find_bar()->SetFindBarController(
        find_bar_controller_.get());
    find_bar_controller_->ChangeWebContents(
        tab_strip_model_->GetActiveWebContents());
    find_bar_controller_->find_bar()->MoveWindowIfNecessary();
  }
  return find_bar_controller_.get();
}

bool Browser::HasFindBarController() const {
  return find_bar_controller_.get() != NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Browser, State Storage and Retrieval for UI:

GURL Browser::GetNewTabURL() const {
  if (app_controller_)
    return app_controller_->GetAppLaunchURL();
  return GURL(chrome::kChromeUINewTabURL);
}

gfx::Image Browser::GetCurrentPageIcon() const {
  if (!icon_override_.IsEmpty())
    return icon_override_;
  WebContents* web_contents = tab_strip_model_->GetActiveWebContents();
  // |web_contents| can be NULL since GetCurrentPageIcon() is called by the
  // window during the window's creation (before tabs have been added).
  favicon::FaviconDriver* favicon_driver =
      web_contents
          ? favicon::ContentFaviconDriver::FromWebContents(web_contents)
          : nullptr;
  return favicon_driver ? favicon_driver->GetFavicon() : gfx::Image();
}

base::string16 Browser::GetWindowTitleForCurrentTab(
    bool include_app_name) const {
  return GetWindowTitleFromWebContents(
      include_app_name, tab_strip_model_->GetActiveWebContents());
}

base::string16 Browser::GetWindowTitleForTab(bool include_app_name,
                                             int index) const {
  return GetWindowTitleFromWebContents(
      include_app_name, tab_strip_model_->GetWebContentsAt(index));
}

base::string16 Browser::GetWindowTitleFromWebContents(
    bool include_app_name,
    content::WebContents* contents) const {
  base::string16 title;
  base::string16 override = base::UTF8ToUTF16(title_override_);

  // |contents| can be NULL because GetWindowTitleForCurrentTab is called by the
  // window during the window's creation (before tabs have been added).
  content::NavigationEntry* entry = contents ?
      contents->GetController().GetLastCommittedEntry() : nullptr;
  if (!entry || entry->GetTitle().empty()) {
    if (override.empty()) {
      const std::string extension_id =
        web_app::GetAppIdFromApplicationName(app_name());
      const Extension* extension =
        extensions::ExtensionRegistry::Get(profile())
          ->GetExtensionById(extension_id,
                             extensions::ExtensionRegistry::EVERYTHING);
      if (extension)
        title = base::UTF8ToUTF16(extension->name());
    } else
      title = override;
  } else if (contents) {
    title = FormatTitleForDisplay(app_controller_ ? app_controller_->GetTitle()
                                                  : contents->GetTitle());
  }

  // If there is no title, leave it empty for apps.
  if (title.empty() && (is_type_normal() || is_type_popup()))
    title = CoreTabHelper::GetDefaultTitle();

#if defined(OS_MACOSX)
  // On Mac, we don't want to suffix the page title with the application name.
  return title;
#endif

  if (title.empty() && deprecated_is_app())
    return override;
  // If there is no title and this is an app, fall back on the app name. This
  // ensures that the native window gets a title which is important for a11y,
  // for example the window selector uses the Aura window title.
  if (title.empty() &&
      (is_type_app() || is_type_app_popup() || is_type_devtools()) &&
      include_app_name) {
    return base::UTF8ToUTF16(
        app_controller_ ? app_controller_->GetAppShortName() : app_name());
  }

  // Include the app name in window titles for tabbed browser windows when
  // requested with |include_app_name|.
  return ((is_type_normal() || is_type_popup()) && include_app_name)
             ? l10n_util::GetStringFUTF16(IDS_BROWSER_WINDOW_TITLE_FORMAT,
                                          title)
             : title;
}

// static
base::string16 Browser::FormatTitleForDisplay(base::string16 title) {
  size_t current_index = 0;
  size_t match_index;
  while ((match_index = title.find(L'\n', current_index)) !=
         base::string16::npos) {
    title.replace(match_index, 1, base::string16());
    current_index = match_index;
  }

  return title;
}

///////////////////////////////////////////////////////////////////////////////
// Browser, OnBeforeUnload handling:

Browser::WarnBeforeClosingResult Browser::MaybeWarnBeforeClosing(
    Browser::WarnBeforeClosingCallback warn_callback) {
  // If the browser can close right away (there are no pending downloads we need
  // to prompt about) then there's no need to warn. In the future, we might need
  // to check other conditions as well.
  if (CanCloseWithInProgressDownloads())
    return WarnBeforeClosingResult::kOkToClose;

  DCHECK(!warn_before_closing_callback_)
      << "Tried to close window during close warning; dialog should be modal.";
  warn_before_closing_callback_ = std::move(warn_callback);
  return WarnBeforeClosingResult::kDoNotClose;
}

bool Browser::ShouldCloseWindow() {
  // If the user needs to see one or more warnings, hold off closing the
  // browser.
  const WarnBeforeClosingResult result = MaybeWarnBeforeClosing(base::BindOnce(
      &Browser::FinishWarnBeforeClosing, weak_factory_.GetWeakPtr()));
  if (result == WarnBeforeClosingResult::kDoNotClose)
    return false;

  return unload_controller_.ShouldCloseWindow();
}

bool Browser::TryToCloseWindow(
    bool skip_beforeunload,
    const base::Callback<void(bool)>& on_close_confirmed) {
  cancel_download_confirmation_state_ = RESPONSE_RECEIVED;
  return unload_controller_.TryToCloseWindow(skip_beforeunload,
                                             on_close_confirmed);
}

void Browser::ResetTryToCloseWindow() {
  cancel_download_confirmation_state_ = NOT_PROMPTED;
  unload_controller_.ResetTryToCloseWindow();
}

bool Browser::IsAttemptingToCloseBrowser() const {
  return unload_controller_.is_attempting_to_close_browser();
}

bool Browser::ShouldRunUnloadListenerBeforeClosing(
    content::WebContents* web_contents) {
  return unload_controller_.ShouldRunUnloadEventsHelper(web_contents);
}

bool Browser::RunUnloadListenerBeforeClosing(
    content::WebContents* web_contents) {
  return unload_controller_.RunUnloadEventsHelper(web_contents);
}

void Browser::SetIsInTabDragging(bool is_in_tab_dragging) {
  window_->TabDraggingStatusChanged(is_in_tab_dragging);
}

void Browser::OnWindowClosing() {
  if (!ShouldCloseWindow())
    return;

  // Application should shutdown on last window close if the user is explicitly
  // trying to quit, or if there is nothing keeping the browser alive (such as
  // AppController on the Mac, or BackgroundContentsService for background
  // pages).
  bool should_quit_if_last_browser =
      browser_shutdown::IsTryingToQuit() ||
      KeepAliveRegistry::GetInstance()->IsKeepingAliveOnlyByBrowserOrigin();

  if (should_quit_if_last_browser && ShouldStartShutdown()) {
    browser_shutdown::OnShutdownStarting(
        browser_shutdown::ShutdownType::kWindowClose);
  }

  // Don't use GetForProfileIfExisting here, we want to force creation of the
  // session service so that user can restore what was open.
  SessionService* session_service =
      SessionServiceFactory::GetForProfile(profile());
  if (session_service)
    session_service->WindowClosing(session_id());

  sessions::TabRestoreService* tab_restore_service =
      TabRestoreServiceFactory::GetForProfile(profile());

  bool notify_restore_service = is_type_normal() && tab_strip_model_->count();
#if defined(USE_AURA) || defined(OS_MACOSX)
  notify_restore_service |= is_type_app() || is_type_app_popup();
#endif

  if (tab_restore_service && notify_restore_service)
    tab_restore_service->BrowserClosing(live_tab_context());

  BrowserList::NotifyBrowserCloseStarted(this);

  tab_strip_model_->CloseAllTabs();
}

////////////////////////////////////////////////////////////////////////////////
// In-progress download termination handling:

Browser::DownloadCloseType Browser::OkToCloseWithInProgressDownloads(
    int* num_downloads_blocking) const {
  DCHECK(num_downloads_blocking);
  *num_downloads_blocking = 0;

  // If we're not running a full browser process with a profile manager
  // (testing), it's ok to close the browser.
  if (!g_browser_process->profile_manager())
    return DownloadCloseType::kOk;

  int total_download_count =
      DownloadCoreService::NonMaliciousDownloadCountAllProfiles();
  if (total_download_count == 0)
    return DownloadCloseType::kOk;  // No downloads; can definitely close.

  // Figure out how many windows are open total, and associated with this
  // profile, that are relevant for the ok-to-close decision.
  int profile_window_count = 0;
  int total_window_count = 0;
  for (auto* browser : *BrowserList::GetInstance()) {
    // Don't count this browser window or any other in the process of closing.
    // Window closing may be delayed, and windows that are in the process of
    // closing don't count against our totals.
    if (browser == this || browser->IsAttemptingToCloseBrowser())
      continue;

    if (browser->profile() == profile())
      profile_window_count++;
    total_window_count++;
  }

  // If there aren't any other windows, we're at browser shutdown,
  // which would cancel all current downloads.
  if (total_window_count == 0) {
    *num_downloads_blocking = total_download_count;
    return DownloadCloseType::kBrowserShutdown;
  }

  // If there aren't any other windows on our profile, and we're an incognito
  // profile, and there are downloads associated with that profile,
  // those downloads would be cancelled by our window (-> profile) close.
  DownloadCoreService* download_core_service =
      DownloadCoreServiceFactory::GetForBrowserContext(profile());
  if ((profile_window_count == 0) &&
      (download_core_service->NonMaliciousDownloadCount() > 0) &&
      profile()->IsOffTheRecord()) {
    *num_downloads_blocking =
        download_core_service->NonMaliciousDownloadCount();
    return profile()->IsGuestSession()
               ? DownloadCloseType::kLastWindowInGuestSession
               : DownloadCloseType::kLastWindowInIncognitoProfile;
  }

  // Those are the only conditions under which we will block shutdown.
  return DownloadCloseType::kOk;
}

////////////////////////////////////////////////////////////////////////////////
// Browser, Tab adding/showing functions:

void Browser::WindowFullscreenStateWillChange() {
  exclusive_access_manager_->fullscreen_controller()
      ->WindowFullscreenStateWillChange();
}

void Browser::WindowFullscreenStateChanged() {
  exclusive_access_manager_->fullscreen_controller()
      ->WindowFullscreenStateChanged();
  command_controller_->FullscreenStateChanged();
  UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_TOGGLE_FULLSCREEN);
}

void Browser::FullscreenTopUIStateChanged() {
  command_controller_->FullscreenStateChanged();
  UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_TOOLBAR_OPTION_CHANGE);
}

void Browser::OnFindBarVisibilityChanged() {
  window()->UpdatePageActionIcon(PageActionIconType::kFind);
  command_controller_->FindBarVisibilityChanged();
}

///////////////////////////////////////////////////////////////////////////////
// Browser, Assorted browser commands:

void Browser::ToggleFullscreenModeWithExtension(const GURL& extension_url) {
  exclusive_access_manager_->fullscreen_controller()
      ->ToggleBrowserFullscreenModeWithExtension(extension_url);
}

bool Browser::SupportsWindowFeature(WindowFeature feature) const {
  bool supports =
      SupportsWindowFeatureImpl(feature, /*check_can_support=*/false);
  // Supported features imply CanSupportWindowFeature.
  DCHECK(!supports || CanSupportWindowFeature(feature));
  return supports;
}

bool Browser::CanSupportWindowFeature(WindowFeature feature) const {
  return SupportsWindowFeatureImpl(feature, /*check_can_support=*/true);
}

void Browser::OpenFile() {
  // Ignore if there is already a select file dialog.
  if (select_file_dialog_)
    return;

  base::RecordAction(UserMetricsAction("OpenFile"));
  select_file_dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(
                tab_strip_model_->GetActiveWebContents()));

  if (!select_file_dialog_)
    return;

  const base::FilePath directory = profile_->last_selected_directory();
  // TODO(beng): figure out how to juggle this.
  gfx::NativeWindow parent_window = window_->GetNativeWindow();
  ui::SelectFileDialog::FileTypeInfo file_types;
  file_types.allowed_paths =
      ui::SelectFileDialog::FileTypeInfo::ANY_PATH_OR_URL;
  select_file_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_OPEN_FILE, base::string16(), directory,
      &file_types, 0, base::FilePath::StringType(), parent_window, NULL);
}

void Browser::UpdateDownloadShelfVisibility(bool visible) {
  if (GetStatusBubble())
    GetStatusBubble()->UpdateDownloadShelfVisibility(visible);
}

bool Browser::CanReloadContents(content::WebContents* web_contents) const {
  return chrome::CanReload(this);
}

bool Browser::CanSaveContents(content::WebContents* web_contents) const {
  return chrome::CanSavePage(this);
}

///////////////////////////////////////////////////////////////////////////////

void Browser::UpdateUIForNavigationInTab(WebContents* contents,
                                         ui::PageTransition transition,
                                         NavigateParams::WindowAction action,
                                         bool user_initiated) {
  tab_strip_model_->TabNavigating(contents, transition);

  bool contents_is_selected =
      contents == tab_strip_model_->GetActiveWebContents();
  if (user_initiated && contents_is_selected && window()->GetLocationBar()) {
    // Forcibly reset the location bar if the url is going to change in the
    // current tab, since otherwise it won't discard any ongoing user edits,
    // since it doesn't realize this is a user-initiated action.
    window()->GetLocationBar()->Revert();
  }

  if (GetStatusBubble())
    GetStatusBubble()->Hide();

  // Update the location bar. This is synchronous. We specifically don't
  // update the load state since the load hasn't started yet and updating it
  // will put it out of sync with the actual state like whether we're
  // displaying a favicon, which controls the throbber. If we updated it here,
  // the throbber will show the default favicon for a split second when
  // navigating away from the new tab page.
  ScheduleUIUpdate(contents, content::INVALIDATE_TYPE_URL);

  // Figure out if the navigating contents can take focus (potentially taking it
  // away from other, currently-focused UI element like the omnibox).
  // Specifically, user-initiated navigations can give focus to the tab;
  // renderer-initiated navigations usually don't, unless the NTP triggers them
  // (in which case they're treated similarly to a user-initiated navigation).
  //
  // TODO(lukasza): https://crbug.com/1034999: Try to avoid special-casing
  // kChromeUINewTabURL below and covering it via IsNTPOrRelatedURL instead.
  const GURL& current_url = contents->GetLastCommittedURL();
  bool contents_can_take_focus =
      user_initiated || current_url == GURL(chrome::kChromeUINewTabURL) ||
      search::IsNTPOrRelatedURL(
          current_url,
          Profile::FromBrowserContext(contents->GetBrowserContext()));

  if (contents_can_take_focus && contents_is_selected &&
      (window()->IsActive() || action == NavigateParams::SHOW_WINDOW)) {
    contents->SetInitialFocus();
  }
}

void Browser::RegisterKeepAlive() {
  keep_alive_ = std::make_unique<ScopedKeepAlive>(
      KeepAliveOrigin::BROWSER, KeepAliveRestartOption::DISABLED);
}
void Browser::UnregisterKeepAlive() {
  keep_alive_.reset();
}

///////////////////////////////////////////////////////////////////////////////
// Browser, PageNavigator implementation:

WebContents* Browser::OpenURL(const OpenURLParams& params) {
#if DCHECK_IS_ON()
  DCHECK(params.Valid());
#endif

  return OpenURLFromTab(NULL, params);
}

///////////////////////////////////////////////////////////////////////////////
// Browser, TabStripModelObserver implementation:

void Browser::OnTabStripModelChanged(TabStripModel* tab_strip_model,
                                     const TabStripModelChange& change,
                                     const TabStripSelectionChange& selection) {
  TRACE_EVENT0("ui", "Browser::OnTabStripModelChanged");
  switch (change.type()) {
    case TabStripModelChange::kInserted: {
      for (const auto& contents : change.GetInsert()->contents)
        OnTabInsertedAt(contents.contents, contents.index);
      break;
    }
    case TabStripModelChange::kRemoved: {
      const bool will_be_deleted = change.GetRemove()->will_be_deleted;
      for (const auto& contents : change.GetRemove()->contents) {
        if (will_be_deleted)
          OnTabClosing(contents.contents);
        OnTabDetached(contents.contents,
                      contents.contents == selection.old_contents);
      }
      break;
    }
    case TabStripModelChange::kMoved: {
      auto* move = change.GetMove();
      OnTabMoved(move->from_index, move->to_index);
      break;
    }
    case TabStripModelChange::kReplaced: {
      auto* replace = change.GetReplace();
      OnTabReplacedAt(replace->old_contents, replace->new_contents,
                      replace->index);
      break;
    }
    case TabStripModelChange::kSelectionOnly:
      break;
  }

  if (!selection.active_tab_changed())
    return;

  if (selection.old_contents)
    OnTabDeactivated(selection.old_contents);

  if (tab_strip_model_->empty())
    return;

  OnActiveTabChanged(selection.old_contents, selection.new_contents,
                     selection.new_model.active(), selection.reason);
}

void Browser::OnTabGroupChanged(const TabGroupChange& change) {
  if (change.type == TabGroupChange::kVisualsChanged) {
    SessionService* const session_service =
        SessionServiceFactory::GetForProfile(profile_);
    if (session_service) {
      const tab_groups::TabGroupVisualData* visual_data =
          tab_strip_model_->group_model()
              ->GetTabGroup(change.group)
              ->visual_data();
      session_service->SetTabGroupMetadata(session_id(), change.group,
                                           visual_data);
    }
  }
}

void Browser::TabPinnedStateChanged(TabStripModel* tab_strip_model,
                                    WebContents* contents,
                                    int index) {
  SessionService* session_service =
      SessionServiceFactory::GetForProfileIfExisting(profile());
  if (session_service) {
    sessions::SessionTabHelper* session_tab_helper =
        sessions::SessionTabHelper::FromWebContents(contents);
    session_service->SetPinnedState(session_id(),
                                    session_tab_helper->session_id(),
                                    tab_strip_model_->IsTabPinned(index));
  }
}

void Browser::TabGroupedStateChanged(
    base::Optional<tab_groups::TabGroupId> group,
    int index) {
  SessionService* const session_service =
      SessionServiceFactory::GetForProfile(profile_);
  if (session_service) {
    content::WebContents* const web_contents =
        tab_strip_model_->GetWebContentsAt(index);
    sessions::SessionTabHelper* const session_tab_helper =
        sessions::SessionTabHelper::FromWebContents(web_contents);
    session_service->SetTabGroup(session_id(), session_tab_helper->session_id(),
                                 std::move(group));
  }
}

void Browser::TabStripEmpty() {
  // Close the frame after we return to the message loop (not immediately,
  // otherwise it will destroy this object before the stack has a chance to
  // cleanly unwind.)
  // Note: This will be called several times if TabStripEmpty is called several
  //       times. This is because it does not close the window if tabs are
  //       still present.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&Browser::CloseFrame, weak_factory_.GetWeakPtr()));

  // Instant may have visible WebContents that need to be detached before the
  // window system closes.
  instant_controller_.reset();
}

void Browser::SetTopControlsShownRatio(content::WebContents* web_contents,
                                       float ratio) {
  window_->SetTopControlsShownRatio(web_contents, ratio);
}

int Browser::GetTopControlsHeight() {
  return window_->GetTopControlsHeight();
}

bool Browser::DoBrowserControlsShrinkRendererSize(
    const content::WebContents* contents) {
  return window_->DoBrowserControlsShrinkRendererSize(contents);
}

void Browser::SetTopControlsGestureScrollInProgress(bool in_progress) {
  window_->SetTopControlsGestureScrollInProgress(in_progress);
}

bool Browser::CanOverscrollContent() {
#if defined(USE_AURA)
  return !is_type_devtools() &&
         base::FeatureList::IsEnabled(features::kOverscrollHistoryNavigation);
#else
  return false;
#endif
}

bool Browser::ShouldPreserveAbortedURLs(WebContents* source) {
  // Allow failed URLs to stick around in the omnibox on the NTP, but not when
  // other pages have committed.
  Profile* profile = Profile::FromBrowserContext(source->GetBrowserContext());
  if (!profile || !source->GetController().GetLastCommittedEntry())
    return false;
  GURL committed_url(source->GetController().GetLastCommittedEntry()->GetURL());
  return search::IsNTPOrRelatedURL(committed_url, profile);
}

void Browser::SetFocusToLocationBar() {
  // Two differences between this and FocusLocationBar():
  // (1) This doesn't get recorded in user metrics, since it's called
  //     internally.
  // (2) This is called with |select_all| == false, because this is a renderer
  //     initiated focus (this method is a WebContentsDelegate override).
  //     We don't select-all for renderer initiated focuses, as the user may
  //     currently be typing something while the tab finishes loading. We don't
  //     want to clobber user input by selecting all while the user is typing.
  window_->SetFocusToLocationBar(false);
}

content::KeyboardEventProcessingResult Browser::PreHandleKeyboardEvent(
    content::WebContents* source,
    const NativeWebKeyboardEvent& event) {
  // Forward keyboard events to the manager for fullscreen / mouse lock. This
  // may consume the event (e.g., Esc exits fullscreen mode).
  // TODO(koz): Write a test for this http://crbug.com/100441.
  if (exclusive_access_manager_->HandleUserKeyEvent(event))
    return content::KeyboardEventProcessingResult::HANDLED;

  return window()->PreHandleKeyboardEvent(event);
}

bool Browser::HandleKeyboardEvent(content::WebContents* source,
                                  const NativeWebKeyboardEvent& event) {
  DevToolsWindow* devtools_window =
      DevToolsWindow::GetInstanceForInspectedWebContents(source);
  return (devtools_window && devtools_window->ForwardKeyboardEvent(event)) ||
         window()->HandleKeyboardEvent(event);
}

bool Browser::TabsNeedBeforeUnloadFired() {
  return unload_controller_.TabsNeedBeforeUnloadFired();
}

bool Browser::PreHandleGestureEvent(content::WebContents* source,
                                    const blink::WebGestureEvent& event) {
  // Disable pinch zooming in undocked dev tools window due to poor UX.
  if (app_name() == DevToolsWindow::kDevToolsApp)
    return blink::WebInputEvent::IsPinchGestureEventType(event.GetType());
  return false;
}

bool Browser::CanDragEnter(content::WebContents* source,
                           const content::DropData& data,
                           blink::WebDragOperationsMask operations_allowed) {
#if defined(OS_CHROMEOS)
  // Disallow drag-and-drop navigation for Settings windows which do not support
  // external navigation.
  if ((operations_allowed & blink::kWebDragOperationLink) &&
      chrome::SettingsWindowManager::GetInstance()->IsSettingsBrowser(this)) {
    return false;
  }
#endif
  return true;
}

blink::SecurityStyle Browser::GetSecurityStyle(
    WebContents* web_contents,
    content::SecurityStyleExplanations* security_style_explanations) {
  SecurityStateTabHelper* helper =
      SecurityStateTabHelper::FromWebContents(web_contents);
  DCHECK(helper);
  return security_state::GetSecurityStyle(helper->GetSecurityLevel(),
                                          *helper->GetVisibleSecurityState(),
                                          security_style_explanations);
}

std::unique_ptr<content::BluetoothChooser> Browser::RunBluetoothChooser(
    content::RenderFrameHost* frame,
    const content::BluetoothChooser::EventHandler& event_handler) {
  std::unique_ptr<BluetoothChooserController> bluetooth_chooser_controller(
      new BluetoothChooserController(frame, event_handler));

  std::unique_ptr<BluetoothChooserDesktop> bluetooth_chooser_desktop(
      new BluetoothChooserDesktop(bluetooth_chooser_controller.get()));

  std::unique_ptr<ChooserBubbleDelegate> chooser_bubble_delegate(
      new ChooserBubbleDelegate(frame,
                                std::move(bluetooth_chooser_controller)));

  Browser* browser = chrome::FindBrowserWithWebContents(
      WebContents::FromRenderFrameHost(frame));
  BubbleReference bubble_reference = browser->GetBubbleManager()->ShowBubble(
      std::move(chooser_bubble_delegate));
  bluetooth_chooser_desktop->set_bubble(std::move(bubble_reference));

  return std::move(bluetooth_chooser_desktop);
}

std::unique_ptr<content::BluetoothScanningPrompt>
Browser::ShowBluetoothScanningPrompt(
    content::RenderFrameHost* frame,
    const content::BluetoothScanningPrompt::EventHandler& event_handler) {
  auto bluetooth_scanning_prompt_controller =
      std::make_unique<BluetoothScanningPromptController>(frame, event_handler);

  auto bluetooth_scanning_prompt_desktop =
      std::make_unique<BluetoothScanningPromptDesktop>(
          bluetooth_scanning_prompt_controller.get());

  auto chooser_bubble_delegate = std::make_unique<ChooserBubbleDelegate>(
      frame, std::move(bluetooth_scanning_prompt_controller));

  BubbleReference bubble_reference =
      GetBubbleManager()->ShowBubble(std::move(chooser_bubble_delegate));
  bluetooth_scanning_prompt_desktop->set_bubble(std::move(bubble_reference));

  return std::move(bluetooth_scanning_prompt_desktop);
}

void Browser::CreateSmsPrompt(content::RenderFrameHost*,
                              const url::Origin&,
                              const std::string& one_time_code,
                              base::OnceClosure on_confirm,
                              base::OnceClosure on_cancel) {
  // TODO(crbug.com/1015645): implementation left pending deliberately.
  std::move(on_confirm).Run();
}

void Browser::PassiveInsecureContentFound(const GURL& resource_url) {
  // Note: this implementation is a mirror of
  // ContentSettingsObserver::passiveInsecureContentFound
  ReportInsecureContent(SslInsecureContentType::DISPLAY);
  FilteredReportInsecureContentDisplayed(resource_url);
}

bool Browser::ShouldAllowRunningInsecureContent(
    content::WebContents* web_contents,
    bool allowed_per_prefs,
    const url::Origin& origin,
    const GURL& resource_url) {
  // Note: this implementation is a mirror of
  // ContentSettingsObserver::allowRunningInsecureContent.
  FilteredReportInsecureContentRan(resource_url);

  if (allowed_per_prefs)
    return true;

  if (base::FeatureList::IsEnabled(features::kMixedContentSiteSetting)) {
    Profile* profile =
        Profile::FromBrowserContext(web_contents->GetBrowserContext());
    HostContentSettingsMap* content_settings =
        HostContentSettingsMapFactory::GetForProfile(profile);
    return content_settings->GetContentSetting(
               web_contents->GetLastCommittedURL(), GURL(),
               ContentSettingsType::MIXEDSCRIPT,
               std::string()) == CONTENT_SETTING_ALLOW;
  }
  MixedContentSettingsTabHelper* mixed_content_settings =
      MixedContentSettingsTabHelper::FromWebContents(web_contents);
  DCHECK(mixed_content_settings);
  bool allowed = mixed_content_settings->is_running_insecure_content_allowed();
  if (!allowed && !origin.host().empty()) {
    // Note: this is a browser-side-translation of the call to
    // DidBlockContentType from inside
    // ContentSettingsObserver::allowRunningInsecureContent.
    TabSpecificContentSettings* tab_settings =
        TabSpecificContentSettings::FromWebContents(web_contents);
    DCHECK(tab_settings);
    tab_settings->OnContentBlocked(ContentSettingsType::MIXEDSCRIPT);
  }
  return allowed;
}

void Browser::OnDidBlockNavigation(
    content::WebContents* web_contents,
    const GURL& blocked_url,
    const GURL& initiator_url,
    blink::mojom::NavigationBlockedReason reason) {
  if (reason ==
      blink::mojom::NavigationBlockedReason::kRedirectWithNoUserGesture) {
    if (auto* framebust_helper =
            FramebustBlockTabHelper::FromWebContents(web_contents)) {
      auto on_click = [](const GURL& url, size_t index, size_t total_elements) {
        UMA_HISTOGRAM_ENUMERATION(
            "WebCore.Framebust.ClickThroughPosition",
            GetListItemPositionFromDistance(index, total_elements));
      };
      framebust_helper->AddBlockedUrl(blocked_url, base::BindOnce(on_click));
    }
  }
  if (auto* trigger =
          safe_browsing::AdRedirectTrigger::FromWebContents(web_contents)) {
    trigger->OnDidBlockNavigation(initiator_url);
  }
}

content::PictureInPictureResult Browser::EnterPictureInPicture(
    content::WebContents* web_contents,
    const viz::SurfaceId& surface_id,
    const gfx::Size& natural_size) {
  return PictureInPictureWindowManager::GetInstance()->EnterPictureInPicture(
      web_contents, surface_id, natural_size);
}

void Browser::ExitPictureInPicture() {
  PictureInPictureWindowManager::GetInstance()->ExitPictureInPicture();
}

std::unique_ptr<content::WebContents> Browser::SwapWebContents(
    content::WebContents* old_contents,
    std::unique_ptr<content::WebContents> new_contents,
    bool did_start_load,
    bool did_finish_load) {
  // Copies the background color and contents of the old WebContents to a new
  // one that replaces it on the screen. This allows the new WebContents to
  // have something to show before having loaded any contents. As a result, we
  // avoid flashing white when navigating from a site with a dark background to
  // another site with a dark background.
  if (old_contents && new_contents) {
    RenderWidgetHostView* old_view = old_contents->GetMainFrame()->GetView();
    RenderWidgetHostView* new_view = new_contents->GetMainFrame()->GetView();
    if (old_view && new_view)
      new_view->TakeFallbackContentFrom(old_view);
  }

  DevToolsWindow* dev_tools_window =
      DevToolsWindow::GetInstanceForInspectedWebContents(old_contents);
  if (dev_tools_window)
    dev_tools_window->UpdateInspectedWebContents(new_contents.get());

  // TODO(crbug.com/836409): TabLoadTracker should not rely on being notified
  // directly about tab contents swaps.
  resource_coordinator::TabLoadTracker::Get()->SwapTabContents(
      old_contents, new_contents.get());

  // Clear the task manager tag. The TabStripModel will associate its own task
  // manager tag.
  task_manager::WebContentsTags::ClearTag(new_contents.get());

  int index = tab_strip_model_->GetIndexOfWebContents(old_contents);
  DCHECK_NE(TabStripModel::kNoTab, index);
  return tab_strip_model_->ReplaceWebContentsAt(index, std::move(new_contents));
}

bool Browser::ShouldShowStaleContentOnEviction(content::WebContents* source) {
#if defined(OS_CHROMEOS)
  return source == tab_strip_model_->GetActiveWebContents();
#else
  return false;
#endif  // defined(OS_CHROMEOS)
}

bool Browser::IsFrameLowPriority(
    const content::WebContents* web_contents,
    const content::RenderFrameHost* render_frame_host) {
  const auto* client =
      ChromeSubresourceFilterClient::FromWebContents(web_contents);
  return client &&
         client->GetThrottleManager()->IsFrameTaggedAsAd(render_frame_host);
}

bool Browser::IsMouseLocked() const {
  return exclusive_access_manager_->mouse_lock_controller()->IsMouseLocked();
}

void Browser::OnWindowDidShow() {
  if (window_has_shown_)
    return;
  window_has_shown_ = true;

  startup_metric_utils::RecordBrowserWindowDisplay(base::TimeTicks::Now());

  // Nothing to do for non-tabbed windows.
  if (!is_type_normal())
    return;

  // Show any pending global error bubble.
  GlobalErrorService* service =
      GlobalErrorServiceFactory::GetForProfile(profile());
  GlobalError* error = service->GetFirstGlobalErrorWithBubbleView();
  if (error)
    error->ShowBubbleView(this);
}

///////////////////////////////////////////////////////////////////////////////
// Browser, content::WebContentsDelegate implementation:

WebContents* Browser::OpenURLFromTab(WebContents* source,
                                     const OpenURLParams& params) {
#if DCHECK_IS_ON()
  DCHECK(params.Valid());
#endif

  if (is_type_devtools()) {
    DevToolsWindow* window = DevToolsWindow::AsDevToolsWindow(source);
    DCHECK(window);
    return window->OpenURLFromTab(source, params);
  }

  NavigateParams nav_params(this, params.url, params.transition);
  nav_params.FillNavigateParamsFromOpenURLParams(params);
  nav_params.source_contents = source;
  nav_params.tabstrip_add_types = TabStripModel::ADD_NONE;
  if (params.user_gesture)
    nav_params.window_action = NavigateParams::SHOW_WINDOW;
  bool is_popup = source && ConsiderForPopupBlocking(params.disposition);
  if (is_popup && MaybeBlockPopup(source, nullptr, &nav_params, &params,
                                  blink::mojom::WindowFeatures())) {
    return nullptr;
  }

  chrome::ConfigureTabGroupForNavigation(&nav_params);

  Navigate(&nav_params);

  if (is_popup && nav_params.navigated_or_inserted_contents) {
    auto* tracker = PopupTracker::CreateForWebContents(
        nav_params.navigated_or_inserted_contents, source);
    tracker->set_is_trusted(params.triggering_event_info !=
                            blink::TriggeringEventInfo::kFromUntrustedEvent);
  }

  return nav_params.navigated_or_inserted_contents;
}

void Browser::NavigationStateChanged(WebContents* source,
                                     content::InvalidateTypes changed_flags) {
  // Only update the UI when something visible has changed.
  if (changed_flags)
    ScheduleUIUpdate(source, changed_flags);

  // We can synchronously update commands since they will only change once per
  // navigation, so we don't have to worry about flickering. We do, however,
  // need to update the command state early on load to always present usable
  // actions in the face of slow-to-commit pages.
  if (changed_flags &
      (content::INVALIDATE_TYPE_URL | content::INVALIDATE_TYPE_LOAD |
       content::INVALIDATE_TYPE_TAB))
    command_controller_->TabStateChanged();

  if (app_controller_)
    app_controller_->UpdateCustomTabBarVisibility(true);
}

void Browser::VisibleSecurityStateChanged(WebContents* source) {
  // When the current tab's security state changes, we need to update the URL
  // bar to reflect the new state.
  DCHECK(source);
  if (tab_strip_model_->GetActiveWebContents() == source) {
    UpdateToolbar(false);

    if (app_controller_)
      app_controller_->UpdateCustomTabBarVisibility(true);
  }
}

void Browser::AddNewContents(WebContents* source,
                             std::unique_ptr<WebContents> new_contents,
                             WindowOpenDisposition disposition,
                             const gfx::Rect& initial_rect,
                             bool user_gesture,
                             bool* was_blocked) {
#if defined(OS_MACOSX)
  // On the Mac, the convention is to turn popups into new tabs when in
  // fullscreen mode. Only worry about user-initiated fullscreen as showing a
  // popup in HTML5 fullscreen would have kicked the page out of fullscreen.
  if (disposition == WindowOpenDisposition::NEW_POPUP &&
      exclusive_access_manager_->fullscreen_controller()
          ->IsFullscreenForBrowser()) {
    disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  }
#endif

  // At this point the |new_contents| is beyond the popup blocker, but we use
  // the same logic for determining if the popup tracker needs to be attached.
  if (source && ConsiderForPopupBlocking(disposition))
    PopupTracker::CreateForWebContents(new_contents.get(), source);

  chrome::AddWebContents(this, source, std::move(new_contents), disposition,
                         initial_rect, tmp_manifest());
}

void Browser::ActivateContents(WebContents* contents) {
  tab_strip_model_->ActivateTabAt(
      tab_strip_model_->GetIndexOfWebContents(contents));
  window_->Activate();
}

void Browser::LoadingStateChanged(WebContents* source,
                                  bool to_different_document) {
  ScheduleUIUpdate(source, content::INVALIDATE_TYPE_LOAD);
  UpdateWindowForLoadingStateChanged(source, to_different_document);
  std::string nwstatus;
  if (source->IsLoading()) {
    nwstatus = "loading";
    last_to_different_document_ = to_different_document;
    if (!to_different_document) //NWJS#5001
      return;
  } else {
    if (!last_to_different_document_)
      return;
    nwstatus = "loaded";
  }
  extensions::TabsWindowsAPI* tabs_window_api = extensions::TabsWindowsAPI::Get(profile_);
  if (!tabs_window_api)
    return;
  extensions::TabsEventRouter* tabs_event_router = tabs_window_api->tabs_event_router();
  if (!tabs_event_router)
    return;
  tabs_event_router->NWStatusUpdated(source, nwstatus);
}

void Browser::CloseContents(WebContents* source) {
  if (unload_controller_.CanCloseContents(source))
    chrome::CloseWebContents(this, source, true);
}

void Browser::SetContentsBounds(WebContents* source, const gfx::Rect& bounds) {
  if (is_type_normal())
    return;

  page_load_metrics::mojom::PageLoadFeatures features;
  features.features.push_back(blink::mojom::WebFeature::kMovedOrResizedPopup);
  if (creation_timer_.Elapsed() > base::TimeDelta::FromSeconds(2)) {
    // Additionally measure whether a popup was moved after creation, to
    // distinguish between popups that reposition themselves after load and
    // those which move popups continuously.
    features.features.push_back(
        blink::mojom::WebFeature::kMovedOrResizedPopup2sAfterCreation);
  }

  page_load_metrics::MetricsWebContentsObserver::RecordFeatureUsage(
      source->GetMainFrame(), features);
  window_->SetBounds(bounds);
}

void Browser::UpdateTargetURL(WebContents* source, const GURL& url) {
  if (!GetStatusBubble())
    return;

  if (source == tab_strip_model_->GetActiveWebContents())
    GetStatusBubble()->SetURL(url);
}

void Browser::ContentsMouseEvent(WebContents* source,
                                 bool motion,
                                 bool exited) {
  exclusive_access_manager_->OnUserInput();

  // Mouse motion events update the status bubble, if it exists.
  if (!GetStatusBubble() || (!motion && !exited))
    return;

  if (source == tab_strip_model_->GetActiveWebContents()) {
    GetStatusBubble()->MouseMoved(exited);
    if (exited)
      GetStatusBubble()->SetURL(GURL());
  }
}

void Browser::ContentsZoomChange(bool zoom_in) {
  //chrome::ExecuteCommand(this, zoom_in ? IDC_ZOOM_PLUS : IDC_ZOOM_MINUS);
}

bool Browser::TakeFocus(content::WebContents* source, bool reverse) {
  return false;
}

void Browser::BeforeUnloadFired(WebContents* web_contents,
                                bool proceed,
                                bool* proceed_to_fire_unload) {
  if (is_type_devtools() && DevToolsWindow::HandleBeforeUnload(
                                web_contents, proceed, proceed_to_fire_unload))
    return;

  *proceed_to_fire_unload =
      unload_controller_.BeforeUnloadFired(web_contents, proceed);
}

bool Browser::ShouldFocusLocationBarByDefault(WebContents* source) {
  // Navigations in background tabs shouldn't change the focus state of the
  // omnibox, since it's associated with the foreground tab.
  if (source != tab_strip_model_->GetActiveWebContents())
    return false;

  // This should be based on the pending entry if there is one, so that
  // back/forward navigations to the NTP are handled.  The visible entry can't
  // be used here, since back/forward navigations are not treated as  visible
  // entries to avoid URL spoofs.
  content::NavigationEntry* entry =
      source->GetController().GetPendingEntry()
          ? source->GetController().GetPendingEntry()
          : source->GetController().GetLastCommittedEntry();
  if (entry) {
    const GURL& url = entry->GetURL();
    const GURL& virtual_url = entry->GetVirtualURL();

    if (virtual_url.SchemeIs(content::kViewSourceScheme))
      return false;

    if ((url.SchemeIs(content::kChromeUIScheme) &&
         url.host_piece() == chrome::kChromeUINewTabHost) ||
        (virtual_url.SchemeIs(content::kChromeUIScheme) &&
         virtual_url.host_piece() == chrome::kChromeUINewTabHost)) {
      return true;
    }
  }

  return search::NavEntryIsInstantNTP(source, entry);
}

void Browser::ShowRepostFormWarningDialog(WebContents* source) {
  TabModalConfirmDialog::Create(
      std::make_unique<RepostFormWarningController>(source), source);
}

bool Browser::IsWebContentsCreationOverridden(
    content::SiteInstance* source_site_instance,
    content::mojom::WindowContainerType window_container_type,
    const GURL& opener_url,
    const std::string& frame_name,
    const GURL& target_url) {
  return window_container_type ==
             content::mojom::WindowContainerType::BACKGROUND &&
         ShouldCreateBackgroundContents(source_site_instance, opener_url,
                                        frame_name);
}

WebContents* Browser::CreateCustomWebContents(
    content::RenderFrameHost* opener,
    content::SiteInstance* source_site_instance,
    bool is_new_browsing_instance,
    const GURL& opener_url,
    const std::string& frame_name,
    const GURL& target_url,
    const std::string& partition_id,
    content::SessionStorageNamespace* session_storage_namespace) {
  BackgroundContents* background_contents = CreateBackgroundContents(
      source_site_instance, opener, opener_url, is_new_browsing_instance,
      frame_name, target_url, partition_id, session_storage_namespace);
  if (background_contents) {
    return background_contents->web_contents();
  }
  return nullptr;
}

void Browser::WebContentsCreated(WebContents* source_contents,
                                 int opener_render_process_id,
                                 int opener_render_frame_id,
                                 const std::string& frame_name,
                                 const GURL& target_url,
                                 WebContents* new_contents, const base::string16& nw_window_manifest) {
  // Adopt the WebContents now, so all observers are in place, as the network
  // requests for its initial navigation will start immediately. The WebContents
  // will later be inserted into this browser using Browser::Navigate via
  // AddNewContents.
  TabHelpers::AttachTabHelpers(new_contents);
  extensions::AppWindow::CreateParams params;
  std::string js_doc_start, js_doc_end;
  nw::CalcNewWinParams(new_contents, &params, &js_doc_start, &js_doc_end);
  nw::SetCurrentNewWinManifest(base::string16());
  new_contents->GetMutableRendererPrefs()->
    nw_inject_js_doc_start = js_doc_start;
  new_contents->GetMutableRendererPrefs()->
    nw_inject_js_doc_end = js_doc_end;
  new_contents->SyncRendererPrefs();
  // Make the tab show up in the task manager.
  task_manager::WebContentsTags::CreateForTabContents(new_contents);
}

void Browser::PortalWebContentsCreated(WebContents* portal_web_contents) {
  TabHelpers::AttachTabHelpers(portal_web_contents);

  // Make the portal show up in the task manager.
  task_manager::WebContentsTags::CreateForPortal(portal_web_contents);
}

void Browser::WebContentsBecamePortal(WebContents* portal_web_contents) {
  // Make the contents show up as a portal in the task manager.
  task_manager::WebContentsTags::ClearTag(portal_web_contents);
  task_manager::WebContentsTags::CreateForPortal(portal_web_contents);
}

void Browser::RendererUnresponsive(
    WebContents* source,
    content::RenderWidgetHost* render_widget_host,
    base::RepeatingClosure hang_monitor_restarter) {
#if 0
  TabDialogs::FromWebContents(source)->ShowHungRendererDialog(
      render_widget_host, std::move(hang_monitor_restarter));
#endif
}

void Browser::RendererResponsive(
    WebContents* source,
    content::RenderWidgetHost* render_widget_host) {
  TabDialogs::FromWebContents(source)->HideHungRendererDialog(
      render_widget_host);
}

void Browser::DidNavigateMainFramePostCommit(WebContents* web_contents) {
  if (web_contents == tab_strip_model_->GetActiveWebContents())
    UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_TAB_STATE);
  OnDidFinishFirstNavigation();
}

content::JavaScriptDialogManager* Browser::GetJavaScriptDialogManager(
    WebContents* source) {
  return JavaScriptDialogTabHelper::FromWebContents(source);
}

bool Browser::GuestSaveFrame(content::WebContents* guest_web_contents) {
  auto* guest_view =
      extensions::MimeHandlerViewGuest::FromWebContents(guest_web_contents);
  return guest_view && guest_view->PluginDoSave();
}

content::ColorChooser* Browser::OpenColorChooser(
    WebContents* web_contents,
    SkColor initial_color,
    const std::vector<blink::mojom::ColorSuggestionPtr>& suggestions) {
  return chrome::ShowColorChooser(web_contents, initial_color);
}

void Browser::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    std::unique_ptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  FileSelectHelper::RunFileChooser(render_frame_host, std::move(listener),
                                   params);
}

void Browser::EnumerateDirectory(
    WebContents* web_contents,
    std::unique_ptr<content::FileSelectListener> listener,
    const base::FilePath& path) {
  FileSelectHelper::EnumerateDirectory(web_contents, std::move(listener), path);
}

bool Browser::EmbedsFullscreenWidget() {
  return true;
}

void Browser::EnterFullscreenModeForTab(
    WebContents* web_contents,
    const GURL& origin,
    const blink::mojom::FullscreenOptions& options) {
  exclusive_access_manager_->fullscreen_controller()->EnterFullscreenModeForTab(
      web_contents, origin, options.display_id);
}

void Browser::ExitFullscreenModeForTab(WebContents* web_contents) {
  exclusive_access_manager_->fullscreen_controller()->ExitFullscreenModeForTab(
      web_contents);
}

bool Browser::IsFullscreenForTabOrPending(const WebContents* web_contents) {
  return exclusive_access_manager_->fullscreen_controller()
      ->IsFullscreenForTabOrPending(web_contents);
}

blink::mojom::DisplayMode Browser::GetDisplayMode(
    const WebContents* web_contents) {
  if (window_->IsFullscreen())
    return blink::mojom::DisplayMode::kFullscreen;

  if (is_type_app() || is_type_devtools() || is_type_app_popup())
    return blink::mojom::DisplayMode::kStandalone;

  return blink::mojom::DisplayMode::kBrowser;
}

void Browser::RegisterProtocolHandler(WebContents* web_contents,
                                      const std::string& protocol,
                                      const GURL& url,
                                      bool user_gesture) {
  content::BrowserContext* context = web_contents->GetBrowserContext();
  if (context->IsOffTheRecord())
    return;

  // Permission request UI cannot currently be rendered binocularly in VR mode,
  // so we suppress the UI. crbug.com/736568
  if (vr::VrTabHelper::IsInVr(web_contents))
    return;

  ProtocolHandler handler =
      ProtocolHandler::CreateProtocolHandler(protocol, url);

  if (!handler.IsValid())
    return;

  ProtocolHandlerRegistry* registry =
      ProtocolHandlerRegistryFactory::GetForBrowserContext(context);
  if (registry->SilentlyHandleRegisterHandlerRequest(handler))
    return;

  TabSpecificContentSettings* tab_content_settings =
      TabSpecificContentSettings::FromWebContents(web_contents);
  if (!user_gesture && window_) {
    tab_content_settings->set_pending_protocol_handler(handler);
    tab_content_settings->set_previous_protocol_handler(
        registry->GetHandlerFor(handler.protocol()));
    window_->GetLocationBar()->UpdateContentSettingsIcons();
    return;
  }

  // Make sure content-setting icon is turned off in case the page does
  // ungestured and gestured RPH calls.
  if (window_) {
    tab_content_settings->ClearPendingProtocolHandler();
    window_->GetLocationBar()->UpdateContentSettingsIcons();
  }

  PermissionRequestManager* permission_request_manager =
      PermissionRequestManager::FromWebContents(web_contents);
  if (permission_request_manager) {
    // At this point, there will be UI presented, and running a dialog causes an
    // exit to webpage-initiated fullscreen. http://crbug.com/728276
    web_contents->ForSecurityDropFullscreen();

    permission_request_manager->AddRequest(
        new RegisterProtocolHandlerPermissionRequest(registry, handler, url,
                                                     user_gesture));
  }
}

void Browser::UnregisterProtocolHandler(WebContents* web_contents,
                                        const std::string& protocol,
                                        const GURL& url,
                                        bool user_gesture) {
  // user_gesture will be used in case we decide to have confirmation bubble
  // for user while un-registering the handler.
  content::BrowserContext* context = web_contents->GetBrowserContext();
  if (context->IsOffTheRecord())
    return;

  ProtocolHandler handler =
      ProtocolHandler::CreateProtocolHandler(protocol, url);

  ProtocolHandlerRegistry* registry =
      ProtocolHandlerRegistryFactory::GetForBrowserContext(context);
  registry->RemoveHandler(handler);
}

void Browser::FindReply(WebContents* web_contents,
                        int request_id,
                        int number_of_matches,
                        const gfx::Rect& selection_rect,
                        int active_match_ordinal,
                        bool final_update) {
  find_in_page::FindTabHelper* find_tab_helper =
      find_in_page::FindTabHelper::FromWebContents(web_contents);
  if (!find_tab_helper)
    return;

  find_tab_helper->HandleFindReply(request_id, number_of_matches,
                                   selection_rect, active_match_ordinal,
                                   final_update);
}

void Browser::RequestToLockMouse(WebContents* web_contents,
                                 bool user_gesture,
                                 bool last_unlocked_by_target) {
  exclusive_access_manager_->mouse_lock_controller()->RequestToLockMouse(
      web_contents, user_gesture, last_unlocked_by_target);
}

void Browser::LostMouseLock() {
  exclusive_access_manager_->mouse_lock_controller()->LostMouseLock();
}

void Browser::RequestKeyboardLock(WebContents* web_contents,
                                  bool esc_key_locked) {
  exclusive_access_manager_->keyboard_lock_controller()->RequestKeyboardLock(
      web_contents, esc_key_locked);
}

void Browser::CancelKeyboardLockRequest(WebContents* web_contents) {
  exclusive_access_manager_->keyboard_lock_controller()
      ->CancelKeyboardLockRequest(web_contents);
}

void Browser::RequestMediaAccessPermission(
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  const extensions::Extension* extension =
      GetExtensionForOrigin(profile_, request.security_origin);
  MediaCaptureDevicesDispatcher::GetInstance()->ProcessMediaAccessRequest(
      web_contents, request, std::move(callback), extension);
}

bool Browser::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const GURL& security_origin,
    blink::mojom::MediaStreamType type) {
  Profile* profile = Profile::FromBrowserContext(
      content::WebContents::FromRenderFrameHost(render_frame_host)
          ->GetBrowserContext());
  const extensions::Extension* extension =
      GetExtensionForOrigin(profile, security_origin);
  return MediaCaptureDevicesDispatcher::GetInstance()
      ->CheckMediaAccessPermission(render_frame_host, security_origin, type,
                                   extension);
}

std::string Browser::GetDefaultMediaDeviceID(
    content::WebContents* web_contents,
    blink::mojom::MediaStreamType type) {
  Profile* profile =
      Profile::FromBrowserContext(web_contents->GetBrowserContext());
  return MediaCaptureDevicesDispatcher::GetInstance()
      ->GetDefaultDeviceIDForProfile(profile, type);
}

void Browser::RequestPpapiBrokerPermission(
    WebContents* web_contents,
    const GURL& url,
    const base::FilePath& plugin_path,
    base::OnceCallback<void(bool)> callback) {
#if BUILDFLAG(ENABLE_PLUGINS)
  Profile* profile =
      Profile::FromBrowserContext(web_contents->GetBrowserContext());
  // TODO(wad): Add ephemeral device ID support for broker in guest mode.
  if (profile->IsGuestSession()) {
    std::move(callback).Run(false);
    return;
  }

  TabSpecificContentSettings* tab_content_settings =
      TabSpecificContentSettings::FromWebContents(web_contents);

  HostContentSettingsMap* content_settings =
      HostContentSettingsMapFactory::GetForProfile(profile);
  ContentSetting setting = content_settings->GetContentSetting(
      url, url, ContentSettingsType::PPAPI_BROKER, std::string());

  if (setting == CONTENT_SETTING_ASK) {
    base::RecordAction(base::UserMetricsAction("PPAPI.BrokerInfobarDisplayed"));

    content::PluginService* plugin_service =
        content::PluginService::GetInstance();
    content::WebPluginInfo plugin;
    bool success = plugin_service->GetPluginInfoByPath(plugin_path, &plugin);
    DCHECK(success);
    std::unique_ptr<PluginMetadata> plugin_metadata(
        PluginFinder::GetInstance()->GetPluginMetadata(plugin));

    PepperBrokerInfoBarDelegate::Create(
        InfoBarService::FromWebContents(web_contents), url,
        plugin_metadata->name(), content_settings, tab_content_settings,
        std::move(callback));
    return;
  }

  bool allowed = (setting == CONTENT_SETTING_ALLOW);
  base::RecordAction(allowed
                         ? base::UserMetricsAction("PPAPI.BrokerSettingAllow")
                         : base::UserMetricsAction("PPAPI.BrokerSettingDeny"));
  tab_content_settings->SetPepperBrokerAllowed(allowed);
  std::move(callback).Run(allowed);
#endif
}

#if BUILDFLAG(ENABLE_PRINTING)
void Browser::PrintCrossProcessSubframe(
    content::WebContents* web_contents,
    const gfx::Rect& rect,
    int document_cookie,
    content::RenderFrameHost* subframe_host) const {
  auto* client = printing::PrintCompositeClient::FromWebContents(web_contents);
  if (client)
    client->PrintCrossProcessSubframe(rect, document_cookie, subframe_host);
}
#endif

///////////////////////////////////////////////////////////////////////////////
// Browser, web_modal::WebContentsModalDialogManagerDelegate implementation:

void Browser::SetWebContentsBlocked(content::WebContents* web_contents,
                                    bool blocked) {
  int index = tab_strip_model_->GetIndexOfWebContents(web_contents);
  if (index == TabStripModel::kNoTab) {
    // Removal of tabs from the TabStripModel can cause observer callbacks to
    // invoke this method. The WebContents may no longer exist in the
    // TabStripModel.
    return;
  }

  // For security, if the WebContents is in fullscreen, have it drop fullscreen.
  // This gives the user the context they need in order to make informed
  // decisions.
  if (web_contents->IsFullscreenForCurrentTab()) {
    // FullscreenWithinTab mode exception: In this case, the browser window is
    // in its normal layout and not fullscreen (tab content rendering is in a
    // "simulated fullscreen" state for the benefit of screen capture). Thus,
    // the user has the same context as they would in any non-fullscreen
    // scenario. See "FullscreenWithinTab note" in FullscreenController's
    // class-level comments for further details.
    if (!exclusive_access_manager_->fullscreen_controller()
             ->IsFullscreenWithinTab(web_contents)) {
      web_contents->ExitFullscreen(true);
    }
  }

  tab_strip_model_->SetTabBlocked(index, blocked);

  bool browser_active = BrowserList::GetInstance()->GetLastActive() == this;
  bool contents_is_active =
      tab_strip_model_->GetActiveWebContents() == web_contents;
  // If the WebContents is foremost (the active tab in the front-most browser)
  // and is being unblocked, focus it to make sure that input works again.
  if (!blocked && contents_is_active && browser_active)
    web_contents->Focus();
}

web_modal::WebContentsModalDialogHost*
Browser::GetWebContentsModalDialogHost() {
  return window_->GetWebContentsModalDialogHost();
}

///////////////////////////////////////////////////////////////////////////////
// Browser, BookmarkTabHelperObserver implementation:

void Browser::URLStarredChanged(content::WebContents* web_contents,
                                bool starred) {
  if (web_contents == tab_strip_model_->GetActiveWebContents())
    window_->SetStarredState(starred);
}

///////////////////////////////////////////////////////////////////////////////
// Browser, ZoomObserver implementation:

void Browser::OnZoomChanged(
    const zoom::ZoomController::ZoomChangedEventData& data) {
  if (data.web_contents == tab_strip_model_->GetActiveWebContents()) {
    window_->ZoomChangedForActiveTab(data.can_show_bubble);
    // Change the zoom commands state based on the zoom state
    command_controller_->ZoomStateChanged();
  }
}

///////////////////////////////////////////////////////////////////////////////
// Browser, ui::SelectFileDialog::Listener implementation:

void Browser::FileSelected(const base::FilePath& path,
                           int index,
                           void* params) {
  FileSelectedWithExtraInfo(ui::SelectedFileInfo(path, path), index, params);
}

void Browser::FileSelectedWithExtraInfo(const ui::SelectedFileInfo& file_info,
                                        int index,
                                        void* params) {
  // Transfer the ownership of select file dialog so that the ref count is
  // released after the function returns. This is needed because the passed-in
  // data such as |file_info| and |params| could be owned by the dialog.
  scoped_refptr<ui::SelectFileDialog> dialog = std::move(select_file_dialog_);

  profile_->set_last_selected_directory(file_info.file_path.DirName());

  GURL url = std::move(file_info.url)
                 .value_or(net::FilePathToFileURL(file_info.local_path));

  if (url.is_empty())
    return;

  OpenURL(OpenURLParams(url, Referrer(), WindowOpenDisposition::CURRENT_TAB,
                        ui::PAGE_TRANSITION_TYPED, false));
}

void Browser::FileSelectionCanceled(void* params) {
  select_file_dialog_.reset();
}

///////////////////////////////////////////////////////////////////////////////
// Browser, content::NotificationObserver implementation:

void Browser::Observe(int type,
                      const content::NotificationSource& source,
                      const content::NotificationDetails& details) {
  DCHECK_EQ(chrome::NOTIFICATION_BROWSER_THEME_CHANGED, type);
  window()->UserChangedTheme(BrowserThemeChangeType::kBrowserTheme);
}

///////////////////////////////////////////////////////////////////////////////
// Browser, translate::ContentTranslateDriver::Observer implementation:

void Browser::OnIsPageTranslatedChanged(content::WebContents* source) {
  DCHECK(source);
#if 0
  if (tab_strip_model_->GetActiveWebContents() == source) {
    window_->SetTranslateIconToggled(
        ChromeTranslateClient::FromWebContents(source)
            ->GetLanguageState()
            .IsPageTranslated());
  }
#endif
}

void Browser::OnTranslateEnabledChanged(content::WebContents* source) {
  DCHECK(source);
  if (tab_strip_model_->GetActiveWebContents() == source)
    UpdateToolbar(false);
}

///////////////////////////////////////////////////////////////////////////////
// Browser, Command and state updating (private):

void Browser::OnTabInsertedAt(WebContents* contents, int index) {
  SetAsDelegate(contents, true);

  sessions::SessionTabHelper::FromWebContents(contents)->SetWindowID(
      session_id());

  SyncHistoryWithTabs(index);

  // Make sure the loading state is updated correctly, otherwise the throbber
  // won't start if the page is loading. Note that we don't want to
  // ScheduleUIUpdate() because the tab may not have been inserted in the UI
  // yet if this function is called before TabStripModel::TabInsertedAt().
  UpdateWindowForLoadingStateChanged(contents, true);

  interstitial_observers_.push_back(new InterstitialObserver(this, contents));

  SessionService* session_service =
      SessionServiceFactory::GetForProfile(profile_);
  if (session_service) {
    session_service->TabInserted(contents);
    int new_active_index = tab_strip_model_->active_index();
    if (index < new_active_index)
      session_service->SetSelectedTabInWindow(session_id(), new_active_index);
  }
}

void Browser::OnTabClosing(WebContents* contents) {
  // When this function is called |contents| has been removed from the
  // TabStripModel. Some of the following code may trigger calling to the
  // WebContentsDelegate, which is |this|, which may try to look for the
  // WebContents in the TabStripModel, and fail because the WebContents has
  // been removed. To avoid these problems the delegate is reset now.
  SetAsDelegate(contents, false);

  // Typically, ModalDialogs are closed when the WebContents is destroyed.
  // However, when the tab is being closed, we must first close the dialogs [to
  // give them an opportunity to clean up after themselves] while the state
  // associated with their tab is still valid.
  WebContentsModalDialogManager::FromWebContents(contents)->CloseAllDialogs();

  // Page load metrics need to be informed that the WebContents will soon be
  // destroyed, so that upcoming visibility changes can be ignored.
  page_load_metrics::MetricsWebContentsObserver* metrics_observer =
      page_load_metrics::MetricsWebContentsObserver::FromWebContents(contents);
  metrics_observer->WebContentsWillSoonBeDestroyed();

  exclusive_access_manager_->OnTabClosing(contents);
  SessionService* session_service =
      SessionServiceFactory::GetForProfile(profile_);
  if (session_service)
    session_service->TabClosing(contents);

  SearchTabHelper::FromWebContents(contents)->OnTabClosing();
}

void Browser::OnTabDetached(WebContents* contents, bool was_active) {
  if (!tab_strip_model_->closing_all()) {
    SessionService* session_service =
        SessionServiceFactory::GetForProfileIfExisting(profile_);
    if (session_service) {
      session_service->SetSelectedTabInWindow(session_id(),
                                              tab_strip_model_->active_index());
    }
  }

  TabDetachedAtImpl(contents, was_active, DETACH_TYPE_DETACH);

  window_->OnTabDetached(contents, was_active);
}

void Browser::OnTabDeactivated(WebContents* contents) {
  exclusive_access_manager_->OnTabDeactivated(contents);
  if (SearchTabHelper::FromWebContents(contents))
  SearchTabHelper::FromWebContents(contents)->OnTabDeactivated();

  // Save what the user's currently typing, so it can be restored when we
  // switch back to this tab.
  window_->GetLocationBar()->SaveStateToContents(contents);
}

void Browser::OnActiveTabChanged(WebContents* old_contents,
                                 WebContents* new_contents,
                                 int index,
                                 int reason) {
  TRACE_EVENT0("ui", "Browser::OnActiveTabChanged");
// Mac correctly sets the initial background color of new tabs to the theme
// background color, so it does not need this block of code. Aura should
// implement this as well.
// https://crbug.com/719230
#if !defined(OS_MACOSX)
  // Copies the background color from an old WebContents to a new one that
  // replaces it on the screen. This allows the new WebContents to use the
  // old one's background color as the starting background color, before having
  // loaded any contents. As a result, we avoid flashing white when moving to
  // a new tab. (There is also code in RenderFrameHostManager to do something
  // similar for intra-tab navigations.)
  if (old_contents && new_contents) {
    // While GetMainFrame() is guaranteed to return non-null, GetView() is not,
    // e.g. between WebContents creation and creation of the
    // RenderWidgetHostView.
    RenderWidgetHostView* old_view = old_contents->GetMainFrame()->GetView();
    RenderWidgetHostView* new_view = new_contents->GetMainFrame()->GetView();
    if (old_view && new_view && old_view->GetBackgroundColor())
      new_view->SetBackgroundColor(*old_view->GetBackgroundColor());
  }
#endif

  base::RecordAction(UserMetricsAction("ActiveTabChanged"));

  // Update the bookmark state, since the BrowserWindow may query it during
  // OnActiveTabChanged() below.
  UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_TAB_SWITCH);

  // Let the BrowserWindow do its handling.  On e.g. views this changes the
  // focused object, which should happen before we update the toolbar below,
  // since the omnibox expects the correct element to already be focused when it
  // is updated.
  window_->OnActiveTabChanged(old_contents, new_contents, index, reason);

  exclusive_access_manager_->OnTabDetachedFromView(old_contents);

  // If we have any update pending, do it now.
  if (chrome_updater_factory_.HasWeakPtrs() && old_contents)
    ProcessPendingUIUpdates();

  // Propagate the profile to the location bar.
  UpdateToolbar((reason & CHANGE_REASON_REPLACED) == 0);

  // Update reload/stop state.
  command_controller_->LoadingStateChanged(new_contents->IsLoading(), true);

  // Update commands to reflect current state.
  command_controller_->TabStateChanged();

  // Reset the status bubble.
  StatusBubble* status_bubble = GetStatusBubble();
  if (status_bubble) {
    status_bubble->Hide();

    // Show the loading state (if any).
    status_bubble->SetStatus(
        CoreTabHelper::FromWebContents(tab_strip_model_->GetActiveWebContents())
            ->GetStatusText());
  }

  if (HasFindBarController()) {
    find_bar_controller_->ChangeWebContents(new_contents);
    find_bar_controller_->find_bar()->MoveWindowIfNecessary();
  }

  // Update sessions (selected tab index and last active time). Don't force
  // creation of sessions. If sessions doesn't exist, the change will be picked
  // up by sessions when created.
  SessionService* session_service =
      SessionServiceFactory::GetForProfileIfExisting(profile_);
  if (session_service && !tab_strip_model_->closing_all()) {
    session_service->SetSelectedTabInWindow(session_id(),
                                            tab_strip_model_->active_index());
    sessions::SessionTabHelper* session_tab_helper =
        sessions::SessionTabHelper::FromWebContents(new_contents);
    session_service->SetLastActiveTime(
        session_id(), session_tab_helper->session_id(), base::TimeTicks::Now());
  }

  //SearchTabHelper::FromWebContents(new_contents)->OnTabActivated();
}

void Browser::OnTabMoved(int from_index, int to_index) {
  DCHECK(from_index >= 0 && to_index >= 0);
  // Notify the history service.
  SyncHistoryWithTabs(std::min(from_index, to_index));
}

void Browser::OnTabReplacedAt(WebContents* old_contents,
                              WebContents* new_contents,
                              int index) {
  bool was_active = index == tab_strip_model_->active_index();
  TabDetachedAtImpl(old_contents, was_active, DETACH_TYPE_REPLACE);
  exclusive_access_manager_->OnTabClosing(old_contents);
  SessionService* session_service =
      SessionServiceFactory::GetForProfile(profile_);
  if (session_service)
    session_service->TabClosing(old_contents);
  OnTabInsertedAt(new_contents, index);

  if (!new_contents->GetController().IsInitialBlankNavigation()) {
    // Send out notification so that observers are updated appropriately.
    int entry_count = new_contents->GetController().GetEntryCount();
    new_contents->GetController().NotifyEntryChanged(
        new_contents->GetController().GetEntryAtIndex(entry_count - 1));
  }

  if (session_service) {
    // The new_contents may end up with a different navigation stack. Force
    // the session service to update itself.
    session_service->TabRestored(new_contents,
                                 tab_strip_model_->IsTabPinned(index));
  }
}

void Browser::OnDevToolsAvailabilityChanged() {
  using DTPH = policy::DeveloperToolsPolicyHandler;
  // We close all windows as a safety measure, even for
  // kDisallowedForForceInstalledExtensions.
  if (DTPH::GetDevToolsAvailability(profile_->GetPrefs()) !=
      DTPH::Availability::kAllowed) {
    content::DevToolsAgentHost::DetachAllClients();
  }
}

///////////////////////////////////////////////////////////////////////////////
// Browser, UI update coalescing and handling (private):

void Browser::UpdateToolbar(bool should_restore_state) {
  TRACE_EVENT0("ui", "Browser::UpdateToolbar");
  window_->UpdateToolbar(
      should_restore_state ? tab_strip_model_->GetActiveWebContents() : NULL);
}

void Browser::ScheduleUIUpdate(WebContents* source, unsigned changed_flags) {
  DCHECK(source);
  // WebContents may in some rare cases send updates after they've been detached
  // from the tabstrip but before they are deleted, causing a potential crash if
  // we proceed. For now bail out.
  // TODO(crbug.com/1007379) Figure out a safe way to detach browser delegate
  // from WebContents when it's removed so this doesn't happen - then put a
  // DCHECK back here.
  if (tab_strip_model_->GetIndexOfWebContents(source) == TabStripModel::kNoTab)
    return;

  // Do some synchronous updates.
  if (changed_flags & content::INVALIDATE_TYPE_URL) {
    if (source == tab_strip_model_->GetActiveWebContents()) {
      // Only update the URL for the current tab. Note that we do not update
      // the navigation commands since those would have already been updated
      // synchronously by NavigationStateChanged.
      UpdateToolbar(false);
    } else {
      // Clear the saved tab state for the tab that navigated, so that we don't
      // restore any user text after the old URL has been invalidated (e.g.,
      // after a new navigation commits in that tab while unfocused).
      window_->ResetToolbarTabState(source);
    }
    changed_flags &= ~content::INVALIDATE_TYPE_URL;
  }

  if (changed_flags & content::INVALIDATE_TYPE_LOAD) {
    // Update the loading state synchronously. This is so the throbber will
    // immediately start/stop, which gives a more snappy feel. We want to do
    // this for any tab so they start & stop quickly.
    tab_strip_model_->UpdateWebContentsStateAt(
        tab_strip_model_->GetIndexOfWebContents(source),
        TabChangeType::kLoadingOnly);
    // The status bubble needs to be updated during INVALIDATE_TYPE_LOAD too,
    // but we do that asynchronously by not stripping INVALIDATE_TYPE_LOAD from
    // changed_flags.
  }

  // If the only updates were synchronously handled above, we're done.
  if (changed_flags == 0)
    return;

  // Save the dirty bits.
  scheduled_updates_[source] |= changed_flags;

  if (!chrome_updater_factory_.HasWeakPtrs()) {
    // No task currently scheduled, start another.
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Browser::ProcessPendingUIUpdates,
                       chrome_updater_factory_.GetWeakPtr()),
        kUIUpdateCoalescingTime);
  }
}

void Browser::ProcessPendingUIUpdates() {
#ifndef NDEBUG
  // Validate that all tabs we have pending updates for exist. This is scary
  // because the pending list must be kept in sync with any detached or
  // deleted tabs.
  for (UpdateMap::const_iterator i = scheduled_updates_.begin();
       i != scheduled_updates_.end(); ++i) {
    bool found = false;
    for (int tab = 0; tab < tab_strip_model_->count(); tab++) {
      if (tab_strip_model_->GetWebContentsAt(tab) == i->first) {
        found = true;
        break;
      }
    }
    DCHECK(found);
  }
#endif

  chrome_updater_factory_.InvalidateWeakPtrs();

  for (UpdateMap::const_iterator i = scheduled_updates_.begin();
       i != scheduled_updates_.end(); ++i) {
    // Do not dereference |contents|, it may be out-of-date!
    const WebContents* contents = i->first;
    unsigned flags = i->second;

    if (contents == tab_strip_model_->GetActiveWebContents()) {
      // Updates that only matter when the tab is selected go here.

      // Updating the URL happens synchronously in ScheduleUIUpdate.
      if (flags & content::INVALIDATE_TYPE_LOAD && GetStatusBubble()) {
        GetStatusBubble()->SetStatus(
            CoreTabHelper::FromWebContents(
                tab_strip_model_->GetActiveWebContents())
                ->GetStatusText());
      }

      if (flags &
          (content::INVALIDATE_TYPE_TAB | content::INVALIDATE_TYPE_TITLE)) {
        window_->UpdateTitleBar();
      }
    }

    // Updates that don't depend upon the selected state go here.
    if (flags & (content::INVALIDATE_TYPE_TAB | content::INVALIDATE_TYPE_TITLE |
                 content::INVALIDATE_TYPE_AUDIO)) {
      tab_strip_model_->UpdateWebContentsStateAt(
          tab_strip_model_->GetIndexOfWebContents(contents),
          TabChangeType::kAll);
    }

    // Update the bookmark bar. It may happen that the tab is crashed, and if
    // so, the bookmark bar should be hidden.
    if (flags & content::INVALIDATE_TYPE_TAB)
      UpdateBookmarkBarState(BOOKMARK_BAR_STATE_CHANGE_TAB_STATE);

    // We don't need to process INVALIDATE_STATE, since that's not visible.
  }

  scheduled_updates_.clear();
}

void Browser::RemoveScheduledUpdatesFor(WebContents* contents) {
  if (!contents)
    return;

  auto i = scheduled_updates_.find(contents);
  if (i != scheduled_updates_.end())
    scheduled_updates_.erase(i);
}

///////////////////////////////////////////////////////////////////////////////
// Browser, Getters for UI (private):

StatusBubble* Browser::GetStatusBubble() {
  // In kiosk and exclusive app mode, we want to always hide the status bubble.
  if (chrome::IsRunningInAppMode())
    return NULL;

  return window_ ? window_->GetStatusBubble() : NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Browser, Session restore functions (private):

void Browser::SyncHistoryWithTabs(int index) {
  SessionService* session_service =
      SessionServiceFactory::GetForProfileIfExisting(profile());
  if (session_service) {
    for (int i = index; i < tab_strip_model_->count(); ++i) {
      WebContents* web_contents = tab_strip_model_->GetWebContentsAt(i);
      if (web_contents) {
        sessions::SessionTabHelper* session_tab_helper =
            sessions::SessionTabHelper::FromWebContents(web_contents);
        session_service->SetTabIndexInWindow(
            session_id(), session_tab_helper->session_id(), i);
        session_service->SetPinnedState(session_id(),
                                        session_tab_helper->session_id(),
                                        tab_strip_model_->IsTabPinned(i));

        base::Optional<tab_groups::TabGroupId> group_id =
            tab_strip_model_->GetTabGroupForTab(i);
        session_service->SetTabGroup(session_id(),
                                     session_tab_helper->session_id(),
                                     std::move(group_id));
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Browser, In-progress download termination handling (private):

bool Browser::CanCloseWithInProgressDownloads() {
#if defined(OS_MACOSX) || defined(OS_CHROMEOS)
  // On Mac and ChromeOS, non-incognito download can still continue after window
  // is closed.
  if (!profile_->IsOffTheRecord())
    return true;
#endif

  // If we've prompted, we need to hear from the user before we
  // can close.
  if (cancel_download_confirmation_state_ != NOT_PROMPTED)
    return cancel_download_confirmation_state_ != WAITING_FOR_RESPONSE;

  int num_downloads_blocking;
  DownloadCloseType dialog_type =
      OkToCloseWithInProgressDownloads(&num_downloads_blocking);
  if (dialog_type == DownloadCloseType::kOk)
    return true;

  // Closing this window will kill some downloads; prompt to make sure
  // that's ok.
  cancel_download_confirmation_state_ = WAITING_FOR_RESPONSE;
  window_->ConfirmBrowserCloseWithPendingDownloads(
      num_downloads_blocking, dialog_type, false,
      base::Bind(&Browser::InProgressDownloadResponse,
                 weak_factory_.GetWeakPtr()));

  // Return false so the browser does not close.  We'll close if the user
  // confirms in the dialog.
  return false;
}

void Browser::InProgressDownloadResponse(bool cancel_downloads) {
  if (cancel_downloads) {
    cancel_download_confirmation_state_ = RESPONSE_RECEIVED;
    std::move(warn_before_closing_callback_)
        .Run(WarnBeforeClosingResult::kOkToClose);
    return;
  }

  // Sets the confirmation state to NOT_PROMPTED so that if the user tries to
  // close again we'll show the warning again.
  cancel_download_confirmation_state_ = NOT_PROMPTED;

  // Show the download page so the user can figure-out what downloads are still
  // in-progress.
  chrome::ShowDownloads(this);

  std::move(warn_before_closing_callback_)
      .Run(WarnBeforeClosingResult::kDoNotClose);
}

void Browser::FinishWarnBeforeClosing(WarnBeforeClosingResult result) {
  switch (result) {
    case WarnBeforeClosingResult::kOkToClose:
      chrome::CloseWindow(this);
      break;
    case WarnBeforeClosingResult::kDoNotClose:
      // Reset UnloadController::is_attempting_to_close_browser_ so that we
      // don't prompt every time any tab is closed. http://crbug.com/305516
      unload_controller_.CancelWindowClose();
  }
}

///////////////////////////////////////////////////////////////////////////////
// Browser, Assorted utility functions (private):

void Browser::SetAsDelegate(WebContents* web_contents, bool set_delegate) {
  Browser* delegate = set_delegate ? this : nullptr;

  // WebContents...
  web_contents->SetDelegate(delegate);

  // ...and all the helpers.
  WebContentsModalDialogManager::FromWebContents(web_contents)
      ->SetDelegate(delegate);
  //translate::ContentTranslateDriver* content_translate_driver =
  //    ChromeTranslateClient::FromWebContents(web_contents)->translate_driver();
  if (delegate) {
    zoom::ZoomController::FromWebContents(web_contents)->AddObserver(this);
    //content_translate_driver->AddObserver(this);
    BookmarkTabHelper::FromWebContents(web_contents)->AddObserver(this);
  } else {
    zoom::ZoomController::FromWebContents(web_contents)->RemoveObserver(this);
    //content_translate_driver->RemoveObserver(this);
    BookmarkTabHelper::FromWebContents(web_contents)->RemoveObserver(this);
  }
}

void Browser::CloseFrame() {
  window_->Close();
}

void Browser::TabDetachedAtImpl(content::WebContents* contents,
                                bool was_active,
                                DetachType type) {
  if (type == DETACH_TYPE_DETACH) {
    // Save the current location bar state, but only if the tab being detached
    // is the selected tab.  Because saving state can conditionally revert the
    // location bar, saving the current tab's location bar state to a
    // non-selected tab can corrupt both tabs.
    if (was_active) {
      LocationBar* location_bar = window()->GetLocationBar();
      if (location_bar)
        location_bar->SaveStateToContents(contents);
    }

    if (!tab_strip_model_->closing_all())
      SyncHistoryWithTabs(0);
  }

  SetAsDelegate(contents, false);
  RemoveScheduledUpdatesFor(contents);

  if (HasFindBarController() && was_active)
    find_bar_controller_->ChangeWebContents(NULL);

  for (size_t i = 0; i < interstitial_observers_.size(); i++) {
    if (interstitial_observers_[i]->web_contents() != contents)
      continue;

    delete interstitial_observers_[i];
    interstitial_observers_.erase(interstitial_observers_.begin() + i);
    return;
  }
}

void Browser::UpdateWindowForLoadingStateChanged(content::WebContents* source,
                                                 bool to_different_document) {
  window_->UpdateLoadingAnimations(tab_strip_model_->TabsAreLoading());
  window_->UpdateTitleBar();

  WebContents* selected_contents = tab_strip_model_->GetActiveWebContents();
  if (source == selected_contents) {
    bool is_loading = source->IsLoading() && to_different_document;
    command_controller_->LoadingStateChanged(is_loading, false);
    if (GetStatusBubble()) {
      GetStatusBubble()->SetStatus(CoreTabHelper::FromWebContents(
                                       tab_strip_model_->GetActiveWebContents())
                                       ->GetStatusText());
    }
  }
}

bool Browser::NormalBrowserSupportsWindowFeature(WindowFeature feature,
                                                 bool check_can_support) const {
  //bool fullscreen = ShouldHideUIForFullscreen();
  switch (feature) {
    case FEATURE_INFOBAR:
    case FEATURE_DOWNLOADSHELF:
    case FEATURE_BOOKMARKBAR:
      return false;
    case FEATURE_TABSTRIP:
    case FEATURE_TOOLBAR:
    case FEATURE_LOCATIONBAR:
      return false;
    case FEATURE_TITLEBAR:
    case FEATURE_NONE:
      return false;
    case FEATURE_NW_FRAMELESS:
      return true;
  }
}

bool Browser::PopupBrowserSupportsWindowFeature(WindowFeature feature,
                                                bool check_can_support) const {
  //bool fullscreen = ShouldHideUIForFullscreen();

  switch (feature) {
    case FEATURE_INFOBAR:
    case FEATURE_DOWNLOADSHELF:
      return true;
    case FEATURE_TITLEBAR:
      return true;
    case FEATURE_LOCATIONBAR:
      return false; //check_can_support || (!fullscreen && !is_trusted_source());
    case FEATURE_TABSTRIP:
    case FEATURE_TOOLBAR:
    case FEATURE_BOOKMARKBAR:
    case FEATURE_NONE:
      return false;
    case FEATURE_NW_FRAMELESS:
      return true;
  }
}

bool Browser::LegacyAppBrowserSupportsWindowFeature(
    WindowFeature feature,
    bool check_can_support) const {
  bool fullscreen = ShouldHideUIForFullscreen();
  switch (feature) {
    case FEATURE_TITLEBAR:
      return check_can_support || !fullscreen;
    case FEATURE_LOCATIONBAR:
      return false;
    default:
      return PopupBrowserSupportsWindowFeature(feature, check_can_support);
  }
}

bool Browser::WebAppBrowserSupportsWindowFeature(WindowFeature feature,
                                                 bool check_can_support) const {
  DCHECK(app_controller_);
  bool fullscreen = ShouldHideUIForFullscreen();
  switch (feature) {
    // Web apps should always support the toolbar, so the title/origin of the
    // current page can be shown when browsing a url that is not inside the app.
    // Note: Final determination of whether or not the toolbar is shown is made
    // by the |AppBrowserController|.
    // TODO(crbug.com/992834): Make this control the visibility of Browser
    // Controls more generally.
    case FEATURE_TOOLBAR:
    case FEATURE_INFOBAR:
    case FEATURE_DOWNLOADSHELF:
      return true;
    case FEATURE_TITLEBAR:
    // TODO(crbug.com/992834): Make this control the visibility of
    // CustomTabBarView.
    case FEATURE_LOCATIONBAR:
      return check_can_support || !fullscreen;
    case FEATURE_TABSTRIP:
      return app_controller_->has_tab_strip();
    case FEATURE_BOOKMARKBAR:
    case FEATURE_NONE:
      return false;
    case FEATURE_NW_FRAMELESS:
      return true;
  }
}

bool Browser::SupportsWindowFeatureImpl(WindowFeature feature,
                                        bool check_can_support) const {
  switch (type_) {
    case TYPE_NORMAL:
      return NormalBrowserSupportsWindowFeature(feature, check_can_support);
    case TYPE_POPUP:
    case TYPE_APP_POPUP:
      return PopupBrowserSupportsWindowFeature(feature, check_can_support);
    case TYPE_APP:
      // TODO(crbug.com/992834): Change to TYPE_WEB_APP.
      if (app_controller_)
        return WebAppBrowserSupportsWindowFeature(feature, check_can_support);
      // TODO(crbug.com/992834): Change to TYPE_LEGACY_APP.
      return LegacyAppBrowserSupportsWindowFeature(feature, check_can_support);
    case TYPE_DEVTOOLS:
      return LegacyAppBrowserSupportsWindowFeature(feature, check_can_support);
  }
}

void Browser::UpdateBookmarkBarState(BookmarkBarStateChangeReason reason) {
  BookmarkBar::State state =
      ShouldShowBookmarkBar() ? BookmarkBar::SHOW : BookmarkBar::HIDDEN;

  if (state == bookmark_bar_state_)
    return;

  bookmark_bar_state_ = state;

  if (!window_)
    return;  // This is called from the constructor when window_ is NULL.

  if (reason == BOOKMARK_BAR_STATE_CHANGE_TAB_SWITCH) {
    // Don't notify BrowserWindow on a tab switch as at the time this is invoked
    // BrowserWindow hasn't yet switched tabs. The BrowserWindow implementations
    // end up querying state once they process the tab switch.
    return;
  }

  bool should_animate = reason == BOOKMARK_BAR_STATE_CHANGE_PREF_CHANGE;
  window_->BookmarkBarStateChanged(
      should_animate ? BookmarkBar::ANIMATE_STATE_CHANGE
                     : BookmarkBar::DONT_ANIMATE_STATE_CHANGE);
}

bool Browser::ShouldShowBookmarkBar() const {
  if (profile_->IsGuestSession())
    return false;

  if (browser_defaults::bookmarks_enabled &&
      profile_->GetPrefs()->GetBoolean(bookmarks::prefs::kShowBookmarkBar) &&
      !ShouldHideUIForFullscreen())
    return true;

  WebContents* web_contents = tab_strip_model_->GetActiveWebContents();
  if (!web_contents)
    return false;

  BookmarkTabHelper* bookmark_tab_helper =
      BookmarkTabHelper::FromWebContents(web_contents);
  return bookmark_tab_helper && bookmark_tab_helper->ShouldShowBookmarkBar();
}

bool Browser::ShouldHideUIForFullscreen() const {
  // Windows and GTK remove the browser controls in fullscreen, but Mac and Ash
  // keep the controls in a slide-down panel.
  return window_ && window_->ShouldHideUIForFullscreen();
}

bool Browser::IsBrowserClosing() const {
  const BrowserList::BrowserSet& closing_browsers =
      BrowserList::GetInstance()->currently_closing_browsers();

  return base::Contains(closing_browsers, this);
}

bool Browser::ShouldStartShutdown() const {
  if (IsBrowserClosing())
    return false;

  const size_t closing_browsers_count =
      BrowserList::GetInstance()->currently_closing_browsers().size();
  return BrowserList::GetInstance()->size() == closing_browsers_count + 1u;
}

bool Browser::ShouldCreateBackgroundContents(
    content::SiteInstance* source_site_instance,
    const GURL& opener_url,
    const std::string& frame_name) {
  extensions::ExtensionService* extensions_service =
      extensions::ExtensionSystem::Get(profile_)->extension_service();

  if (!opener_url.is_valid() || frame_name.empty() || !extensions_service ||
      !extensions_service->is_ready())
    return false;

  // Only hosted apps have web extents, so this ensures that only hosted apps
  // can create BackgroundContents. We don't have to check for background
  // permission as that is checked in RenderMessageFilter when the CreateWindow
  // message is processed.
  const Extension* extension = extensions::ExtensionRegistry::Get(profile_)
                                   ->enabled_extensions()
                                   .GetHostedAppByURL(opener_url);
  if (!extension)
    return false;

  // No BackgroundContents allowed if BackgroundContentsService doesn't exist.
  BackgroundContentsService* service =
      BackgroundContentsServiceFactory::GetForProfile(profile_);
  if (!service)
    return false;

  // Ensure that we're trying to open this from the extension's process.
  extensions::ProcessMap* process_map = extensions::ProcessMap::Get(profile_);
  if (!source_site_instance->GetProcess() ||
      !process_map->Contains(extension->id(),
                             source_site_instance->GetProcess()->GetID())) {
    return false;
  }

  return true;
}

BackgroundContents* Browser::CreateBackgroundContents(
    content::SiteInstance* source_site_instance,
    content::RenderFrameHost* opener,
    const GURL& opener_url,
    bool is_new_browsing_instance,
    const std::string& frame_name,
    const GURL& target_url,
    const std::string& partition_id,
    content::SessionStorageNamespace* session_storage_namespace) {
  BackgroundContentsService* service =
      BackgroundContentsServiceFactory::GetForProfile(profile_);
  const Extension* extension = extensions::ExtensionRegistry::Get(profile_)
                                   ->enabled_extensions()
                                   .GetHostedAppByURL(opener_url);
  bool allow_js_access = extensions::BackgroundInfo::AllowJSAccess(extension);
  // Only allow a single background contents per app.
  BackgroundContents* existing =
      service->GetAppBackgroundContents(extension->id());
  if (existing) {
    // For non-scriptable background contents, ignore the request altogether,
    // Note that ShouldCreateBackgroundContents() returning true will also
    // suppress creation of the normal WebContents.
    if (!allow_js_access)
      return nullptr;
    // For scriptable background pages, if one already exists, close it (even
    // if it was specified in the manifest).
    service->DeleteBackgroundContents(existing);
  }

  // Passed all the checks, so this should be created as a BackgroundContents.
  if (allow_js_access) {
    return service->CreateBackgroundContents(
        source_site_instance, opener, is_new_browsing_instance, frame_name,
        extension->id(), partition_id, session_storage_namespace);
  }

  // If script access is not allowed, create the the background contents in a
  // new SiteInstance, so that a separate process is used. We must not use any
  // of the passed-in routing IDs, as they are objects in the opener's
  // process.
  BackgroundContents* contents = service->CreateBackgroundContents(
      content::SiteInstance::Create(source_site_instance->GetBrowserContext()),
      nullptr, is_new_browsing_instance, frame_name, extension->id(),
      partition_id, session_storage_namespace);

  // When a separate process is used, the original renderer cannot access the
  // new window later, thus we need to navigate the window now.
  contents->web_contents()->GetController().LoadURL(
      target_url, content::Referrer(), ui::PAGE_TRANSITION_LINK,
      std::string());  // No extra headers.

  return contents;
}
