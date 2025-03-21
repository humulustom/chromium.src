// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_OVERLAYS_PUBLIC_COMMON_INFOBARS_INFOBAR_OVERLAY_REQUEST_CONFIG_H_
#define IOS_CHROME_BROWSER_OVERLAYS_PUBLIC_COMMON_INFOBARS_INFOBAR_OVERLAY_REQUEST_CONFIG_H_

#import "ios/chrome/browser/infobars/infobar_type.h"
#import "ios/chrome/browser/infobars/overlays/infobar_overlay_type.h"
#include "ios/chrome/browser/overlays/public/overlay_request_config.h"

class InfoBarIOS;

// OverlayUserData used to hold a pointer to an InfoBar.  Used as auxiliary
// data for OverlayRequests for InfoBars.
class InfobarOverlayRequestConfig
    : public OverlayRequestConfig<InfobarOverlayRequestConfig> {
 public:
  ~InfobarOverlayRequestConfig() override;

  // The infobar that triggered this OverlayRequest.
  InfoBarIOS* infobar() const { return infobar_; }
  // |infobar_|'s type.
  InfobarType infobar_type() const { return infobar_type_; }
  // Whether |infobar_| has a badge.
  bool has_badge() const { return has_badge_; }
  // The overlay type for this infobar OverlayRequest.
  InfobarOverlayType overlay_type() const { return overlay_type_; }

 private:
  OVERLAY_USER_DATA_SETUP(InfobarOverlayRequestConfig);
  explicit InfobarOverlayRequestConfig(InfoBarIOS* infobar,
                                       InfobarOverlayType overlay_type);

  InfoBarIOS* infobar_ = nullptr;
  InfobarType infobar_type_;
  bool has_badge_ = false;
  InfobarOverlayType overlay_type_;
};

#endif  // IOS_CHROME_BROWSER_OVERLAYS_PUBLIC_COMMON_INFOBARS_INFOBAR_OVERLAY_REQUEST_CONFIG_H_
