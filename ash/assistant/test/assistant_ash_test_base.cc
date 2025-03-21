// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/assistant/test/assistant_ash_test_base.h"

#include <string>
#include <utility>

#include "ash/app_list/app_list_controller_impl.h"
#include "ash/assistant/assistant_controller.h"
#include "ash/assistant/test/test_assistant_web_view_factory.h"
#include "ash/keyboard/ui/keyboard_ui_controller.h"
#include "ash/keyboard/ui/test/keyboard_test_util.h"
#include "ash/public/cpp/app_list/app_list_features.h"
#include "ash/public/cpp/test/assistant_test_api.h"
#include "ash/shell.h"
#include "base/run_loop.h"

namespace ash {

namespace {

using chromeos::assistant::mojom::AssistantInteractionMetadata;
using chromeos::assistant::mojom::AssistantInteractionType;

gfx::Point GetPointInside(const views::View* view) {
  return view->GetBoundsInScreen().CenterPoint();
}

bool CanProcessEvents(const views::View* view) {
  const views::View* ancestor = view;
  while (ancestor != nullptr) {
    if (!ancestor->CanProcessEventsWithinSubtree())
      return false;
    ancestor = ancestor->parent();
  }
  return true;
}

void CheckCanProcessEvents(const views::View* view) {
  if (!view->IsDrawn()) {
    ADD_FAILURE()
        << view->GetClassName()
        << " can not process events because it is not drawn on screen.";
  } else if (!CanProcessEvents(view)) {
    ADD_FAILURE() << view->GetClassName() << " can not process events.";
  }
}

void PressHomeButton() {
  Shell::Get()->app_list_controller()->ToggleAppList(
      display::Screen::GetScreen()->GetPrimaryDisplay().id(),
      AppListShowSource::kShelfButton, base::TimeTicks::Now());
}

}  // namespace

AssistantAshTestBase::AssistantAshTestBase()
    : test_api_(AssistantTestApi::Create()),
      test_web_view_factory_(std::make_unique<TestAssistantWebViewFactory>()) {}

AssistantAshTestBase::~AssistantAshTestBase() = default;

void AssistantAshTestBase::SetUp() {
  scoped_feature_list_.InitAndEnableFeature(
      app_list_features::kEnableAssistantLauncherUI);

  AshTestBase::SetUp();

  // Make the display big enough to hold the app list.
  UpdateDisplay("1024x768");

  // Enable Assistant in settings.
  test_api_->SetAssistantEnabled(true);

  // Cache controller.
  controller_ = Shell::Get()->assistant_controller();
  DCHECK(controller_);

  // At this point our Assistant service is ready for use.
  // Indicate this by changing status from NOT_READY to READY.
  test_api_->GetAssistantState()->NotifyStatusChanged(
      mojom::AssistantState::READY);

  test_api_->DisableAnimations();

  EnableKeyboard();
}

void AssistantAshTestBase::TearDown() {
  windows_.clear();
  widgets_.clear();
  DisableKeyboard();
  AshTestBase::TearDown();
  scoped_feature_list_.Reset();
}

void AssistantAshTestBase::ShowAssistantUi(AssistantEntryPoint entry_point) {
  if (entry_point == AssistantEntryPoint::kHotword) {
    // If the Assistant is triggered via Hotword, the interaction is triggered
    // by the Assistant service.
    assistant_service()->StartVoiceInteraction();
  } else {
    // Otherwise, the interaction is triggered by a call to |ShowUi|.
    controller_->ui_controller()->ShowUi(entry_point);
  }
  // Send all mojom messages to/from the assistant service.
  base::RunLoop().RunUntilIdle();
}

void AssistantAshTestBase::CloseAssistantUi(AssistantExitPoint exit_point) {
  controller_->ui_controller()->CloseUi(exit_point);
}

void AssistantAshTestBase::OpenLauncher() {
  PressHomeButton();
}

void AssistantAshTestBase::CloseLauncher() {
  Shell::Get()->app_list_controller()->ViewClosing();
}

void AssistantAshTestBase::SetTabletMode(bool enable) {
  test_api_->SetTabletMode(enable);
}

void AssistantAshTestBase::SetPreferVoice(bool prefer_voice) {
  test_api_->SetPreferVoice(prefer_voice);
}

bool AssistantAshTestBase::IsVisible() {
  return test_api_->IsVisible();
}

views::View* AssistantAshTestBase::main_view() {
  return test_api_->main_view();
}

views::View* AssistantAshTestBase::page_view() {
  return test_api_->page_view();
}

views::View* AssistantAshTestBase::app_list_view() {
  return test_api_->app_list_view();
}

views::View* AssistantAshTestBase::root_view() {
  views::View* result = app_list_view();
  while (result && result->parent())
    result = result->parent();
  return result;
}

void AssistantAshTestBase::MockAssistantInteractionWithResponse(
    const std::string& response_text) {
  MockAssistantInteractionWithQueryAndResponse(/*query=*/"input text",
                                               response_text);
}

void AssistantAshTestBase::MockAssistantInteractionWithQueryAndResponse(
    const std::string& query,
    const std::string& response_text) {
  SendQueryThroughTextField(query);
  auto response = std::make_unique<InteractionResponse>();
  response->AddTextResponse(response_text)
      ->AddResolution(InteractionResponse::Resolution::kNormal);
  assistant_service()->SetInteractionResponse(std::move(response));

  base::RunLoop().RunUntilIdle();
}

void AssistantAshTestBase::SendQueryThroughTextField(const std::string& query) {
  test_api_->SendTextQuery(query);
}

void AssistantAshTestBase::TapOnAndWait(views::View* view) {
  CheckCanProcessEvents(view);
  TapAndWait(GetPointInside(view));
}

void AssistantAshTestBase::TapAndWait(gfx::Point position) {
  GetEventGenerator()->GestureTapAt(position);

  base::RunLoop().RunUntilIdle();
}

void AssistantAshTestBase::ClickOnAndWait(views::View* view) {
  CheckCanProcessEvents(view);
  GetEventGenerator()->MoveMouseTo(GetPointInside(view));
  GetEventGenerator()->ClickLeftButton();

  base::RunLoop().RunUntilIdle();
}

base::Optional<chromeos::assistant::mojom::AssistantInteractionMetadata>
AssistantAshTestBase::current_interaction() {
  return assistant_service()->current_interaction();
}

aura::Window* AssistantAshTestBase::SwitchToNewAppWindow() {
  windows_.push_back(CreateAppWindow());

  aura::Window* window = windows_.back().get();
  window->SetName("<app-window>");
  return window;
}

views::Widget* AssistantAshTestBase::SwitchToNewWidget() {
  widgets_.push_back(CreateTestWidget());

  views::Widget* result = widgets_.back().get();
  // Give the widget a non-zero size, otherwise things like tapping and clicking
  // on it do not work.
  result->SetBounds(gfx::Rect(500, 100));
  return result;
}

aura::Window* AssistantAshTestBase::window() {
  return test_api_->window();
}

views::Textfield* AssistantAshTestBase::input_text_field() {
  return test_api_->input_text_field();
}

views::View* AssistantAshTestBase::mic_view() {
  return test_api_->mic_view();
}

views::View* AssistantAshTestBase::greeting_label() {
  return test_api_->greeting_label();
}

views::View* AssistantAshTestBase::voice_input_toggle() {
  return test_api_->voice_input_toggle();
}

views::View* AssistantAshTestBase::keyboard_input_toggle() {
  return test_api_->keyboard_input_toggle();
}

void AssistantAshTestBase::ShowKeyboard() {
  auto* keyboard_controller = keyboard::KeyboardUIController::Get();
  keyboard_controller->ShowKeyboard(/*lock=*/false);
}

void AssistantAshTestBase::DismissKeyboard() {
  auto* keyboard_controller = keyboard::KeyboardUIController::Get();
  keyboard_controller->HideKeyboardImplicitlyByUser();
  EXPECT_FALSE(IsKeyboardShowing());
}

bool AssistantAshTestBase::IsKeyboardShowing() const {
  auto* keyboard_controller = keyboard::KeyboardUIController::Get();
  return keyboard_controller->IsEnabled() && keyboard::IsKeyboardShowing();
}

AssistantInteractionController* AssistantAshTestBase::interaction_controller() {
  return controller_->interaction_controller();
}

const AssistantInteractionModel* AssistantAshTestBase::interaction_model() {
  return interaction_controller()->model();
}

TestAssistantService* AssistantAshTestBase::assistant_service() {
  return ash_test_helper()->test_assistant_service();
}

}  // namespace ash
