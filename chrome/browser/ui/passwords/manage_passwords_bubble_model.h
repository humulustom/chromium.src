// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_PASSWORDS_MANAGE_PASSWORDS_BUBBLE_MODEL_H_
#define CHROME_BROWSER_UI_PASSWORDS_MANAGE_PASSWORDS_BUBBLE_MODEL_H_

#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/time/clock.h"
#include "components/autofill/core/common/password_form.h"
#include "components/password_manager/core/browser/manage_passwords_referrer.h"
#include "components/password_manager/core/browser/password_manager_metrics_util.h"
#include "components/password_manager/core/browser/statistics_table.h"
#include "components/password_manager/core/common/password_manager_ui.h"

namespace content {
class WebContents;
}

namespace password_manager {
class PasswordFormMetricsRecorder;
}

class PasswordsModelDelegate;
class Profile;

// This model provides data for the ManagePasswordsBubble and controls the
// password management actions.
class ManagePasswordsBubbleModel {
 public:
  enum PasswordAction { REMOVE_PASSWORD, ADD_PASSWORD };
  enum DisplayReason { AUTOMATIC, USER_ACTION };

  // Creates a ManagePasswordsBubbleModel, which holds a weak pointer to the
  // delegate. Construction implies that the bubble is shown. The bubble's state
  // is updated from the ManagePasswordsUIController associated with |delegate|.
  ManagePasswordsBubbleModel(base::WeakPtr<PasswordsModelDelegate> delegate,
                             DisplayReason reason);
  ~ManagePasswordsBubbleModel();

  // The method MAY BE called to record the statistics while the bubble is being
  // closed. Otherwise, it is called later on when the model is destroyed.
  void OnBubbleClosing();

  // Called by the view code when the "Nope" button in clicked by the user in
  // update bubble.
  void OnNopeUpdateClicked();

  // Called by the view code when the "Never for this site." button in clicked
  // by the user.
  void OnNeverForThisSiteClicked();

  // Called by the view code when username or password is corrected using
  // the username correction or password selection features in PendingView.
  void OnCredentialEdited(base::string16 new_username,
                          base::string16 new_password);

  // Called by the view code when the save/update button is clicked by the user.
  void OnSaveClicked();

#if defined(PASSWORD_STORE_SELECT_ENABLED)
  // Called by the view when the account store checkbox is toggled.
  void OnToggleAccountStore(bool is_checked);
#endif  // defined(PASSWORD_STORE_SELECT_ENABLED)

  password_manager::ui::State state() const { return state_; }

  const base::string16& title() const { return title_; }
  const autofill::PasswordForm& pending_password() const {
    return pending_password_;
  }

  bool are_passwords_revealed_when_bubble_is_opened() const {
    return are_passwords_revealed_when_bubble_is_opened_;
  }

#if defined(UNIT_TEST)
  void allow_passwords_revealing() {
    password_revealing_requires_reauth_ = false;
  }

  bool password_revealing_requires_reauth() const {
    return password_revealing_requires_reauth_;
  }
#endif

  bool enable_editing() const { return enable_editing_; }

  Profile* GetProfile() const;
  content::WebContents* GetWebContents() const;

  // The password bubble can switch its state between "save" and "update"
  // depending on the user input. |state_| only captures the correct state on
  // creation. This method returns true iff the current state is "update".
  bool IsCurrentStateUpdate() const;

  // Returns true iff the bubble is supposed to show the footer about syncing
  // to Google account.
  bool ShouldShowFooter() const;

  // Returns the ID of the picture to show above the title.
  int GetTopIllustration(bool dark_mode) const;

  // Returns true and updates the internal state iff the Save bubble should
  // switch to show a promotion after the password was saved. Otherwise,
  // returns false and leaves the current state.
  bool ReplaceToShowPromotionIfNeeded();

  void SetClockForTesting(base::Clock* clock);

  // Returns true if passwords revealing is not locked or re-authentication is
  // not available on the given platform. Otherwise, the method schedules
  // re-authentication and bubble reopen (the current bubble will be destroyed),
  // and returns false immediately. New bubble will reveal the passwords if the
  // re-authentication is successful.
  bool RevealPasswords();

#if defined(PASSWORD_STORE_SELECT_ENABLED)
  // Returns true iff the password account store is used.
  bool IsUsingAccountStore();
#endif  // defined(PASSWORD_STORE_SELECT_ENABLED)

 private:
  class InteractionKeeper;
  // Updates |title_| for the PENDING_PASSWORD_STATE.
  void UpdatePendingStateTitle();

  // URL of the page from where this bubble was triggered.
  GURL origin_;
  password_manager::ui::State state_;
  base::string16 title_;
  autofill::PasswordForm pending_password_;
  std::vector<autofill::PasswordForm> local_credentials_;

  // Responsible for recording all the interactions required.
  std::unique_ptr<InteractionKeeper> interaction_keeper_;

  // A bridge to ManagePasswordsUIController instance.
  base::WeakPtr<PasswordsModelDelegate> delegate_;

  // True if the model has already recorded all the necessary statistics when
  // the bubble is closing.
  bool interaction_reported_;

  // True iff password revealing should require re-auth for privacy reasons.
  bool password_revealing_requires_reauth_;

  // True iff bubble should pop up with revealed password value.
  bool are_passwords_revealed_when_bubble_is_opened_;

  // True iff username/password editing should be enabled.
  bool enable_editing_;

  // Reference to metrics recorder of the PasswordForm presented to the user by
  // |this|. We hold on to this because |delegate_| may not be able to provide
  // the reference anymore when we need it.
  scoped_refptr<password_manager::PasswordFormMetricsRecorder>
      metrics_recorder_;

  DISALLOW_COPY_AND_ASSIGN(ManagePasswordsBubbleModel);
};

#endif  // CHROME_BROWSER_UI_PASSWORDS_MANAGE_PASSWORDS_BUBBLE_MODEL_H_
