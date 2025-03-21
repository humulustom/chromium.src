// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/extensions/extension_action_view_controller.h"

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/json/json_reader.h"
#include "base/run_loop.h"
#include "base/stl_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/extensions/api/extension_action/extension_action_api.h"
#include "chrome/browser/extensions/chrome_extensions_browser_client.h"
#include "chrome/browser/extensions/extension_action.h"
#include "chrome/browser/extensions/extension_action_manager.h"
#include "chrome/browser/extensions/extension_action_runner.h"
#include "chrome/browser/extensions/extension_action_test_util.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/scripting_permissions_modifier.h"
#include "chrome/browser/extensions/test_extension_system.h"
#include "chrome/browser/ui/extensions/icon_with_badge_image_source.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_bar.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_bar_unittest.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/grit/generated_resources.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/notification_service.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/notification_types.h"
#include "extensions/common/extension_builder.h"
#include "extensions/common/user_script.h"
#include "ui/base/l10n/l10n_util.h"

namespace {

// A helper class to create a "main" and "overflow" extension toolbar. This is
// used in tests that are relevant to the overflow behavior, and not valid with
// the new ExtensionsMenu (https://crbug.com/943702).
class LegacyToolbarTestHelper {
 public:
  explicit LegacyToolbarTestHelper(Browser* browser)
      : test_util_(ExtensionActionTestHelper::Create(browser, false)),
        overflow_test_util_(test_util_->CreateOverflowBar(browser)),
        main_bar_(test_util_->GetToolbarActionsBar()),
        overflow_bar_(overflow_test_util_->GetToolbarActionsBar()) {
    if (base::FeatureList::IsEnabled(features::kExtensionsToolbarMenu))
      ADD_FAILURE();
  }
  ~LegacyToolbarTestHelper() = default;

  LegacyToolbarTestHelper(const LegacyToolbarTestHelper& other) = delete;
  LegacyToolbarTestHelper& operator=(const LegacyToolbarTestHelper& other) =
      delete;

  ToolbarActionsBar* main_bar() { return main_bar_; }
  ToolbarActionsBar* overflow_bar() { return overflow_bar_; }

 private:
  std::unique_ptr<ExtensionActionTestHelper> test_util_;
  std::unique_ptr<ExtensionActionTestHelper> overflow_test_util_;
  ToolbarActionsBar* main_bar_ = nullptr;
  ToolbarActionsBar* overflow_bar_ = nullptr;
};

enum class ToolbarType {
  kExtensionsMenu,
  kLegacyToolbar,
};

}  // namespace

class ExtensionActionViewControllerUnitTest
    : public BrowserWithTestWindowTest,
      public testing::WithParamInterface<ToolbarType> {
 public:
  ExtensionActionViewControllerUnitTest() {
    if (GetParam() == ToolbarType::kExtensionsMenu) {
      scoped_feature_list_.InitAndEnableFeature(
          features::kExtensionsToolbarMenu);
    } else {
      DCHECK_EQ(ToolbarType::kLegacyToolbar, GetParam());
      scoped_feature_list_.InitAndDisableFeature(
          features::kExtensionsToolbarMenu);
    }
  }

  ExtensionActionViewControllerUnitTest(
      const ExtensionActionViewControllerUnitTest& other) = delete;
  ExtensionActionViewControllerUnitTest& operator=(
      const ExtensionActionViewControllerUnitTest& other) = delete;

  ~ExtensionActionViewControllerUnitTest() override = default;

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();

    // Initialize the various pieces of the extensions system.
    extensions::LoadErrorReporter::Init(false);
    extensions::TestExtensionSystem* extension_system =
        static_cast<extensions::TestExtensionSystem*>(
            extensions::ExtensionSystem::Get(profile()));
    extension_system->CreateExtensionService(
        base::CommandLine::ForCurrentProcess(), base::FilePath(), false);
    toolbar_model_ =
        extensions::extension_action_test_util::CreateToolbarModelForProfile(
            profile());
    extension_service_ =
        extensions::ExtensionSystem::Get(profile())->extension_service();

    test_util_ = ExtensionActionTestHelper::Create(browser(), false);

    view_size_ = test_util_->GetToolbarActionSize();
  }

  void TearDown() override {
    test_util_.reset();
    BrowserWithTestWindowTest::TearDown();
  }

  // Sets whether the given |action| wants to run on the |web_contents|.
  void SetActionWantsToRunOnTab(ExtensionAction* action,
                                content::WebContents* web_contents,
                                bool wants_to_run) {
    action->SetIsVisible(
        sessions::SessionTabHelper::IdForTab(web_contents).id(), wants_to_run);
    extensions::ExtensionActionAPI::Get(profile())->NotifyChange(
        action, web_contents, profile());
  }

  // Returns the active WebContents for the primary browser.
  content::WebContents* GetActiveWebContents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  ExtensionActionViewController* GetViewControllerForId(
      const std::string& action_id) {
    // It's safe to static cast here, because these tests only deal with
    // extensions.
    return static_cast<ExtensionActionViewController*>(
        test_util_->GetExtensionsContainer()->GetActionForId(action_id));
  }

  scoped_refptr<const extensions::Extension> CreateAndAddExtension(
      const std::string& name,
      extensions::ExtensionBuilder::ActionType action_type) {
    scoped_refptr<const extensions::Extension> extension =
        extensions::ExtensionBuilder(name)
            .SetAction(action_type)
            .SetLocation(extensions::Manifest::INTERNAL)
            .Build();
    extension_service()->AddExtension(extension.get());
    return extension;
  }

  extensions::ExtensionService* extension_service() {
    return extension_service_;
  }
  ToolbarActionsModel* toolbar_model() { return toolbar_model_; }
  const gfx::Size& view_size() const { return view_size_; }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;

  // The ExtensionService associated with the primary profile.
  extensions::ExtensionService* extension_service_ = nullptr;

  // ToolbarActionsModel associated with the main profile.
  ToolbarActionsModel* toolbar_model_ = nullptr;

  std::unique_ptr<ExtensionActionTestHelper> test_util_;

  // The standard size associated with a toolbar action view.
  gfx::Size view_size_;
};

// Helper classes used to instantiate a test for one version of the toolbar
// only. These are used for behavior that is specific to that version, such as
// overflow behavior.
class LegacyExtensionActionViewControllerUnitTest
    : public ExtensionActionViewControllerUnitTest {
 public:
  LegacyExtensionActionViewControllerUnitTest() {
    DCHECK_EQ(ToolbarType::kLegacyToolbar, GetParam());
  }
};
class ExtensionsMenuExtensionActionViewControllerUnitTest
    : public ExtensionActionViewControllerUnitTest {
 public:
  ExtensionsMenuExtensionActionViewControllerUnitTest() {
    DCHECK_EQ(ToolbarType::kExtensionsMenu, GetParam());
  }
};

// Tests the icon appearance of extension actions with the toolbar redesign.
// Extensions that don't want to run should have their icons grayscaled.
TEST_P(ExtensionActionViewControllerUnitTest,
       ExtensionActionWantsToRunAppearance) {
  const std::string id =
      CreateAndAddExtension(
          "extension", extensions::ExtensionBuilder::ActionType::PAGE_ACTION)
          ->id();

  AddTab(browser(), GURL("chrome://newtab"));

  content::WebContents* web_contents = GetActiveWebContents();
  ExtensionActionViewController* const action = GetViewControllerForId(id);
  ASSERT_TRUE(action);
  std::unique_ptr<IconWithBadgeImageSource> image_source =
      action->GetIconImageSourceForTesting(web_contents, view_size());
  EXPECT_TRUE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());

  SetActionWantsToRunOnTab(action->extension_action(), web_contents, true);
  image_source =
      action->GetIconImageSourceForTesting(web_contents, view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());
}

// Tests that overflowed extensions with page actions that want to run have an
// additional decoration.
// The overflow menu is only applicable to the legacy toolbar.
TEST_P(LegacyExtensionActionViewControllerUnitTest,
       OverflowedPageActionAppearance) {
  CreateAndAddExtension("extension",
                        extensions::ExtensionBuilder::ActionType::PAGE_ACTION);

  LegacyToolbarTestHelper test_helper(browser());
  EXPECT_EQ(1u, test_helper.main_bar()->GetIconCount());
  EXPECT_EQ(0u, test_helper.overflow_bar()->GetIconCount());

  AddTab(browser(), GURL("chrome://newtab"));

  content::WebContents* web_contents = GetActiveWebContents();

  toolbar_model()->SetVisibleIconCount(0u);
  EXPECT_EQ(0u, test_helper.main_bar()->GetIconCount());
  ASSERT_EQ(1u, test_helper.overflow_bar()->GetIconCount());

  auto* const action = static_cast<ExtensionActionViewController*>(
      test_helper.overflow_bar()->GetActions()[0]);
  std::unique_ptr<IconWithBadgeImageSource> image_source =
      action->GetIconImageSourceForTesting(web_contents, view_size());
  EXPECT_TRUE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());

  SetActionWantsToRunOnTab(action->extension_action(), web_contents, true);
  image_source =
      action->GetIconImageSourceForTesting(web_contents, view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_TRUE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());
}

// Tests the appearance of browser actions with blocked script actions.
TEST_P(ExtensionActionViewControllerUnitTest, BrowserActionBlockedActions) {
  scoped_refptr<const extensions::Extension> extension =
      extensions::ExtensionBuilder("browser action")
          .SetAction(extensions::ExtensionBuilder::ActionType::BROWSER_ACTION)
          .SetLocation(extensions::Manifest::INTERNAL)
          .AddPermission("https://www.google.com/*")
          .Build();

  extension_service()->GrantPermissions(extension.get());
  extension_service()->AddExtension(extension.get());
  extensions::ScriptingPermissionsModifier permissions_modifier(profile(),
                                                                extension);
  permissions_modifier.SetWithholdHostPermissions(true);

  AddTab(browser(), GURL("https://www.google.com/"));

  ExtensionActionViewController* const action_controller =
      GetViewControllerForId(extension->id());
  ASSERT_TRUE(action_controller);
  EXPECT_EQ(extension.get(), action_controller->extension());

  content::WebContents* web_contents = GetActiveWebContents();
  ASSERT_TRUE(web_contents);
  std::unique_ptr<IconWithBadgeImageSource> image_source =
      action_controller->GetIconImageSourceForTesting(web_contents,
                                                      view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());

  extensions::ExtensionActionRunner* action_runner =
      extensions::ExtensionActionRunner::GetForWebContents(web_contents);
  ASSERT_TRUE(action_runner);
  action_runner->RequestScriptInjectionForTesting(
      extension.get(), extensions::UserScript::DOCUMENT_IDLE,
      base::DoNothing());
  image_source = action_controller->GetIconImageSourceForTesting(web_contents,
                                                                 view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_TRUE(image_source->paint_blocked_actions_decoration());

  action_runner->RunForTesting(extension.get());
  image_source = action_controller->GetIconImageSourceForTesting(web_contents,
                                                                 view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());
}

// Tests the appearance of page actions with blocked script actions.
TEST_P(ExtensionActionViewControllerUnitTest, PageActionBlockedActions) {
  scoped_refptr<const extensions::Extension> extension =
      extensions::ExtensionBuilder("page action")
          .SetAction(extensions::ExtensionBuilder::ActionType::PAGE_ACTION)
          .SetLocation(extensions::Manifest::INTERNAL)
          .AddPermission("https://www.google.com/*")
          .Build();

  extension_service()->GrantPermissions(extension.get());
  extension_service()->AddExtension(extension.get());
  extensions::ScriptingPermissionsModifier permissions_modifier(profile(),
                                                                extension);
  permissions_modifier.SetWithholdHostPermissions(true);
  AddTab(browser(), GURL("https://www.google.com/"));

  ExtensionActionViewController* const action_controller =
      GetViewControllerForId(extension->id());
  ASSERT_TRUE(action_controller);
  EXPECT_EQ(extension.get(), action_controller->extension());

  content::WebContents* web_contents = GetActiveWebContents();
  std::unique_ptr<IconWithBadgeImageSource> image_source =
      action_controller->GetIconImageSourceForTesting(web_contents,
                                                      view_size());
  EXPECT_TRUE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());

  extensions::ExtensionActionRunner* action_runner =
      extensions::ExtensionActionRunner::GetForWebContents(web_contents);
  action_runner->RequestScriptInjectionForTesting(
      extension.get(), extensions::UserScript::DOCUMENT_IDLE,
      base::DoNothing());
  image_source = action_controller->GetIconImageSourceForTesting(web_contents,
                                                                 view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_TRUE(image_source->paint_blocked_actions_decoration());
}

// Tests the appearance of page actions with blocked actions in the overflow
// menu.
// The overflow menu is only applicable to the legacy toolbar.
TEST_P(LegacyExtensionActionViewControllerUnitTest,
       PageActionBlockedActionsInOverflow) {
  scoped_refptr<const extensions::Extension> extension =
      extensions::ExtensionBuilder("page action")
          .SetAction(extensions::ExtensionBuilder::ActionType::PAGE_ACTION)
          .SetLocation(extensions::Manifest::INTERNAL)
          .AddPermission("https://www.google.com/*")
          .Build();

  extension_service()->GrantPermissions(extension.get());
  extension_service()->AddExtension(extension.get());
  extensions::ScriptingPermissionsModifier permissions_modifier(profile(),
                                                                extension);
  permissions_modifier.SetWithholdHostPermissions(true);
  AddTab(browser(), GURL("https://www.google.com/"));

  // Overflow the page action and set the page action as wanting to run. We
  // shouldn't show the page action decoration because we are showing the
  // blocked action decoration (and should only show one at a time).
  toolbar_model()->SetVisibleIconCount(0u);

  LegacyToolbarTestHelper test_helper(browser());
  EXPECT_EQ(0u, test_helper.main_bar()->GetIconCount());
  EXPECT_EQ(1u, test_helper.overflow_bar()->GetIconCount());
  auto* const action = static_cast<ExtensionActionViewController*>(
      test_helper.overflow_bar()->GetActions()[0]);

  content::WebContents* web_contents = GetActiveWebContents();
  SetActionWantsToRunOnTab(action->extension_action(), web_contents, true);
  std::unique_ptr<IconWithBadgeImageSource> image_source =
      action->GetIconImageSourceForTesting(web_contents, view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_TRUE(image_source->paint_page_action_decoration());
  EXPECT_FALSE(image_source->paint_blocked_actions_decoration());

  extensions::ExtensionActionRunner* action_runner =
      extensions::ExtensionActionRunner::GetForWebContents(web_contents);
  action_runner->RequestScriptInjectionForTesting(
      extension.get(), extensions::UserScript::DOCUMENT_IDLE,
      base::DoNothing());

  image_source =
      action->GetIconImageSourceForTesting(web_contents, view_size());
  EXPECT_FALSE(image_source->grayscale());
  EXPECT_FALSE(image_source->paint_page_action_decoration());
  EXPECT_TRUE(image_source->paint_blocked_actions_decoration());
}

TEST_P(LegacyExtensionActionViewControllerUnitTest,
       ExtensionActionContextMenuVisibility) {
  std::string id =
      CreateAndAddExtension(
          "extension", extensions::ExtensionBuilder::ActionType::BROWSER_ACTION)
          ->id();

  // Check that the context menu has the proper string for the action's position
  // (in the main toolbar, in the overflow container, or temporarily popped
  // out).
  auto check_visibility_string = [](ToolbarActionViewController* action,
                                    int expected_visibility_string) {
    ui::SimpleMenuModel* context_menu =
        static_cast<ui::SimpleMenuModel*>(action->GetContextMenu());
    int visibility_index = context_menu->GetIndexOfCommandId(
        extensions::ExtensionContextMenuModel::TOGGLE_VISIBILITY);
    ASSERT_GE(visibility_index, 0);
    base::string16 visibility_label =
        context_menu->GetLabelAt(visibility_index);
    EXPECT_EQ(l10n_util::GetStringUTF16(expected_visibility_string),
              visibility_label);
  };

  LegacyToolbarTestHelper test_helper(browser());
  check_visibility_string(test_helper.main_bar()->GetActions()[0],
                          IDS_EXTENSIONS_HIDE_BUTTON_IN_MENU);
  toolbar_model()->SetVisibleIconCount(0u);
  check_visibility_string(test_helper.overflow_bar()->GetActions()[0],
                          IDS_EXTENSIONS_SHOW_BUTTON_IN_TOOLBAR);
  base::RunLoop run_loop;
  test_helper.main_bar()->PopOutAction(test_helper.main_bar()->GetActions()[0],
                                       false, run_loop.QuitClosure());
  run_loop.Run();
  check_visibility_string(test_helper.main_bar()->GetActions()[0],
                          IDS_EXTENSIONS_KEEP_BUTTON_IN_TOOLBAR);
}

TEST_P(ExtensionsMenuExtensionActionViewControllerUnitTest,
       ExtensionActionContextMenuVisibility) {
  std::string id =
      CreateAndAddExtension(
          "extension", extensions::ExtensionBuilder::ActionType::BROWSER_ACTION)
          ->id();

  // Check that the context menu has the proper string for the action's pinned
  // state.
  auto check_visibility_string = [](ToolbarActionViewController* action,
                                    int expected_visibility_string) {
    ui::SimpleMenuModel* context_menu =
        static_cast<ui::SimpleMenuModel*>(action->GetContextMenu());
    int visibility_index = context_menu->GetIndexOfCommandId(
        extensions::ExtensionContextMenuModel::TOGGLE_VISIBILITY);
    ASSERT_GE(visibility_index, 0);
    base::string16 visibility_label =
        context_menu->GetLabelAt(visibility_index);
    EXPECT_EQ(l10n_util::GetStringUTF16(expected_visibility_string),
              visibility_label);
  };

  ExtensionActionViewController* const action = GetViewControllerForId(id);
  ASSERT_TRUE(action);

  // Default state: unpinned.
  check_visibility_string(action, IDS_EXTENSIONS_PIN_TO_TOOLBAR);
  toolbar_model()->SetActionVisibility(id, true);
  check_visibility_string(action, IDS_EXTENSIONS_UNPIN_FROM_TOOLBAR);
}

class ExtensionActionViewControllerGrayscaleTest
    : public ExtensionActionViewControllerUnitTest {
 public:
  enum class PermissionType {
    kScriptableHost,
    kExplicitHost,
  };

  ExtensionActionViewControllerGrayscaleTest() {}
  ~ExtensionActionViewControllerGrayscaleTest() override = default;

  void RunGrayscaleTest(PermissionType permission_type);

 private:
  scoped_refptr<const extensions::Extension> CreateExtension(
      PermissionType permission_type);

  DISALLOW_COPY_AND_ASSIGN(ExtensionActionViewControllerGrayscaleTest);
};

void ExtensionActionViewControllerGrayscaleTest::RunGrayscaleTest(
    PermissionType permission_type) {
  scoped_refptr<const extensions::Extension> extension =
      CreateExtension(permission_type);
  extension_service()->GrantPermissions(extension.get());
  extension_service()->AddExtension(extension.get());

  extensions::ScriptingPermissionsModifier permissions_modifier(profile(),
                                                                extension);
  permissions_modifier.SetWithholdHostPermissions(true);
  const GURL kUrl("https://www.google.com/");

  // Make sure UserScriptListener doesn't hold up the navigation.
  extensions::ExtensionsBrowserClient::Get()
      ->GetUserScriptListener()
      ->TriggerUserScriptsReadyForTesting(browser()->profile());

  AddTab(browser(), kUrl);

  enum class ActionState {
    kEnabled,
    kDisabled,
  };
  enum class PageAccess {
    kGranted,
    kPending,
    kNone,
  };
  enum class Opacity {
    kGrayscale,
    kFull,
  };
  enum class BlockedActions {
    kPainted,
    kNotPainted,
  };

  struct {
    ActionState action_state;
    PageAccess page_access;
    Opacity expected_opacity;
    BlockedActions expected_blocked_actions;
  } test_cases[] = {
      {ActionState::kEnabled, PageAccess::kNone, Opacity::kFull,
       BlockedActions::kNotPainted},
      {ActionState::kEnabled, PageAccess::kPending, Opacity::kFull,
       BlockedActions::kPainted},
      {ActionState::kEnabled, PageAccess::kGranted, Opacity::kFull,
       BlockedActions::kNotPainted},

      {ActionState::kDisabled, PageAccess::kNone, Opacity::kGrayscale,
       BlockedActions::kNotPainted},
      {ActionState::kDisabled, PageAccess::kPending, Opacity::kFull,
       BlockedActions::kPainted},
      {ActionState::kDisabled, PageAccess::kGranted, Opacity::kFull,
       BlockedActions::kNotPainted},
  };

  ExtensionActionViewController* const controller =
      GetViewControllerForId(extension->id());
  ASSERT_TRUE(controller);
  content::WebContents* web_contents = GetActiveWebContents();
  ExtensionAction* extension_action =
      extensions::ExtensionActionManager::Get(profile())->GetExtensionAction(
          *extension);
  extensions::ExtensionActionRunner* action_runner =
      extensions::ExtensionActionRunner::GetForWebContents(web_contents);
  int tab_id = sessions::SessionTabHelper::IdForTab(web_contents).id();

  for (size_t i = 0; i < base::size(test_cases); ++i) {
    SCOPED_TRACE(
        base::StringPrintf("Running test case %d", static_cast<int>(i)));
    const auto& test_case = test_cases[i];

    // Set up the proper state.
    extension_action->SetIsVisible(
        tab_id, test_case.action_state == ActionState::kEnabled);
    switch (test_case.page_access) {
      case PageAccess::kNone: {
        // Page access should already be withheld; verify.
        extensions::PermissionsData::PageAccess page_access =
            extensions::PermissionsData::PageAccess::kDenied;
        switch (permission_type) {
          case PermissionType::kExplicitHost:
            page_access = extension->permissions_data()->GetPageAccess(
                kUrl, tab_id, /*error=*/nullptr);
            break;
          case PermissionType::kScriptableHost:
            page_access = extension->permissions_data()->GetContentScriptAccess(
                kUrl, tab_id, /*error=*/nullptr);
            break;
        }
        EXPECT_EQ(extensions::PermissionsData::PageAccess::kWithheld,
                  page_access);
        break;
      }
      case PageAccess::kPending:
        action_runner->RequestScriptInjectionForTesting(
            extension.get(), extensions::UserScript::DOCUMENT_IDLE,
            base::DoNothing());
        break;
      case PageAccess::kGranted:
        permissions_modifier.GrantHostPermission(kUrl);
        break;
    }

    std::unique_ptr<IconWithBadgeImageSource> image_source =
        controller->GetIconImageSourceForTesting(web_contents, view_size());
    EXPECT_EQ(test_case.expected_opacity == Opacity::kGrayscale,
              image_source->grayscale());
    EXPECT_EQ(test_case.expected_blocked_actions == BlockedActions::kPainted,
              image_source->paint_blocked_actions_decoration());

    // Clean up permissions state.
    if (test_case.page_access == PageAccess::kGranted)
      permissions_modifier.RemoveGrantedHostPermission(kUrl);
    action_runner->ClearInjectionsForTesting(*extension);
  }
}

scoped_refptr<const extensions::Extension>
ExtensionActionViewControllerGrayscaleTest::CreateExtension(
    PermissionType permission_type) {
  extensions::ExtensionBuilder builder("extension");
  builder.SetAction(extensions::ExtensionBuilder::ActionType::BROWSER_ACTION)
      .SetLocation(extensions::Manifest::INTERNAL);
  switch (permission_type) {
    case PermissionType::kScriptableHost: {
      std::unique_ptr<base::Value> content_scripts =
          base::JSONReader::ReadDeprecated(
              R"([{
                     "matches": ["https://www.google.com/*"],
                     "js": ["script.js"]
                 }])");
      builder.SetManifestKey("content_scripts", std::move(content_scripts));
      break;
    }
    case PermissionType::kExplicitHost:
      builder.AddPermission("https://www.google.com/*");
      break;
  }

  return builder.Build();
}

// Tests the behavior for icon grayscaling. Ideally, these would be a single
// parameterized test, but toolbar tests are already parameterized with the UI
// mode.
TEST_P(ExtensionActionViewControllerGrayscaleTest,
       GrayscaleIcon_ExplicitHosts) {
  RunGrayscaleTest(PermissionType::kExplicitHost);
}
TEST_P(ExtensionActionViewControllerGrayscaleTest,
       GrayscaleIcon_ScriptableHosts) {
  RunGrayscaleTest(PermissionType::kScriptableHost);
}

INSTANTIATE_TEST_SUITE_P(All,
                         ExtensionActionViewControllerGrayscaleTest,
                         testing::Values(ToolbarType::kExtensionsMenu,
                                         ToolbarType::kLegacyToolbar));

TEST_P(ExtensionActionViewControllerUnitTest, RuntimeHostsTooltip) {
  scoped_refptr<const extensions::Extension> extension =
      extensions::ExtensionBuilder("extension name")
          .SetAction(extensions::ExtensionBuilder::ActionType::BROWSER_ACTION)
          .SetLocation(extensions::Manifest::INTERNAL)
          .AddPermission("https://www.google.com/*")
          .Build();
  extension_service()->GrantPermissions(extension.get());
  extension_service()->AddExtension(extension.get());

  extensions::ScriptingPermissionsModifier permissions_modifier(profile(),
                                                                extension);
  permissions_modifier.SetWithholdHostPermissions(true);
  const GURL kUrl("https://www.google.com/");
  AddTab(browser(), kUrl);

  ExtensionActionViewController* const controller =
      GetViewControllerForId(extension->id());
  ASSERT_TRUE(controller);
  content::WebContents* web_contents = GetActiveWebContents();
  int tab_id = sessions::SessionTabHelper::IdForTab(web_contents).id();

  // Page access should already be withheld.
  EXPECT_EQ(extensions::PermissionsData::PageAccess::kWithheld,
            extension->permissions_data()->GetPageAccess(kUrl, tab_id,
                                                         /*error=*/nullptr));
  EXPECT_EQ("extension name",
            base::UTF16ToUTF8(controller->GetTooltip(web_contents)));

  // Request access.
  extensions::ExtensionActionRunner* action_runner =
      extensions::ExtensionActionRunner::GetForWebContents(web_contents);
  action_runner->RequestScriptInjectionForTesting(
      extension.get(), extensions::UserScript::DOCUMENT_IDLE,
      base::DoNothing());
  EXPECT_EQ("extension name\nWants access to this site",
            base::UTF16ToUTF8(controller->GetTooltip(web_contents)));

  // Grant access.
  action_runner->ClearInjectionsForTesting(*extension);
  permissions_modifier.GrantHostPermission(kUrl);
  EXPECT_EQ("extension name\nHas access to this site",
            base::UTF16ToUTF8(controller->GetTooltip(web_contents)));
}

// ExtensionActionViewController::GetIcon() can potentially be called with a
// null web contents if the tab strip model doesn't know of an active tab
// (though it's a bit unclear when this is the case).
// See https://crbug.com/888121
TEST_P(ExtensionActionViewControllerUnitTest, TestGetIconWithNullWebContents) {
  scoped_refptr<const extensions::Extension> extension =
      extensions::ExtensionBuilder("extension name")
          .SetAction(extensions::ExtensionBuilder::ActionType::BROWSER_ACTION)
          .AddPermission("https://example.com/")
          .Build();

  extension_service()->GrantPermissions(extension.get());
  extension_service()->AddExtension(extension.get());

  extensions::ScriptingPermissionsModifier permissions_modifier(profile(),
                                                                extension);
  permissions_modifier.SetWithholdHostPermissions(true);

  // Try getting an icon with no active web contents. Nothing should crash, and
  // a non-empty icon should be returned.
  ExtensionActionViewController* const controller =
      GetViewControllerForId(extension->id());
  gfx::Image icon = controller->GetIcon(nullptr, view_size());
  EXPECT_FALSE(icon.IsEmpty());
}

INSTANTIATE_TEST_SUITE_P(All,
                         ExtensionActionViewControllerUnitTest,
                         testing::Values(ToolbarType::kExtensionsMenu,
                                         ToolbarType::kLegacyToolbar));
INSTANTIATE_TEST_SUITE_P(,
                         LegacyExtensionActionViewControllerUnitTest,
                         testing::Values(ToolbarType::kLegacyToolbar));
INSTANTIATE_TEST_SUITE_P(,
                         ExtensionsMenuExtensionActionViewControllerUnitTest,
                         testing::Values(ToolbarType::kExtensionsMenu));
