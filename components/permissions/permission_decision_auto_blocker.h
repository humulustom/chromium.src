// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PERMISSIONS_PERMISSION_DECISION_AUTO_BLOCKER_H_
#define COMPONENTS_PERMISSIONS_PERMISSION_DECISION_AUTO_BLOCKER_H_

#include "base/callback.h"
#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/singleton.h"
#include "base/time/default_clock.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/permissions/permission_result.h"
#include "url/gurl.h"

class GURL;

namespace settings {
FORWARD_DECLARE_TEST(SiteSettingsHandlerTest, GetAllSites);
}  // namespace settings

namespace permissions {

// The PermissionDecisionAutoBlocker decides whether or not a given origin
// should be automatically blocked from requesting a permission. When an origin
// is blocked, it is placed under an "embargo". Until the embargo expires, any
// requests made by the origin are automatically blocked. Once the embargo is
// lifted, the origin will be permitted to request a permission again, which may
// result in it being placed under embargo again. Currently, an origin can be
// placed under embargo if it has a number of prior dismissals greater than a
// threshold.
class PermissionDecisionAutoBlocker : public KeyedService {
 public:
  explicit PermissionDecisionAutoBlocker(HostContentSettingsMap* settings_map);
  ~PermissionDecisionAutoBlocker() override;

  // Checks the status of the content setting to determine if |request_origin|
  // is under embargo for |permission|. This checks all types of embargo.
  // Prefer to use PermissionManager::GetPermissionStatus when possible. This
  // method is only exposed to facilitate permission checks from threads other
  // than the UI thread. See https://crbug.com/658020.
  static PermissionResult GetEmbargoResult(HostContentSettingsMap* settings_map,
                                           const GURL& request_origin,
                                           ContentSettingsType permission,
                                           base::Time current_time);

  // Updates the threshold to start blocking prompts from the field trial.
  static void UpdateFromVariations();

  // Checks the status of the content setting to determine if |request_origin|
  // is under embargo for |permission|. This checks all types of embargo.
  PermissionResult GetEmbargoResult(const GURL& request_origin,
                                    ContentSettingsType permission);

  // Returns the most recent recorded time either an ignore or dismiss embargo
  // was started. Records of embargo start times persist beyond the duration of
  // the embargo, but are removed along with embargoes when RemoveEmbargoByUrl
  // or RemoveCountsByUrl are used. Returns base::Time() if no record is found.
  base::Time GetEmbargoStartTime(const GURL& request_origin,
                                 ContentSettingsType permission);

  // Returns the current number of dismisses recorded for |permission| type at
  // |url|.
  int GetDismissCount(const GURL& url, ContentSettingsType permission);

  // Returns the current number of ignores recorded for |permission|
  // type at |url|.
  int GetIgnoreCount(const GURL& url, ContentSettingsType permission);

  // Returns a set of urls currently under embargo for |content_type|.
  std::set<GURL> GetEmbargoedOrigins(ContentSettingsType content_type);

  // Returns a set of urls currently under embargo for the provided
  // |content_type| types.
  std::set<GURL> GetEmbargoedOrigins(
      std::vector<ContentSettingsType> content_types);

  // Records that a dismissal of a prompt for |permission| was made. If the
  // total number of dismissals exceeds a threshhold and
  // features::kBlockPromptsIfDismissedOften is enabled, it will place |url|
  // under embargo for |permission|. |dismissed_prompt_was_quiet| will inform
  // the decision of which threshold to pick, depending on whether the prompt
  // that was presented to the user was quiet or not.
  bool RecordDismissAndEmbargo(const GURL& url,
                               ContentSettingsType permission,
                               bool dismissed_prompt_was_quiet);

  // Records that an ignore of a prompt for |permission| was made. If the total
  // number of ignores exceeds a threshold and
  // features::kBlockPromptsIfIgnoredOften is enabled, it will place |url| under
  // embargo for |permission|. |ignored_prompt_was_quiet| will inform the
  // decision of which threshold to pick, depending on whether the prompt that
  // was presented to the user was quiet or not.
  bool RecordIgnoreAndEmbargo(const GURL& url,
                              ContentSettingsType permission,
                              bool ignored_prompt_was_quiet);

  // Clears any existing embargo status for |url|, |permission|. For permissions
  // embargoed under repeated dismissals, this means a prompt will be shown to
  // the user on next permission request. This is a NO-OP for non-embargoed
  // |url|, |permission| pairs.
  void RemoveEmbargoByUrl(const GURL& url, ContentSettingsType permission);

  // Removes any recorded counts for urls which match |filter|.
  void RemoveCountsByUrl(base::Callback<bool(const GURL& url)> filter);

  static const char* GetPromptDismissCountKeyForTesting();

 private:
  friend class PermissionDecisionAutoBlockerUnitTest;
  FRIEND_TEST_ALL_PREFIXES(settings::SiteSettingsHandlerTest, GetAllSites);

  void PlaceUnderEmbargo(const GURL& request_origin,
                         ContentSettingsType permission,
                         const char* key);

  void SetClockForTesting(base::Clock* clock);

  // Keys used for storing count data in a website setting.
  static const char kPromptDismissCountKey[];
  static const char kPromptIgnoreCountKey[];
  static const char kPromptDismissCountWithQuietUiKey[];
  static const char kPromptIgnoreCountWithQuietUiKey[];
  static const char kPermissionDismissalEmbargoKey[];
  static const char kPermissionIgnoreEmbargoKey[];

  HostContentSettingsMap* settings_map_;

  base::Clock* clock_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(PermissionDecisionAutoBlocker);
};

}  // namespace permissions

#endif  // COMPONENTS_PERMISSIONS_PERMISSION_DECISION_AUTO_BLOCKER_H_
