// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/passwords/manage_passwords_bubble_model.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/metrics/histogram_samples.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/simple_test_clock.h"
#include "build/build_config.h"
#include "chrome/browser/password_manager/password_store_factory.h"
#include "chrome/browser/ui/passwords/passwords_model_delegate_mock.h"
#include "chrome/test/base/testing_profile.h"
#include "components/password_manager/core/browser/mock_password_store.h"
#include "components/password_manager/core/browser/password_form_metrics_recorder.h"
#include "components/password_manager/core/browser/password_manager_metrics_util.h"
#include "components/password_manager/core/browser/password_manager_test_utils.h"
#include "components/password_manager/core/browser/statistics_table.h"
#include "components/password_manager/core/common/credential_manager_types.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/password_manager/core/common/password_manager_ui.h"
#include "components/prefs/pref_service.h"
#include "components/signin/public/identity_manager/account_info.h"
#include "components/ukm/test_ukm_recorder.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "services/metrics/public/cpp/ukm_recorder.h"
#include "services/metrics/public/cpp/ukm_source.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::_;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

constexpr ukm::SourceId kTestSourceId = 0x1234;

constexpr char kSiteOrigin[] = "http://example.com/login";
constexpr char kUsername[] = "Admin";
constexpr char kUsernameExisting[] = "User";
constexpr char kUsernameNew[] = "User585";
constexpr char kPassword[] = "AdminPass";
constexpr char kPasswordEdited[] = "asDfjkl;";
constexpr char kUIDismissalReasonGeneralMetric[] =
    "PasswordManager.UIDismissalReason";
constexpr char kUIDismissalReasonSaveMetric[] =
    "PasswordManager.SaveUIDismissalReason";
constexpr char kUIDismissalReasonUpdateMetric[] =
    "PasswordManager.UpdateUIDismissalReason";

}  // namespace

class ManagePasswordsBubbleModelTest : public ::testing::Test {
 public:
  ManagePasswordsBubbleModelTest() = default;
  ~ManagePasswordsBubbleModelTest() override = default;

  void SetUp() override {
    test_web_contents_ =
        content::WebContentsTester::CreateTestWebContents(&profile_, nullptr);
    mock_delegate_ =
        std::make_unique<testing::NiceMock<PasswordsModelDelegateMock>>();
    ON_CALL(*mock_delegate_, GetPasswordFormMetricsRecorder())
        .WillByDefault(Return(nullptr));
    PasswordStoreFactory::GetInstance()->SetTestingFactoryAndUse(
        profile(),
        base::BindRepeating(
            &password_manager::BuildPasswordStore<
                content::BrowserContext,
                testing::StrictMock<password_manager::MockPasswordStore>>));
    pending_password_.origin = GURL(kSiteOrigin);
    pending_password_.signon_realm = kSiteOrigin;
    pending_password_.username_value = base::ASCIIToUTF16(kUsername);
    pending_password_.password_value = base::ASCIIToUTF16(kPassword);
  }

  void TearDown() override {
    // Reset the delegate first. It can happen if the user closes the tab.
    mock_delegate_.reset();
    model_.reset();
  }

  PrefService* prefs() { return profile_.GetPrefs(); }

  TestingProfile* profile() { return &profile_; }

  password_manager::MockPasswordStore* GetStore() {
    return static_cast<password_manager::MockPasswordStore*>(
        PasswordStoreFactory::GetInstance()
            ->GetForProfile(profile(), ServiceAccessType::EXPLICIT_ACCESS)
            .get());
  }

  PasswordsModelDelegateMock* controller() { return mock_delegate_.get(); }

  ManagePasswordsBubbleModel* model() { return model_.get(); }

  autofill::PasswordForm& pending_password() { return pending_password_; }
  const autofill::PasswordForm& pending_password() const {
    return pending_password_;
  }

  void SetUpWithState(password_manager::ui::State state,
                      ManagePasswordsBubbleModel::DisplayReason reason);
  void PretendPasswordWaiting(ManagePasswordsBubbleModel::DisplayReason reason =
                                  ManagePasswordsBubbleModel::AUTOMATIC);
  void PretendUpdatePasswordWaiting();

  void DestroyModelAndVerifyControllerExpectations();
  void DestroyModelExpectReason(
      password_manager::metrics_util::UIDismissalReason dismissal_reason);

  static password_manager::InteractionsStats GetTestStats();
  std::vector<std::unique_ptr<autofill::PasswordForm>> GetCurrentForms() const;

 private:
  content::BrowserTaskEnvironment task_environment_;
  content::RenderViewHostTestEnabler rvh_enabler_;
  TestingProfile profile_;
  std::unique_ptr<content::WebContents> test_web_contents_;
  std::unique_ptr<ManagePasswordsBubbleModel> model_;
  std::unique_ptr<PasswordsModelDelegateMock> mock_delegate_;
  autofill::PasswordForm pending_password_;
};

void ManagePasswordsBubbleModelTest::SetUpWithState(
    password_manager::ui::State state,
    ManagePasswordsBubbleModel::DisplayReason reason) {
  GURL origin(kSiteOrigin);
  EXPECT_CALL(*controller(), GetOrigin()).WillOnce(ReturnRef(origin));
  EXPECT_CALL(*controller(), GetState()).WillOnce(Return(state));
  EXPECT_CALL(*controller(), OnBubbleShown());
  EXPECT_CALL(*controller(), GetWebContents())
      .WillRepeatedly(Return(test_web_contents_.get()));
  model_.reset(
      new ManagePasswordsBubbleModel(mock_delegate_->AsWeakPtr(), reason));
  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(controller()));
  EXPECT_CALL(*controller(), GetWebContents())
      .WillRepeatedly(Return(test_web_contents_.get()));
}

void ManagePasswordsBubbleModelTest::PretendPasswordWaiting(
    ManagePasswordsBubbleModel::DisplayReason reason) {
  EXPECT_CALL(*controller(), GetPendingPassword())
      .WillOnce(ReturnRef(pending_password()));
  password_manager::InteractionsStats stats = GetTestStats();
  EXPECT_CALL(*controller(), GetCurrentInteractionStats())
      .WillOnce(Return(&stats));
  std::vector<std::unique_ptr<autofill::PasswordForm>> forms =
      GetCurrentForms();
  EXPECT_CALL(*controller(), GetCurrentForms()).WillOnce(ReturnRef(forms));
  SetUpWithState(password_manager::ui::PENDING_PASSWORD_STATE, reason);
}

void ManagePasswordsBubbleModelTest::PretendUpdatePasswordWaiting() {
  EXPECT_CALL(*controller(), GetPendingPassword())
      .WillOnce(ReturnRef(pending_password()));
  std::vector<std::unique_ptr<autofill::PasswordForm>> forms =
      GetCurrentForms();
  auto current_form =
      std::make_unique<autofill::PasswordForm>(pending_password());
  current_form->password_value = base::ASCIIToUTF16("old_password");
  forms.push_back(std::move(current_form));
  EXPECT_CALL(*controller(), GetCurrentForms()).WillOnce(ReturnRef(forms));
  SetUpWithState(password_manager::ui::PENDING_PASSWORD_UPDATE_STATE,
                 ManagePasswordsBubbleModel::AUTOMATIC);
}

void ManagePasswordsBubbleModelTest::
    DestroyModelAndVerifyControllerExpectations() {
  EXPECT_CALL(*controller(), OnBubbleHidden());
  model_->OnBubbleClosing();
  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(controller()));
  model_.reset();
}

void ManagePasswordsBubbleModelTest::DestroyModelExpectReason(
    password_manager::metrics_util::UIDismissalReason dismissal_reason) {
  base::HistogramTester histogram_tester;
  password_manager::ui::State state = model_->state();
  std::string histogram(kUIDismissalReasonGeneralMetric);
  if (state == password_manager::ui::PENDING_PASSWORD_STATE)
    histogram = kUIDismissalReasonSaveMetric;
  else if (state == password_manager::ui::PENDING_PASSWORD_UPDATE_STATE)
    histogram = kUIDismissalReasonUpdateMetric;
  DestroyModelAndVerifyControllerExpectations();
  histogram_tester.ExpectUniqueSample(histogram, dismissal_reason, 1);
}

// static
password_manager::InteractionsStats
ManagePasswordsBubbleModelTest::GetTestStats() {
  password_manager::InteractionsStats result;
  result.origin_domain = GURL(kSiteOrigin).GetOrigin();
  result.username_value = base::ASCIIToUTF16(kUsername);
  result.dismissal_count = 5;
  result.update_time = base::Time::FromTimeT(1);
  return result;
}

std::vector<std::unique_ptr<autofill::PasswordForm>>
ManagePasswordsBubbleModelTest::GetCurrentForms() const {
  autofill::PasswordForm form(pending_password());
  form.username_value = base::ASCIIToUTF16(kUsernameExisting);
  form.password_value = base::ASCIIToUTF16("123456");

  autofill::PasswordForm preferred_form(pending_password());
  preferred_form.username_value = base::ASCIIToUTF16("preferred_username");
  preferred_form.password_value = base::ASCIIToUTF16("654321");

  std::vector<std::unique_ptr<autofill::PasswordForm>> forms;
  forms.push_back(std::make_unique<autofill::PasswordForm>(form));
  forms.push_back(std::make_unique<autofill::PasswordForm>(preferred_form));
  return forms;
}

TEST_F(ManagePasswordsBubbleModelTest, CloseWithoutInteraction) {
  PretendPasswordWaiting();

  EXPECT_EQ(password_manager::ui::PENDING_PASSWORD_STATE, model()->state());
  base::SimpleTestClock clock;
  base::Time now = base::Time::Now();
  clock.SetNow(now);
  model()->SetClockForTesting(&clock);
  password_manager::InteractionsStats stats = GetTestStats();
  stats.dismissal_count++;
  stats.update_time = now;
  EXPECT_CALL(*GetStore(), AddSiteStatsImpl(stats));
  EXPECT_CALL(*controller(), OnNoInteraction());
  EXPECT_CALL(*controller(), SavePassword(_, _)).Times(0);
  EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
  DestroyModelExpectReason(
      password_manager::metrics_util::NO_DIRECT_INTERACTION);
}

TEST_F(ManagePasswordsBubbleModelTest, ClickSave) {
  PretendPasswordWaiting();

  EXPECT_TRUE(model()->enable_editing());
  EXPECT_FALSE(model()->IsCurrentStateUpdate());

  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), OnPasswordsRevealed()).Times(0);
  EXPECT_CALL(*controller(), SavePassword(pending_password().username_value,
                                          pending_password().password_value));
  EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
  EXPECT_CALL(*controller(), OnNopeUpdateClicked()).Times(0);
  model()->OnSaveClicked();
  DestroyModelExpectReason(password_manager::metrics_util::CLICKED_SAVE);
}

TEST_F(ManagePasswordsBubbleModelTest, ClickSaveInUpdateState) {
  PretendUpdatePasswordWaiting();

  // Edit username, now it's a new credential.
  model()->OnCredentialEdited(base::ASCIIToUTF16(kUsernameNew),
                              base::ASCIIToUTF16(kPasswordEdited));
  EXPECT_FALSE(model()->IsCurrentStateUpdate());

  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), SavePassword(base::ASCIIToUTF16(kUsernameNew),
                                          base::ASCIIToUTF16(kPasswordEdited)));
  EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
  EXPECT_CALL(*controller(), OnNopeUpdateClicked()).Times(0);
  model()->OnSaveClicked();
  DestroyModelExpectReason(password_manager::metrics_util::CLICKED_SAVE);
}

TEST_F(ManagePasswordsBubbleModelTest, ClickNever) {
  PretendPasswordWaiting();

  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), SavePassword(_, _)).Times(0);
  EXPECT_CALL(*controller(), NeverSavePassword());
  model()->OnNeverForThisSiteClicked();
  EXPECT_EQ(password_manager::ui::PENDING_PASSWORD_STATE, model()->state());
  DestroyModelExpectReason(password_manager::metrics_util::CLICKED_NEVER);
}

TEST_F(ManagePasswordsBubbleModelTest, ClickUpdate) {
  PretendUpdatePasswordWaiting();

  EXPECT_TRUE(model()->enable_editing());
  EXPECT_TRUE(model()->IsCurrentStateUpdate());

  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), OnPasswordsRevealed()).Times(0);
  EXPECT_CALL(*controller(), SavePassword(pending_password().username_value,
                                          pending_password().password_value));
  EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
  EXPECT_CALL(*controller(), OnNopeUpdateClicked()).Times(0);
  model()->OnSaveClicked();
  DestroyModelExpectReason(password_manager::metrics_util::CLICKED_SAVE);
}

TEST_F(ManagePasswordsBubbleModelTest, ClickUpdateInSaveState) {
  PretendPasswordWaiting();

  // Edit username, now it's an existing credential.
  model()->OnCredentialEdited(base::ASCIIToUTF16(kUsernameExisting),
                              base::ASCIIToUTF16(kPasswordEdited));
  EXPECT_TRUE(model()->IsCurrentStateUpdate());

  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), SavePassword(base::ASCIIToUTF16(kUsernameExisting),
                                          base::ASCIIToUTF16(kPasswordEdited)));
  EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
  EXPECT_CALL(*controller(), OnNopeUpdateClicked()).Times(0);
  model()->OnSaveClicked();
  DestroyModelExpectReason(password_manager::metrics_util::CLICKED_SAVE);
}

TEST_F(ManagePasswordsBubbleModelTest, GetInitialUsername_MatchedUsername) {
  PretendUpdatePasswordWaiting();
  EXPECT_EQ(base::UTF8ToUTF16(kUsername),
            model()->pending_password().username_value);
}

TEST_F(ManagePasswordsBubbleModelTest, EditCredential) {
  PretendPasswordWaiting();
  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));

  const base::string16 kExpectedUsername = base::UTF8ToUTF16("new_username");
  const base::string16 kExpectedPassword = base::UTF8ToUTF16("new_password");

  model()->OnCredentialEdited(kExpectedUsername, kExpectedPassword);
  EXPECT_EQ(kExpectedUsername, model()->pending_password().username_value);
  EXPECT_EQ(kExpectedPassword, model()->pending_password().password_value);
  EXPECT_CALL(*controller(),
              SavePassword(kExpectedUsername, kExpectedPassword));
  EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
  model()->OnSaveClicked();
  DestroyModelAndVerifyControllerExpectations();
}

TEST_F(ManagePasswordsBubbleModelTest, SuppressSignInPromo) {
  prefs()->SetBoolean(password_manager::prefs::kSignInPasswordPromoRevive,
                      true);
  prefs()->SetBoolean(password_manager::prefs::kWasSignInPasswordPromoClicked,
                      true);
  PretendPasswordWaiting();
  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), SavePassword(pending_password().username_value,
                                          pending_password().password_value));
  model()->OnSaveClicked();

  EXPECT_FALSE(model()->ReplaceToShowPromotionIfNeeded());
  DestroyModelAndVerifyControllerExpectations();
}

TEST_F(ManagePasswordsBubbleModelTest, SignInPromoOK) {
  base::HistogramTester histogram_tester;
  PretendPasswordWaiting();
  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), SavePassword(pending_password().username_value,
                                          pending_password().password_value));
  model()->OnSaveClicked();

#if defined(OS_CHROMEOS)
  EXPECT_FALSE(model()->ReplaceToShowPromotionIfNeeded());
#else
  EXPECT_TRUE(model()->ReplaceToShowPromotionIfNeeded());
#endif
}

#if !defined(OS_CHROMEOS)
TEST_F(ManagePasswordsBubbleModelTest, SignInPromoCancel) {
  base::HistogramTester histogram_tester;
  PretendPasswordWaiting();
  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), SavePassword(pending_password().username_value,
                                          pending_password().password_value));
  model()->OnSaveClicked();

  EXPECT_TRUE(model()->ReplaceToShowPromotionIfNeeded());
  DestroyModelAndVerifyControllerExpectations();
  histogram_tester.ExpectUniqueSample(
      kUIDismissalReasonSaveMetric,
      password_manager::metrics_util::CLICKED_SAVE, 1);
}

TEST_F(ManagePasswordsBubbleModelTest, SignInPromoDismiss) {
  base::HistogramTester histogram_tester;
  PretendPasswordWaiting();
  EXPECT_CALL(*GetStore(), RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
  EXPECT_CALL(*controller(), SavePassword(pending_password().username_value,
                                          pending_password().password_value));
  model()->OnSaveClicked();

  EXPECT_TRUE(model()->ReplaceToShowPromotionIfNeeded());
  DestroyModelAndVerifyControllerExpectations();
  histogram_tester.ExpectUniqueSample(
      kUIDismissalReasonSaveMetric,
      password_manager::metrics_util::CLICKED_SAVE, 1);
  EXPECT_FALSE(prefs()->GetBoolean(
      password_manager::prefs::kWasSignInPasswordPromoClicked));
}
#endif  // !defined(OS_CHROMEOS)

// Verify that URL keyed metrics are properly recorded.
TEST_F(ManagePasswordsBubbleModelTest, RecordUKMs) {
  using BubbleDismissalReason =
      password_manager::PasswordFormMetricsRecorder::BubbleDismissalReason;
  using BubbleTrigger =
      password_manager::PasswordFormMetricsRecorder::BubbleTrigger;
  using password_manager::metrics_util::CredentialSourceType;
  using UkmEntry = ukm::builders::PasswordForm;

  // |credential_management_api| defines whether credentials originate from the
  // credential management API.
  for (const bool credential_management_api : {false, true}) {
    // |update| defines whether this is an update or a save bubble.
    for (const bool update : {false, true}) {
      for (const auto interaction :
           {BubbleDismissalReason::kAccepted, BubbleDismissalReason::kDeclined,
            BubbleDismissalReason::kIgnored}) {
        SCOPED_TRACE(testing::Message()
                     << "update = " << update
                     << ", interaction = " << static_cast<int64_t>(interaction)
                     << ", credential management api ="
                     << credential_management_api);
        ukm::TestAutoSetUkmRecorder test_ukm_recorder;
        {
          // Setup metrics recorder
          auto recorder = base::MakeRefCounted<
              password_manager::PasswordFormMetricsRecorder>(
              true /*is_main_frame_secure*/, kTestSourceId);

          // Exercise bubble.
          ON_CALL(*controller(), GetPasswordFormMetricsRecorder())
              .WillByDefault(Return(recorder.get()));
          ON_CALL(*controller(), GetCredentialSource())
              .WillByDefault(
                  Return(credential_management_api
                             ? CredentialSourceType::kCredentialManagementAPI
                             : CredentialSourceType::kPasswordManager));

          if (update)
            PretendUpdatePasswordWaiting();
          else
            PretendPasswordWaiting();

          if (interaction == BubbleDismissalReason::kAccepted) {
            EXPECT_CALL(*GetStore(),
                        RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
            EXPECT_CALL(*controller(),
                        SavePassword(pending_password().username_value,
                                     pending_password().password_value));
            model()->OnSaveClicked();
          } else if (interaction == BubbleDismissalReason::kDeclined &&
                     update) {
            EXPECT_CALL(*controller(), SavePassword(_, _)).Times(0);
            model()->OnNopeUpdateClicked();
          } else if (interaction == BubbleDismissalReason::kDeclined &&
                     !update) {
            EXPECT_CALL(*GetStore(),
                        RemoveSiteStatsImpl(GURL(kSiteOrigin).GetOrigin()));
            EXPECT_CALL(*controller(), SavePassword(_, _)).Times(0);
            EXPECT_CALL(*controller(), NeverSavePassword());
            model()->OnNeverForThisSiteClicked();
          } else if (interaction == BubbleDismissalReason::kIgnored && update) {
            EXPECT_CALL(*controller(), SavePassword(_, _)).Times(0);
            EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
          } else if (interaction == BubbleDismissalReason::kIgnored &&
                     !update) {
            EXPECT_CALL(*GetStore(), AddSiteStatsImpl(testing::_));
            EXPECT_CALL(*controller(), OnNoInteraction());
            EXPECT_CALL(*controller(), SavePassword(_, _)).Times(0);
            EXPECT_CALL(*controller(), NeverSavePassword()).Times(0);
          } else {
            NOTREACHED();
          }
          DestroyModelAndVerifyControllerExpectations();
        }

        ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(controller()));
        // Flush async calls on password store.
        base::RunLoop().RunUntilIdle();
        ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(GetStore()));

        // Verify metrics.
        const auto& entries =
            test_ukm_recorder.GetEntriesByName(UkmEntry::kEntryName);
        EXPECT_EQ(1u, entries.size());
        for (const auto* entry : entries) {
          EXPECT_EQ(kTestSourceId, entry->source_id);
          test_ukm_recorder.ExpectEntryMetric(
              entry,
              update ? UkmEntry::kUpdating_Prompt_ShownName
                     : UkmEntry::kSaving_Prompt_ShownName,
              1);
          test_ukm_recorder.ExpectEntryMetric(
              entry,
              update ? UkmEntry::kUpdating_Prompt_TriggerName
                     : UkmEntry::kSaving_Prompt_TriggerName,
              static_cast<int64_t>(
                  credential_management_api
                      ? BubbleTrigger::kCredentialManagementAPIAutomatic
                      : BubbleTrigger::kPasswordManagerSuggestionAutomatic));
          test_ukm_recorder.ExpectEntryMetric(
              entry,
              update ? UkmEntry::kUpdating_Prompt_InteractionName
                     : UkmEntry::kSaving_Prompt_InteractionName,
              static_cast<int64_t>(interaction));
        }
      }
    }
  }
}

TEST_F(ManagePasswordsBubbleModelTest, EyeIcon_ReauthForPasswordsRevealing) {
  for (bool is_manual_fallback_for_saving : {false, true}) {
    for (bool form_has_autofilled_value : {false, true}) {
      for (ManagePasswordsBubbleModel::DisplayReason display_reason :
           {ManagePasswordsBubbleModel::AUTOMATIC,
            ManagePasswordsBubbleModel::USER_ACTION}) {
        // That state is impossible.
        if (is_manual_fallback_for_saving &&
            (display_reason == ManagePasswordsBubbleModel::AUTOMATIC))
          continue;

        SCOPED_TRACE(testing::Message()
                     << "is_manual_fallback_for_saving = "
                     << is_manual_fallback_for_saving
                     << " form_has_autofilled_value = "
                     << form_has_autofilled_value << " display_reason = "
                     << (display_reason == ManagePasswordsBubbleModel::AUTOMATIC
                             ? "AUTOMATIC"
                             : "USER_ACTION"));

        pending_password().form_has_autofilled_value =
            form_has_autofilled_value;
        EXPECT_CALL(*controller(), ArePasswordsRevealedWhenBubbleIsOpened())
            .WillOnce(Return(false));
        EXPECT_CALL(*controller(), BubbleIsManualFallbackForSaving())
            .WillRepeatedly(Return(is_manual_fallback_for_saving));

        PretendPasswordWaiting(display_reason);
        bool reauth_expected = form_has_autofilled_value;
        if (!reauth_expected) {
          reauth_expected =
              !is_manual_fallback_for_saving &&
              display_reason == ManagePasswordsBubbleModel::USER_ACTION;
        }
        EXPECT_EQ(reauth_expected,
                  model()->password_revealing_requires_reauth());

        if (reauth_expected) {
          EXPECT_CALL(*controller(), AuthenticateUser())
              .WillOnce(Return(false));
          EXPECT_FALSE(model()->RevealPasswords());

          EXPECT_CALL(*controller(), AuthenticateUser()).WillOnce(Return(true));
          EXPECT_TRUE(model()->RevealPasswords());
        } else {
          EXPECT_TRUE(model()->RevealPasswords());
        }

        if (display_reason == ManagePasswordsBubbleModel::AUTOMATIC)
          EXPECT_CALL(*GetStore(), AddSiteStatsImpl(_));

        DestroyModelAndVerifyControllerExpectations();
        // Flush async calls on password store.
        base::RunLoop().RunUntilIdle();
        ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(GetStore()));
      }
    }
  }
}

TEST_F(ManagePasswordsBubbleModelTest, EyeIcon_BubbleReopenedAfterAuth) {
  // Checks re-authentication is not needed if the bubble is opened right after
  // successful authentication.
  pending_password().form_has_autofilled_value = true;
  // After successful authentication this value is set to true.
  EXPECT_CALL(*controller(), ArePasswordsRevealedWhenBubbleIsOpened())
      .WillOnce(Return(true));
  PretendPasswordWaiting(ManagePasswordsBubbleModel::USER_ACTION);

  EXPECT_FALSE(model()->password_revealing_requires_reauth());
  EXPECT_TRUE(model()->RevealPasswords());
}

TEST_F(ManagePasswordsBubbleModelTest, PasswordsRevealedReported) {
  PretendPasswordWaiting();

  EXPECT_CALL(*controller(), OnPasswordsRevealed());
  EXPECT_TRUE(model()->RevealPasswords());
}

TEST_F(ManagePasswordsBubbleModelTest, PasswordsRevealedReportedAfterReauth) {
  // The bubble is opened after reauthentication and the passwords are revealed.
  pending_password().form_has_autofilled_value = true;
  // After successful authentication this value is set to true.
  EXPECT_CALL(*controller(), ArePasswordsRevealedWhenBubbleIsOpened())
      .WillOnce(Return(true));
  EXPECT_CALL(*controller(), OnPasswordsRevealed());
  PretendPasswordWaiting(ManagePasswordsBubbleModel::USER_ACTION);
}

TEST_F(ManagePasswordsBubbleModelTest, DisableEditing) {
  EXPECT_CALL(*controller(), BubbleIsManualFallbackForSaving())
      .WillRepeatedly(Return(false));
  EXPECT_CALL(*controller(), GetCredentialSource())
      .WillOnce(Return(password_manager::metrics_util::CredentialSourceType::
                           kCredentialManagementAPI));
  PretendPasswordWaiting();
  EXPECT_FALSE(model()->enable_editing());
}
