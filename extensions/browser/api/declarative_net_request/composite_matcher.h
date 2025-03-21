// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXTENSIONS_BROWSER_API_DECLARATIVE_NET_REQUEST_COMPOSITE_MATCHER_H_
#define EXTENSIONS_BROWSER_API_DECLARATIVE_NET_REQUEST_COMPOSITE_MATCHER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/optional.h"
#include "extensions/browser/api/declarative_net_request/request_action.h"
#include "extensions/browser/api/declarative_net_request/ruleset_matcher.h"
#include "extensions/common/permissions/permissions_data.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace extensions {
namespace declarative_net_request {

struct RequestAction;

// Per extension instance which manages the different rulesets for an extension
// while respecting their priorities.
class CompositeMatcher {
 public:
  struct ActionInfo {
    ActionInfo(base::Optional<RequestAction> action, bool notify);
    ~ActionInfo();
    ActionInfo(ActionInfo&& other);
    ActionInfo& operator=(ActionInfo&& other);

    // The action to be taken for this request.
    base::Optional<RequestAction> action;

    // Whether the extension should be notified that the request was unable to
    // be redirected as the extension lacks the appropriate host permission for
    // the request. Can only be true for redirect actions.
    bool notify_request_withheld = false;

    DISALLOW_COPY_AND_ASSIGN(ActionInfo);
  };

  using MatcherList = std::vector<std::unique_ptr<RulesetMatcher>>;

  // Each RulesetMatcher should have a distinct ID and priority.
  explicit CompositeMatcher(MatcherList matchers);
  ~CompositeMatcher();

  // Adds the |new_matcher| to the list of matchers. If a matcher with the
  // corresponding ID is already present, updates the matcher.
  void AddOrUpdateRuleset(std::unique_ptr<RulesetMatcher> new_matcher);

  // Returns a RequestAction for the network request specified by |params|, or
  // base::nullopt if there is no matching rule.
  ActionInfo GetBeforeRequestAction(
      const RequestParams& params,
      PermissionsData::PageAccess page_access) const;

  // Returns the bitmask of headers to remove from the request corresponding to
  // rules matched from this extension. The bitmask corresponds to
  // RemoveHeadersMask type. |excluded_remove_headers_mask| denotes the current
  // mask of headers to be skipped for evaluation and is excluded in the return
  // value.
  uint8_t GetRemoveHeadersMask(
      const RequestParams& params,
      uint8_t excluded_remove_headers_mask,
      std::vector<RequestAction>* remove_headers_actions) const;

  // Returns whether this modifies "extraHeaders".
  bool HasAnyExtraHeadersMatcher() const;

  void OnRenderFrameDeleted(content::RenderFrameHost* host);
  void OnDidFinishNavigation(content::RenderFrameHost* host);

 private:
  bool ComputeHasAnyExtraHeadersMatcher() const;

  // Sorts |matchers_| in descending order of priority.
  void SortMatchersByPriority();

  // Sorted by priority in descending order.
  MatcherList matchers_;

  // Denotes the cached return value for |HasAnyExtraHeadersMatcher|. Care must
  // be taken to reset this as this object is modified.
  mutable base::Optional<bool> has_any_extra_headers_matcher_;

  DISALLOW_COPY_AND_ASSIGN(CompositeMatcher);
};

}  // namespace declarative_net_request
}  // namespace extensions

#endif  // EXTENSIONS_BROWSER_API_DECLARATIVE_NET_REQUEST_COMPOSITE_MATCHER_H_
