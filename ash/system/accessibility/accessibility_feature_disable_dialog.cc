// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/accessibility/accessibility_feature_disable_dialog.h"

#include <memory>
#include <utility>

#include "ash/public/cpp/shell_window_ids.h"
#include "ash/session/session_controller_impl.h"
#include "ash/shell.h"
#include "ash/strings/grit/ash_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/ui_base_types.h"
#include "ui/views/border.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/widget/widget.h"

namespace ash {

AccessibilityFeatureDisableDialog::AccessibilityFeatureDisableDialog(
    int window_title_text_id,
    int dialog_text_id,
    base::OnceClosure on_accept_callback,
    base::OnceClosure on_cancel_callback)
    : window_title_(l10n_util::GetStringUTF16(window_title_text_id)),
      on_accept_callback_(std::move(on_accept_callback)),
      on_cancel_callback_(std::move(on_cancel_callback)) {
  DialogDelegate::set_button_label(
      ui::DIALOG_BUTTON_OK, l10n_util::GetStringUTF16(IDS_ASH_YES_BUTTON));

  SetLayoutManager(std::make_unique<views::FillLayout>());
  SetBorder(views::CreateEmptyBorder(
      views::LayoutProvider::Get()->GetDialogInsetsForContentType(
          views::TEXT, views::TEXT)));
  AddChildView(std::make_unique<views::Label>(
      l10n_util::GetStringUTF16(dialog_text_id)));

  // Parent the dialog widget to the LockSystemModalContainer, or
  // OverlayContainer to ensure that it will get displayed on respective
  // lock/signin or OOBE screen.
  SessionControllerImpl* session_controller =
      Shell::Get()->session_controller();
  int container_id = kShellWindowId_SystemModalContainer;
  if (session_controller->GetSessionState() ==
      session_manager::SessionState::OOBE) {
    container_id = kShellWindowId_OverlayContainer;
  } else if (session_controller->IsUserSessionBlocked()) {
    container_id = kShellWindowId_LockSystemModalContainer;
  }

  views::Widget* widget = CreateDialogWidget(
      this, nullptr,
      Shell::GetContainer(Shell::GetPrimaryRootWindow(), container_id));
  widget->Show();
}

AccessibilityFeatureDisableDialog::~AccessibilityFeatureDisableDialog() =
    default;

bool AccessibilityFeatureDisableDialog::Cancel() {
  std::move(on_cancel_callback_).Run();
  return true;
}

bool AccessibilityFeatureDisableDialog::Accept() {
  std::move(on_accept_callback_).Run();
  return true;
}

ui::ModalType AccessibilityFeatureDisableDialog::GetModalType() const {
  return ui::MODAL_TYPE_SYSTEM;
}

base::string16 AccessibilityFeatureDisableDialog::GetWindowTitle() const {
  return window_title_;
}

base::WeakPtr<AccessibilityFeatureDisableDialog>
AccessibilityFeatureDisableDialog::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

const char* AccessibilityFeatureDisableDialog::GetClassName() const {
  return "AccessibilityFeatureDisableDialog";
}

}  // namespace ash
