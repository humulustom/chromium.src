// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SSL_CHROME_SSL_HOST_STATE_DELEGATE_H_
#define CHROME_BROWSER_SSL_CHROME_SSL_HOST_STATE_DELEGATE_H_

#include <memory>
#include <set>

#include "base/feature_list.h"
#include "base/macros.h"
#include "base/time/time.h"
#include "content/public/browser/ssl_host_state_delegate.h"

class Profile;

namespace base {
class Clock;
class DictionaryValue;
}  //  namespace base

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs


// Tracks state related to certificate and SSL errors. This state includes:
// - certificate error exceptions (which are remembered for a particular length
//   of time depending on experimental groups)
// - mixed content exceptions
// - when errors have recurred multiple times
class ChromeSSLHostStateDelegate : public content::SSLHostStateDelegate {
 public:
  enum RecurrentInterstitialMode { PREF, IN_MEMORY, NOT_SET };

  explicit ChromeSSLHostStateDelegate(Profile* profile);
  ~ChromeSSLHostStateDelegate() override;

  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

  // content::SSLHostStateDelegate overrides:
  void AllowCert(const std::string& host,
                 const net::X509Certificate& cert,
                 int error,
                 content::WebContents* web_contents) override;
  void Clear(
      base::RepeatingCallback<bool(const std::string&)> host_filter) override;
  CertJudgment QueryPolicy(const std::string& host,
                           const net::X509Certificate& cert,
                           int error,
                           content::WebContents* web_contents) override;
  void HostRanInsecureContent(const std::string& host,
                              int child_id,
                              InsecureContentType content_type) override;
  bool DidHostRunInsecureContent(const std::string& host,
                                 int child_id,
                                 InsecureContentType content_type) override;
  void RevokeUserAllowExceptions(const std::string& host) override;
  bool HasAllowException(const std::string& host,
                         content::WebContents* web_contents) override;

  // RevokeUserAllowExceptionsHard is the same as RevokeUserAllowExceptions but
  // additionally may close idle connections in the process. This should be used
  // *only* for rare events, such as a user controlled button, as it may be very
  // disruptive to the networking stack.
  virtual void RevokeUserAllowExceptionsHard(const std::string& host);

  // Called when an error page is displayed for a given error code |error|.
  // Tracks whether an error of interest has recurred over a threshold number of
  // times.
  void DidDisplayErrorPage(int error);

  // Returns true if DidDisplayErrorPage() has been called over a threshold
  // number of times for a particular error in a particular time period. The number
  // of times and time period are controlled by the feature parameters. Only
  // certain error codes of interest are tracked, so this may return false for
  // an error code that has recurred.
  bool HasSeenRecurrentErrors(int error) const;

  void ResetRecurrentErrorCountForTesting();

  // SetClockForTesting takes ownership of the passed in clock.
  void SetClockForTesting(std::unique_ptr<base::Clock> clock);

  void SetRecurrentInterstitialThresholdForTesting(int threshold);
  void SetRecurrentInterstitialModeForTesting(
      ChromeSSLHostStateDelegate::RecurrentInterstitialMode mode);
  void SetRecurrentInterstitialResetTimeForTesting(int reset);

  RecurrentInterstitialMode GetRecurrentInterstitialMode() const;
  int GetRecurrentInterstitialThreshold() const;
  int GetRecurrentInterstitialResetTime() const;

 private:
  // Used to specify whether new content setting entries should be created if
  // they don't already exist when querying the user's settings.
  enum CreateDictionaryEntriesDisposition {
    CREATE_DICTIONARY_ENTRIES,
    DO_NOT_CREATE_DICTIONARY_ENTRIES
  };

  // Returns a dictionary of certificate fingerprints and errors that have been
  // allowed as exceptions by the user.
  //
  // |dict| specifies the user's full exceptions dictionary for a specific site
  // in their content settings. Must be retrieved directly from a website
  // setting in the the profile's HostContentSettingsMap.
  //
  // If |create_entries| specifies CreateDictionaryEntries, then
  // GetValidCertDecisionsDict will create a new set of entries within the
  // dictionary if they do not already exist. Otherwise will fail and return if
  // NULL if they do not exist.
  base::DictionaryValue* GetValidCertDecisionsDict(
      base::DictionaryValue* dict,
      CreateDictionaryEntriesDisposition create_entries);

  std::unique_ptr<base::Clock> clock_;
  Profile* profile_;

  using AllowedCert = std::pair<std::string /* certificate fingerprint */,
                                base::FilePath /* StoragePartition path */>;

  // Typically, cert decisions are stored in ContentSettings and persisted to
  // disk. For non-default StoragePartitions, particularly a <webview> in a
  // Chrome App, the decisions should be isolated from normal browsing and don't
  // need to be persisted to disk. In fact, persisting them is undesirable
  // because they may not have UI exposed to the user when a certificate error
  // is bypassed. So we track these decisions purely in memory. See
  // https://crbug.com/639173.
  std::map<std::string /* host */, std::set<AllowedCert>>
      allowed_certs_for_non_default_storage_partitions_;

  // A BrokenHostEntry is a pair of (host, child_id) that indicates the host
  // contains insecure content in that renderer process.
  using BrokenHostEntry = std::pair<std::string, int>;

  // Hosts which have been contaminated with insecure mixed content in the
  // specified process.  Note that insecure content can travel between
  // same-origin frames in one processs but cannot jump between processes.
  std::set<BrokenHostEntry> ran_mixed_content_hosts_;

  // Hosts which have been contaminated with content with certificate errors in
  // the specific process.
  std::set<BrokenHostEntry> ran_content_with_cert_errors_hosts_;

  // Tracks how many times an error page has been shown for a given error, up
  // to a certain threshold value.
  std::map<int /* error code */, int /* count */> recurrent_errors_;

  DISALLOW_COPY_AND_ASSIGN(ChromeSSLHostStateDelegate);

  int recurrent_interstitial_threshold_for_testing;
  enum RecurrentInterstitialMode recurrent_interstitial_mode_for_testing;
  int recurrent_interstitial_reset_time_for_testing;
};

#endif  // CHROME_BROWSER_SSL_CHROME_SSL_HOST_STATE_DELEGATE_H_
