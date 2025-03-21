// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_INFOBARS_PRESENTATION_INFOBAR_BANNER_PRESENTATION_CONTROLLER_H_
#define IOS_CHROME_BROWSER_UI_INFOBARS_PRESENTATION_INFOBAR_BANNER_PRESENTATION_CONTROLLER_H_

#import <UIKit/UIKit.h>

@protocol InfobarBannerPositioner;

// InfobarBanner Presentation Controller.
@interface InfobarBannerPresentationController : UIPresentationController

// Designated initializer. |bannerPositioner| is used to position the
// InfobarBanner, it can't be nil.
- (instancetype)
    initWithPresentedViewController:(UIViewController*)presentedViewController
           presentingViewController:(UIViewController*)presentingViewController
                   bannerPositioner:
                       (id<InfobarBannerPositioner>)bannerPositioner
    NS_DESIGNATED_INITIALIZER;

- (instancetype)
    initWithPresentedViewController:(UIViewController*)presentedViewController
           presentingViewController:(UIViewController*)presentingViewController
    NS_UNAVAILABLE;

@end

#endif  // IOS_CHROME_BROWSER_UI_INFOBARS_PRESENTATION_INFOBAR_BANNER_PRESENTATION_CONTROLLER_H_
