// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ash/chrome_shell_delegate.h"

#include <memory>

#include "ash/screenshot_delegate.h"
#include "base/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/browser_process_platform_part_chromeos.h"
#include "chrome/browser/chromeos/multidevice_setup/multidevice_setup_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/ash/chrome_accessibility_delegate.h"
#include "chrome/browser/ui/ash/chrome_screenshot_grabber.h"
#include "chrome/browser/ui/ash/keyboard/chrome_keyboard_ui.h"
#include "chrome/browser/ui/ash/session_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/scoped_tabbed_browser_displayer.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chromeos/services/multidevice_setup/multidevice_setup_service.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/media_session_service.h"
#include "ui/aura/window.h"
#include "url/gurl.h"

namespace {

const char kKeyboardShortcutHelpPageUrl[] =
    "https://support.google.com/chromebook/answer/183101";

}  // namespace

ChromeShellDelegate::ChromeShellDelegate() = default;

ChromeShellDelegate::~ChromeShellDelegate() = default;

bool ChromeShellDelegate::CanShowWindowForUser(
    const aura::Window* window) const {
  return ::CanShowWindowForUser(window,
                                base::BindRepeating(&GetActiveBrowserContext));
}

void ChromeShellDelegate::OpenKeyboardShortcutHelpPage() const {
  chrome::ScopedTabbedBrowserDisplayer scoped_tabbed_browser_displayer(
      ProfileManager::GetActiveUserProfile());
  NavigateParams params(scoped_tabbed_browser_displayer.browser(),
                        GURL(kKeyboardShortcutHelpPageUrl),
                        ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  params.disposition = WindowOpenDisposition::SINGLETON_TAB;
  Navigate(&params);
}

bool ChromeShellDelegate::CanGoBack(gfx::NativeWindow window) const {
  BrowserView* browser_view =
      BrowserView::GetBrowserViewForNativeWindow(window);
  if (!browser_view)
    return false;
  content::WebContents* contents =
      browser_view->browser()->tab_strip_model()->GetActiveWebContents();
  if (!contents)
    return false;
  return contents->GetController().CanGoBack();
}

void ChromeShellDelegate::BindBluetoothSystemFactory(
    mojo::PendingReceiver<device::mojom::BluetoothSystemFactory> receiver) {
  content::GetDeviceService().BindBluetoothSystemFactory(std::move(receiver));
}

void ChromeShellDelegate::BindFingerprint(
    mojo::PendingReceiver<device::mojom::Fingerprint> receiver) {
  content::GetDeviceService().BindFingerprint(std::move(receiver));
}

void ChromeShellDelegate::BindNavigableContentsFactory(
    mojo::PendingReceiver<content::mojom::NavigableContentsFactory> receiver) {
  ProfileManager::GetActiveUserProfile()->BindNavigableContentsFactory(
      std::move(receiver));
}

void ChromeShellDelegate::BindMultiDeviceSetup(
    mojo::PendingReceiver<chromeos::multidevice_setup::mojom::MultiDeviceSetup>
        receiver) {
  chromeos::multidevice_setup::MultiDeviceSetupService* service =
      chromeos::multidevice_setup::MultiDeviceSetupServiceFactory::
          GetForProfile(ProfileManager::GetPrimaryUserProfile());
  if (service)
    service->BindMultiDeviceSetup(std::move(receiver));
}

media_session::mojom::MediaSessionService*
ChromeShellDelegate::GetMediaSessionService() {
  return &content::GetMediaSessionService();
}

ash::AccessibilityDelegate* ChromeShellDelegate::CreateAccessibilityDelegate() {
  return new ChromeAccessibilityDelegate;
}

std::unique_ptr<ash::ScreenshotDelegate>
ChromeShellDelegate::CreateScreenshotDelegate() {
  return std::make_unique<ChromeScreenshotGrabber>();
}

