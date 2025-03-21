// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_PASSWORDS_BUBBLE_CONTROLLERS_PASSWORD_BUBBLE_CONTROLLER_BASE_H_
#define CHROME_BROWSER_UI_PASSWORDS_BUBBLE_CONTROLLERS_PASSWORD_BUBBLE_CONTROLLER_BASE_H_

#include "base/memory/weak_ptr.h"
#include "components/password_manager/core/browser/password_manager_metrics_util.h"

namespace content {
class WebContents;
}

namespace password_manager {
class PasswordFormMetricsRecorder;
}

class PasswordsModelDelegate;
class Profile;

// This is the base class for all bubble controllers. There should be a bubble
// controller per view. Bubble controller provides the data and controls the
// password management actions for the coressponding view.
class PasswordBubbleControllerBase {
 public:
  PasswordBubbleControllerBase(
      base::WeakPtr<PasswordsModelDelegate> delegate,
      password_manager::metrics_util::UIDisplayDisposition display_disposition);

  PasswordBubbleControllerBase(const PasswordBubbleControllerBase&) = delete;
  PasswordBubbleControllerBase& operator=(const PasswordBubbleControllerBase&) =
      delete;

  virtual ~PasswordBubbleControllerBase();

  // Subclasses must override this method to provide the proper title.
  virtual base::string16 GetTitle() const = 0;

  // Subclasses must override this method to report their interactions.
  virtual void ReportInteractions() = 0;

  // The method MAY BE called to record the statistics while the bubble is
  // being closed. Otherwise, it is called later on when the controller is
  // destroyed.
  void OnBubbleClosing();

  Profile* GetProfile() const;
  content::WebContents* GetWebContents() const;

 protected:
  // Reference to metrics recorder of the PasswordForm presented to the user by
  // |this|. We hold on to this because |delegate_| may not be able to provide
  // the reference anymore when we need it.
  scoped_refptr<password_manager::PasswordFormMetricsRecorder>
      metrics_recorder_;

  // True if the model has already recorded all the necessary statistics when
  // the bubble is closing.
  bool interaction_reported_ = false;

  // A bridge to ManagePasswordsUIController instance.
  base::WeakPtr<PasswordsModelDelegate> delegate_;
};

#endif  // CHROME_BROWSER_UI_PASSWORDS_BUBBLE_CONTROLLERS_PASSWORD_BUBBLE_CONTROLLER_BASE_H_
