// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/passwords/manage_passwords_bubble_model.h"

#include <stddef.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "base/metrics/field_trial_params.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/default_clock.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/password_manager/password_store_factory.h"
#include "chrome/browser/password_manager/password_store_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/browser/ui/passwords/manage_passwords_view_utils.h"
#include "chrome/browser/ui/passwords/passwords_model_delegate.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "components/password_manager/core/browser/password_bubble_experiment.h"
#include "components/password_manager/core/browser/password_feature_manager.h"
#include "components/password_manager/core/browser/password_form_metrics_recorder.h"
#include "components/password_manager/core/browser/password_manager_constants.h"
#include "components/password_manager/core/browser/password_store.h"
#include "components/password_manager/core/common/password_manager_features.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/password_manager/core/common/password_manager_ui.h"
#include "components/prefs/pref_service.h"
#include "components/sync/driver/sync_service.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"

namespace metrics_util = password_manager::metrics_util;

namespace {

using Store = autofill::PasswordForm::Store;

void CleanStatisticsForSite(Profile* profile, const GURL& origin) {
  DCHECK(profile);
  password_manager::PasswordStore* password_store =
      PasswordStoreFactory::GetForProfile(profile,
                                          ServiceAccessType::IMPLICIT_ACCESS)
          .get();
  password_store->RemoveSiteStats(origin.GetOrigin());
}

std::vector<autofill::PasswordForm> DeepCopyForms(
    const std::vector<std::unique_ptr<autofill::PasswordForm>>& forms) {
  std::vector<autofill::PasswordForm> result;
  result.reserve(forms.size());
  std::transform(forms.begin(), forms.end(), std::back_inserter(result),
                 [](const std::unique_ptr<autofill::PasswordForm>& form) {
                   return *form;
                 });
  return result;
}

bool IsSyncUser(Profile* profile) {
  const syncer::SyncService* sync_service =
      ProfileSyncServiceFactory::GetForProfile(profile);
  return password_bubble_experiment::IsSmartLockUser(sync_service);
}

}  // namespace

// Class responsible for collecting and reporting all the runtime interactions
// with the bubble.
class ManagePasswordsBubbleModel::InteractionKeeper {
 public:
  InteractionKeeper(
      password_manager::InteractionsStats stats,
      password_manager::metrics_util::UIDisplayDisposition display_disposition);

  ~InteractionKeeper() = default;

  // Records UMA events, updates the interaction statistics and sends
  // notifications to the delegate when the bubble is closed.
  void ReportInteractions(const ManagePasswordsBubbleModel* model);

  void set_dismissal_reason(
      password_manager::metrics_util::UIDismissalReason reason) {
    dismissal_reason_ = reason;
  }

  void SetClockForTesting(base::Clock* clock) { clock_ = clock; }

 private:
  // The way the bubble appeared.
  const password_manager::metrics_util::UIDisplayDisposition
      display_disposition_;

  // Dismissal reason for a password bubble.
  password_manager::metrics_util::UIDismissalReason dismissal_reason_;

  // Current statistics for the save password bubble;
  password_manager::InteractionsStats interaction_stats_;

  // Used to retrieve the current time, in base::Time units.
  base::Clock* clock_;

  DISALLOW_COPY_AND_ASSIGN(InteractionKeeper);
};

ManagePasswordsBubbleModel::InteractionKeeper::InteractionKeeper(
    password_manager::InteractionsStats stats,
    password_manager::metrics_util::UIDisplayDisposition display_disposition)
    : display_disposition_(display_disposition),
      dismissal_reason_(metrics_util::NO_DIRECT_INTERACTION),
      interaction_stats_(std::move(stats)),
      clock_(base::DefaultClock::GetInstance()) {}

void ManagePasswordsBubbleModel::InteractionKeeper::ReportInteractions(
    const ManagePasswordsBubbleModel* model) {
  if (model->state() == password_manager::ui::PENDING_PASSWORD_STATE) {
    // Update the statistics for the save password bubble.
    Profile* profile = model->GetProfile();
    if (profile) {
      if (dismissal_reason_ == metrics_util::NO_DIRECT_INTERACTION &&
          display_disposition_ ==
              metrics_util::AUTOMATIC_WITH_PASSWORD_PENDING) {
        if (interaction_stats_.dismissal_count <
            std::numeric_limits<decltype(
                interaction_stats_.dismissal_count)>::max())
          interaction_stats_.dismissal_count++;
        interaction_stats_.update_time = clock_->Now();
        password_manager::PasswordStore* password_store =
            PasswordStoreFactory::GetForProfile(
                profile, ServiceAccessType::IMPLICIT_ACCESS)
                .get();
        password_store->AddSiteStats(interaction_stats_);
      }
    }
  }

  // Log UMA histograms.
  if (model->state() == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE) {
    metrics_util::LogUpdateUIDismissalReason(dismissal_reason_);
  } else if (model->state() == password_manager::ui::PENDING_PASSWORD_STATE) {
    metrics_util::LogSaveUIDismissalReason(dismissal_reason_);
  } else {
    metrics_util::LogGeneralUIDismissalReason(dismissal_reason_);
  }

  // Update the delegate so that it can send votes to the server.
  if (model->state() == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE ||
      model->state() == password_manager::ui::PENDING_PASSWORD_STATE) {
    // Send a notification if there was no interaction with the bubble.
    bool no_interaction =
        dismissal_reason_ == metrics_util::NO_DIRECT_INTERACTION;
    if (no_interaction && model->delegate_) {
      model->delegate_->OnNoInteraction();
    }
  }

  // Record UKM statistics on dismissal reason.
  if (model->metrics_recorder_)
    model->metrics_recorder_->RecordUIDismissalReason(dismissal_reason_);
}

ManagePasswordsBubbleModel::ManagePasswordsBubbleModel(
    base::WeakPtr<PasswordsModelDelegate> delegate,
    DisplayReason display_reason)
    : delegate_(std::move(delegate)),
      interaction_reported_(false),
      are_passwords_revealed_when_bubble_is_opened_(false),
      metrics_recorder_(delegate_->GetPasswordFormMetricsRecorder()) {
  origin_ = delegate_->GetOrigin();
  state_ = delegate_->GetState();
  password_manager::InteractionsStats interaction_stats;
  if (state_ == password_manager::ui::PENDING_PASSWORD_STATE ||
      state_ == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE) {
    pending_password_ = delegate_->GetPendingPassword();
    local_credentials_ = DeepCopyForms(delegate_->GetCurrentForms());
    if (state_ == password_manager::ui::PENDING_PASSWORD_STATE) {
      interaction_stats.origin_domain = origin_.GetOrigin();
      interaction_stats.username_value = pending_password_.username_value;
      const password_manager::InteractionsStats* stats =
          delegate_->GetCurrentInteractionStats();
      if (stats) {
        DCHECK_EQ(interaction_stats.username_value, stats->username_value);
        DCHECK_EQ(interaction_stats.origin_domain, stats->origin_domain);
        interaction_stats.dismissal_count = stats->dismissal_count;
      }
    }

    if (delegate_->ArePasswordsRevealedWhenBubbleIsOpened()) {
      are_passwords_revealed_when_bubble_is_opened_ = true;
      delegate_->OnPasswordsRevealed();
    }
    // The condition for the password reauth:
    // If the bubble opened after reauth -> no more reauth necessary, otherwise
    // If a password was autofilled -> require reauth to view it, otherwise
    // Require reauth iff the user opened the bubble manually and it's not the
    // manual saving state. The manual saving state as well as automatic prompt
    // are temporary states, therefore, it's better for the sake of convinience
    // for the user not to break the UX with the reauth prompt.
    password_revealing_requires_reauth_ =
        !are_passwords_revealed_when_bubble_is_opened_ &&
        (pending_password_.form_has_autofilled_value ||
         (!delegate_->BubbleIsManualFallbackForSaving() &&
          display_reason == USER_ACTION));
    enable_editing_ = delegate_->GetCredentialSource() !=
                      password_manager::metrics_util::CredentialSourceType::
                          kCredentialManagementAPI;

    UpdatePendingStateTitle();
  }

  password_manager::metrics_util::UIDisplayDisposition display_disposition =
      metrics_util::AUTOMATIC_WITH_PASSWORD_PENDING;
  if (display_reason == USER_ACTION) {
    switch (state_) {
      case password_manager::ui::PENDING_PASSWORD_STATE:
        display_disposition = metrics_util::MANUAL_WITH_PASSWORD_PENDING;
        break;
      case password_manager::ui::PENDING_PASSWORD_UPDATE_STATE:
        display_disposition = metrics_util::MANUAL_WITH_PASSWORD_PENDING_UPDATE;
        break;
      case password_manager::ui::MANAGE_STATE:
      case password_manager::ui::CONFIRMATION_STATE:
      case password_manager::ui::CREDENTIAL_REQUEST_STATE:
      case password_manager::ui::AUTO_SIGNIN_STATE:
      case password_manager::ui::CHROME_SIGN_IN_PROMO_STATE:
      case password_manager::ui::INACTIVE_STATE:
        NOTREACHED();
        break;
    }
  } else {
    switch (state_) {
      case password_manager::ui::PENDING_PASSWORD_STATE:
        display_disposition = metrics_util::AUTOMATIC_WITH_PASSWORD_PENDING;
        break;
      case password_manager::ui::PENDING_PASSWORD_UPDATE_STATE:
        display_disposition =
            metrics_util::AUTOMATIC_WITH_PASSWORD_PENDING_UPDATE;
        break;
      case password_manager::ui::CONFIRMATION_STATE:
      case password_manager::ui::AUTO_SIGNIN_STATE:
      case password_manager::ui::MANAGE_STATE:
      case password_manager::ui::CREDENTIAL_REQUEST_STATE:
      case password_manager::ui::CHROME_SIGN_IN_PROMO_STATE:
      case password_manager::ui::INACTIVE_STATE:
        NOTREACHED();
        break;
    }
  }

  if (metrics_recorder_) {
    metrics_recorder_->RecordPasswordBubbleShown(
        delegate_->GetCredentialSource(), display_disposition);
  }
  metrics_util::LogUIDisplayDisposition(display_disposition);
  interaction_keeper_ = std::make_unique<InteractionKeeper>(
      std::move(interaction_stats), display_disposition);

  delegate_->OnBubbleShown();
}

ManagePasswordsBubbleModel::~ManagePasswordsBubbleModel() {
  if (!interaction_reported_)
    OnBubbleClosing();
}

void ManagePasswordsBubbleModel::OnBubbleClosing() {
  interaction_keeper_->ReportInteractions(this);
  if (delegate_)
    delegate_->OnBubbleHidden();
  delegate_.reset();
  interaction_reported_ = true;
}

void ManagePasswordsBubbleModel::OnNopeUpdateClicked() {
  DCHECK_EQ(password_manager::ui::PENDING_PASSWORD_UPDATE_STATE, state_);
  interaction_keeper_->set_dismissal_reason(metrics_util::CLICKED_CANCEL);
  if (delegate_)
    delegate_->OnNopeUpdateClicked();
}

void ManagePasswordsBubbleModel::OnNeverForThisSiteClicked() {
  DCHECK_EQ(password_manager::ui::PENDING_PASSWORD_STATE, state_);
  interaction_keeper_->set_dismissal_reason(metrics_util::CLICKED_NEVER);
  if (delegate_) {
    CleanStatisticsForSite(GetProfile(), origin_);
    delegate_->NeverSavePassword();
  }
}

void ManagePasswordsBubbleModel::OnCredentialEdited(
    base::string16 new_username,
    base::string16 new_password) {
  DCHECK(state_ == password_manager::ui::PENDING_PASSWORD_STATE ||
         state_ == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE);
  pending_password_.username_value = std::move(new_username);
  pending_password_.password_value = std::move(new_password);
}

void ManagePasswordsBubbleModel::OnSaveClicked() {
  DCHECK(state_ == password_manager::ui::PENDING_PASSWORD_STATE ||
         state_ == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE);
  interaction_keeper_->set_dismissal_reason(metrics_util::CLICKED_SAVE);
  if (delegate_) {
    CleanStatisticsForSite(GetProfile(), origin_);
    delegate_->SavePassword(pending_password_.username_value,
                            pending_password_.password_value);
  }
}

#if defined(PASSWORD_STORE_SELECT_ENABLED)
void ManagePasswordsBubbleModel::OnToggleAccountStore(bool is_checked) {
  delegate_->GetPasswordFeatureManager()->SetDefaultPasswordStore(
      is_checked ? Store::kAccountStore : Store::kProfileStore);
}
#endif  // defined(PASSWORD_STORE_SELECT_ENABLED)

Profile* ManagePasswordsBubbleModel::GetProfile() const {
  content::WebContents* web_contents = GetWebContents();
  if (!web_contents)
    return nullptr;
  return Profile::FromBrowserContext(web_contents->GetBrowserContext());
}

content::WebContents* ManagePasswordsBubbleModel::GetWebContents() const {
  return delegate_ ? delegate_->GetWebContents() : nullptr;
}

bool ManagePasswordsBubbleModel::IsCurrentStateUpdate() const {
  DCHECK(state_ == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE ||
         state_ == password_manager::ui::PENDING_PASSWORD_STATE);
  return std::any_of(local_credentials_.begin(), local_credentials_.end(),
                     [this](const autofill::PasswordForm& form) {
                       return form.username_value ==
                              pending_password_.username_value;
                     });
}

bool ManagePasswordsBubbleModel::ShouldShowFooter() const {
  return (state_ == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE ||
          state_ == password_manager::ui::PENDING_PASSWORD_STATE) &&
         IsSyncUser(GetProfile());
}

int ManagePasswordsBubbleModel::GetTopIllustration(bool dark_mode) const {
  if (state_ == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE ||
      state_ == password_manager::ui::PENDING_PASSWORD_STATE) {
    int image = base::GetFieldTrialParamByFeatureAsInt(
        password_manager::features::kPasswordSaveIllustration, "image", 0);
    switch (image) {
      case 1:
        return dark_mode ? IDR_SAVE_PASSWORD1_DARK : IDR_SAVE_PASSWORD1;
      case 2:
        return dark_mode ? IDR_SAVE_PASSWORD2_DARK : IDR_SAVE_PASSWORD2;
      case 3:
        return dark_mode ? IDR_SAVE_PASSWORD3_DARK : IDR_SAVE_PASSWORD3;
      default:
        return 0;
    }
  }
  return 0;
}

bool ManagePasswordsBubbleModel::ReplaceToShowPromotionIfNeeded() {
  Profile* profile = GetProfile();
  if (!profile)
    return false;
  PrefService* prefs = profile->GetPrefs();
  const syncer::SyncService* sync_service =
      ProfileSyncServiceFactory::GetForProfile(profile);
  // Signin promotion.
  if (password_bubble_experiment::ShouldShowChromeSignInPasswordPromo(
          prefs, sync_service)) {
    interaction_keeper_->ReportInteractions(this);
    title_ = l10n_util::GetStringUTF16(IDS_PASSWORD_MANAGER_SYNC_PROMO_TITLE);
    state_ = password_manager::ui::CHROME_SIGN_IN_PROMO_STATE;
    int show_count = prefs->GetInteger(
        password_manager::prefs::kNumberSignInPasswordPromoShown);
    show_count++;
    prefs->SetInteger(password_manager::prefs::kNumberSignInPasswordPromoShown,
                      show_count);
    return true;
  }
  return false;
}

void ManagePasswordsBubbleModel::SetClockForTesting(base::Clock* clock) {
  interaction_keeper_->SetClockForTesting(clock);
}

bool ManagePasswordsBubbleModel::RevealPasswords() {
  bool reveal_immediately = !password_revealing_requires_reauth_ ||
                            (delegate_ && delegate_->AuthenticateUser());
  if (reveal_immediately)
    delegate_->OnPasswordsRevealed();
  return reveal_immediately;
}

#if defined(PASSWORD_STORE_SELECT_ENABLED)
bool ManagePasswordsBubbleModel::IsUsingAccountStore() {
  return delegate_->GetPasswordFeatureManager()->GetDefaultPasswordStore() ==
         Store::kAccountStore;
}
#endif  // defined(PASSWORD_STORE_SELECT_ENABLED)

void ManagePasswordsBubbleModel::UpdatePendingStateTitle() {
  PasswordTitleType type =
      state_ == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE
          ? PasswordTitleType::UPDATE_PASSWORD
          : (pending_password_.federation_origin.opaque()
                 ? PasswordTitleType::SAVE_PASSWORD
                 : PasswordTitleType::SAVE_ACCOUNT);
  GetSavePasswordDialogTitleTextAndLinkRange(GetWebContents()->GetVisibleURL(),
                                             origin_, type, &title_);
}
