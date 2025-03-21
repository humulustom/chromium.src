// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_EMBEDDER_SUPPORT_ANDROID_METRICS_ANDROID_METRICS_SERVICE_CLIENT_H_
#define COMPONENTS_EMBEDDER_SUPPORT_ANDROID_METRICS_ANDROID_METRICS_SERVICE_CLIENT_H_

#include <memory>
#include <string>

#include "base/metrics/field_trial.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "components/metrics/enabled_state_provider.h"
#include "components/metrics/metrics_log_uploader.h"
#include "components/metrics/metrics_service_client.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"

class PrefRegistrySimple;
class PrefService;

namespace metrics {
class MetricsStateManager;

// AndroidMetricsServiceClient is a singleton which manages metrics collection
// intended for use by WebView & WebLayer.
//
// Metrics should be enabled iff all these conditions are met:
//  - The user has not opted out.
//  - The app has not opted out.
//  - This client is in the 10% sample (controlled by client ID hash).
// The first two are recorded in |user_consent_| and |app_consent_|, which are
// set by SetHaveMetricsConsent(). The last is recorded in |is_in_sample_|.
//
// Metrics are pseudonymously identified by a randomly-generated "client ID".
// AndroidMetricsServiceClient stores this in prefs, written to the app's data
// directory. There's a different such directory for each user, for each app,
// on each device. So the ID should be unique per (device, app, user) tuple.
//
// In order to be transparent about not associating an ID with an opted out user
// or app, the client ID should only be created and retained when neither the
// user nor the app have opted out. Otherwise, the presence of the ID could give
// the impression that metrics were being collected.
//
// AndroidMetricsServiceClient metrics set up happens like so:
//
//   startup
//      │
//      ├────────────┐
//      │            ▼
//      │         query for consent
//      ▼            │
//   Initialize()    │
//      │            ▼
//      │         SetHaveMetricsConsent()
//      │            │
//      │ ┌──────────┘
//      ▼ ▼
//   MaybeStartMetrics()
//      │
//      ▼
//   MetricsService::Start()
//
// All the named functions in this diagram happen on the UI thread. Querying GMS
// happens in the background, and the result is posted back to the UI thread, to
// SetHaveMetricsConsent(). Querying GMS is slow, so SetHaveMetricsConsent()
// typically happens after Initialize(), but it may happen before.
//
// Each path sets a flag, |init_finished_| or |set_consent_finished_|, to show
// that path has finished, and then calls MaybeStartMetrics(). When
// MaybeStartMetrics() is called the first time, it sees only one flag is true,
// and does nothing. When MaybeStartMetrics() is called the second time, it
// decides whether to start metrics.
//
// If consent was granted, MaybeStartMetrics() determines sampling by hashing
// the client ID (generating a new ID if there was none). If this client is in
// the sample, it then calls MetricsService::Start(). If consent was not
// granted, MaybeStartMetrics() instead clears the client ID, if any.
class AndroidMetricsServiceClient : public MetricsServiceClient,
                                    public EnabledStateProvider,
                                    public content::NotificationObserver {
 public:
  AndroidMetricsServiceClient();
  ~AndroidMetricsServiceClient() override;

  AndroidMetricsServiceClient(const AndroidMetricsServiceClient&) = delete;
  AndroidMetricsServiceClient& operator=(const AndroidMetricsServiceClient&) =
      delete;

  static void RegisterPrefs(PrefRegistrySimple* registry);

  void Initialize(PrefService* pref_service);
  void SetHaveMetricsConsent(bool user_consent, bool app_consent);
  void SetFastStartupForTesting(bool fast_startup_for_testing);
  void SetUploadIntervalForTesting(const base::TimeDelta& upload_interval);
  std::unique_ptr<const base::FieldTrial::EntropyProvider>
  CreateLowEntropyProvider();

  // EnabledStateProvider
  bool IsConsentGiven() const override;
  bool IsReportingEnabled() const override;

  // MetricsServiceClient
  MetricsService* GetMetricsService() override;
  void SetMetricsClientId(const std::string& client_id) override;
  std::string GetApplicationLocale() override;
  bool GetBrand(std::string* brand_code) override;
  void CollectFinalMetricsForLog(
      const base::OnceClosure done_callback) override;
  std::unique_ptr<MetricsLogUploader> CreateUploader(
      const GURL& server_url,
      const GURL& insecure_server_url,
      base::StringPiece mime_type,
      MetricsLogUploader::MetricServiceType service_type,
      const MetricsLogUploader::UploadCallback& on_upload_complete) override;
  base::TimeDelta GetStandardUploadInterval() override;
  bool ShouldStartUpFastForTesting() const override;

  // Gets the embedding app's package name if it's OK to log. Otherwise, this
  // returns the empty string.
  std::string GetAppPackageName() override;

  // content::NotificationObserver
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

 protected:
  // Called by Initialize() to allow embedder specific initialization.
  virtual void InitInternal() = 0;

  // Called by MaybeStartMetrics() to allow embedder specific initialization.
  virtual void OnMetricsStart() = 0;

  // Returns the metrics sampling rate, to be used by IsInSample(). This is a
  // double in the non-inclusive range (0.00, 1.00). Virtual for testing.
  virtual double GetSampleRate() = 0;

  // Determines if the client is within the random sample of clients for which
  // we log metrics. If this returns false, MetricsServiceClient should
  // indicate reporting is disabled. Sampling is due to storage/bandwidth
  // considerations. Virtual for testing.
  virtual bool IsInSample();

  // Prefer calling the IsInSample() which takes no arguments. Virtual for
  // testing.
  virtual bool IsInSample(uint32_t value);

  // Determines if the embedder app is the type of app for which we may log the
  // package name. If this returns false, GetAppPackageName() must return empty
  // string. Virtual for testing.
  virtual bool CanRecordPackageNameForAppType() = 0;

  // Determines if this client falls within the group for which it's acceptable
  // to include the embedding app's package name. If this returns false,
  // GetAppPackageName() must return the empty string (for
  // privacy/fingerprintability reasons). Virtual for testing.
  virtual bool IsInPackageNameSample();

  // Prefer calling the IsInPackageNameSample() which takes no arguments.
  // Virtual for testing.
  virtual bool IsInPackageNameSample(uint32_t value);

  // Caps the rate at which we upload package names. This is privacy sensitive.
  virtual double GetPackageNameLimitRate() = 0;

  // Whether or not MetricsService::OnApplicationNotIdle should be called for
  // notifications.
  virtual bool ShouldWakeMetricsService() = 0;

  // Called by CreateMetricsService, allows the embedder to register additional
  // MetricsProviders.
  virtual void RegisterAdditionalMetricsProviders(MetricsService* service) = 0;

  // Returns the embedding application's package name.
  virtual std::string GetAppPackageNameInternal() = 0;

  void EnsureOnValidSequence() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }

  PrefService* pref_service() const { return pref_service_; }

 private:
  void MaybeStartMetrics();
  void RegisterForNotifications();

  std::unique_ptr<MetricsService> CreateMetricsService(
      MetricsStateManager* state_manager,
      AndroidMetricsServiceClient* client,
      PrefService* prefs);

  std::unique_ptr<MetricsStateManager> metrics_state_manager_;
  std::unique_ptr<MetricsService> metrics_service_;
  content::NotificationRegistrar registrar_;
  PrefService* pref_service_ = nullptr;
  bool init_finished_ = false;
  bool set_consent_finished_ = false;
  bool user_consent_ = false;
  bool app_consent_ = false;
  bool is_in_sample_ = false;
  bool fast_startup_for_testing_ = false;

  // When non-zero, this overrides the default value in
  // GetStandardUploadInterval().
  base::TimeDelta overridden_upload_interval_;

  // MetricsServiceClient may be created before the UI thread is promoted to
  // BrowserThread::UI. Use |sequence_checker_| to enforce that the
  // MetricsServiceClient is used on a single thread.
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace metrics

#endif  // COMPONENTS_EMBEDDER_SUPPORT_ANDROID_METRICS_ANDROID_METRICS_SERVICE_CLIENT_H_
