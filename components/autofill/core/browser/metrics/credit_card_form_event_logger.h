// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_CORE_BROWSER_METRICS_CREDIT_CARD_FORM_EVENT_LOGGER_H_
#define COMPONENTS_AUTOFILL_CORE_BROWSER_METRICS_CREDIT_CARD_FORM_EVENT_LOGGER_H_

#include <string>

#include "components/autofill/core/browser/autofill_client.h"
#include "components/autofill/core/browser/autofill_field.h"
#include "components/autofill/core/browser/autofill_metrics.h"
#include "components/autofill/core/browser/data_model/credit_card.h"
#include "components/autofill/core/browser/form_structure.h"
#include "components/autofill/core/browser/metrics/form_event_logger_base.h"
#include "components/autofill/core/browser/metrics/form_events.h"
#include "components/autofill/core/browser/personal_data_manager.h"
#include "components/autofill/core/browser/sync_utils.h"
#include "components/autofill/core/common/signatures_util.h"

namespace autofill {

class CreditCardFormEventLogger : public FormEventLoggerBase {
 public:
  // Metric for tracking which card unmask authentication method was used.
  enum class UnmaskAuthFlowType {
    kNone = 0,
    // Only CVC prompt was shown.
    kCvc = 1,
    // Only WebAuthn prompt was shown.
    kFido = 2,
    // CVC authentication was required in addition to WebAuthn.
    kCvcThenFido = 3,
    // WebAuthn prompt failed and fell back to CVC prompt.
    kCvcFallbackFromFido = 4,
  };
  enum class UnmaskAuthFlowEvent {
    // Authentication prompt is shown.
    kPromptShown = 0,
    // Authentication prompt successfully completed.
    kPromptCompleted = 1,
    // Form was submitted.
    kFormSubmitted = 2,
    kMaxValue = kFormSubmitted,
  };

  CreditCardFormEventLogger(
      bool is_in_main_frame,
      AutofillMetrics::FormInteractionsUkmLogger* form_interactions_ukm_logger,
      PersonalDataManager* personal_data_manager,
      AutofillClient* client);

  ~CreditCardFormEventLogger() override;

  inline void set_is_context_secure(bool is_context_secure) {
    is_context_secure_ = is_context_secure;
  }

  void OnDidSelectCardSuggestion(const CreditCard& credit_card,
                                 const FormStructure& form,
                                 AutofillSyncSigninState sync_state);

  // In case of masked cards, caller must make sure this gets called before
  // the card is upgraded to a full card.
  void OnDidFillSuggestion(const CreditCard& credit_card,
                           const FormStructure& form,
                           const AutofillField& field,
                           AutofillSyncSigninState sync_state);

  // Logging what type of authentication flow was prompted.
  void LogCardUnmaskAuthenticationPromptShown(UnmaskAuthFlowType flow);

  // Logging when an authentication prompt is completed.
  void LogCardUnmaskAuthenticationPromptCompleted(UnmaskAuthFlowType flow);

 protected:
  // FormEventLoggerBase pure-virtual overrides.
  void RecordPollSuggestions() override;
  void RecordParseForm() override;
  void RecordShowSuggestions() override;

  // FormEventLoggerBase virtual overrides.
  void LogWillSubmitForm(const FormStructure& form) override;
  void LogFormSubmitted(const FormStructure& form) override;
  void LogUkmInteractedWithForm(FormSignature form_signature) override;
  void OnSuggestionsShownOnce() override;
  void OnSuggestionsShownSubmittedOnce(const FormStructure& form) override;
  void OnLog(const std::string& name,
             FormEvent event,
             const FormStructure& form) const override;

  // Bringing base class' Log function into scope to allow overloading.
  using FormEventLoggerBase::Log;

 private:
  FormEvent GetCardNumberStatusFormEvent(const CreditCard& credit_card);
  void RecordCardUnmaskFlowEvent(UnmaskAuthFlowType flow,
                                 UnmaskAuthFlowEvent event);

  bool is_context_secure_ = false;
  UnmaskAuthFlowType current_authentication_flow_;
  bool has_logged_masked_server_card_suggestion_selected_ = false;
  bool logged_suggestion_filled_was_masked_server_card_ = false;

  // Weak references.
  PersonalDataManager* personal_data_manager_;
  AutofillClient* client_;
};

}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CORE_BROWSER_METRICS_CREDIT_CARD_FORM_EVENT_LOGGER_H_
