// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/toolbar/accessory/toolbar_accessory_presenter.h"

#include "base/i18n/rtl.h"
#import "base/logging.h"
#import "ios/chrome/browser/ui/image_util/image_util.h"
#import "ios/chrome/browser/ui/presenters/contained_presenter_delegate.h"
#import "ios/chrome/browser/ui/toolbar/accessory/toolbar_accessory_constants.h"
#import "ios/chrome/browser/ui/toolbar/public/toolbar_constants.h"
#import "ios/chrome/browser/ui/util/layout_guide_names.h"
#import "ios/chrome/browser/ui/util/named_guide.h"
#import "ios/chrome/browser/ui/util/uikit_ui_util.h"
#import "ios/chrome/common/colors/dynamic_color_util.h"
#import "ios/chrome/common/colors/semantic_color_names.h"
#import "ios/chrome/common/ui_util/constraints_ui_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace {
const CGFloat kToolbarAccessoryWidthRegularRegular = 375;
const CGFloat kToolbarAccessoryCornerRadiusRegularRegular = 13;
const CGFloat kRegularRegularHorizontalMargin = 5;

// Margin between the beginning of the shadow image and the content being
// shadowed.
const CGFloat kShadowMargin = 196;

// Toolbar Accessory animation drop down duration.
const CGFloat kAnimationDuration = 0.15;
}  // namespace

@interface ToolbarAccessoryPresenter ()

// The |presenting| public property redefined as readwrite.
@property(nonatomic, assign, readwrite, getter=isPresenting) BOOL presenting;

// The view that acts as the background for |presentedView|, redefined as
// readwrite. This is especially important on iPhone, as this view that holds
// everything around the safe area.
@property(nonatomic, strong, readwrite) UIView* backgroundView;

// A constraint that constrains any views to their pre-animation positions.
// It should be deactiviated during the presentation animation and replaced with
// a constraint that sets the views to their final position.
@property(nonatomic, strong) NSLayoutConstraint* animationConstraint;

@property(nonatomic, assign) BOOL isIncognito;

@end

@implementation ToolbarAccessoryPresenter

@synthesize baseViewController = _baseViewController;
@synthesize presentedViewController = _presentedViewController;
@synthesize delegate = _delegate;

#pragma mark - Public

- (instancetype)initWithIsIncognito:(BOOL)isIncognito {
  if (self = [super init]) {
    _isIncognito = isIncognito;
  }
  return self;
}

- (void)prepareForPresentation {
  self.presenting = YES;
  self.backgroundView = [self createBackgroundView];
  [self.baseViewController addChildViewController:self.presentedViewController];
  [self.baseViewController.view addSubview:self.backgroundView];

  if (ShouldShowCompactToolbar()) {
    [self prepareForPresentationOnIPhone];
  } else {
    [self prepareForPresentationOnIPad];
  }
}

- (void)presentAnimated:(BOOL)animated {
  if (animated) {
    // Force initial layout before the animation.
    [self.baseViewController.view layoutIfNeeded];
  }

  if (ShouldShowCompactToolbar()) {
    [self setupFinalConstraintsOnIPhone];
  } else {
    [self setupFinalConstraintsOnIPad];
  }

  __weak __typeof(self) weakSelf = self;
  auto completion = ^void(BOOL) {
    [weakSelf.presentedViewController
        didMoveToParentViewController:weakSelf.baseViewController];
    if ([weakSelf.delegate
            respondsToSelector:@selector(containedPresenterDidPresent:)]) {
      [weakSelf.delegate containedPresenterDidPresent:weakSelf];
    }
  };

  if (animated) {
    [UIView animateWithDuration:kAnimationDuration
                     animations:^() {
                       [self.baseViewController.view layoutIfNeeded];
                     }
                     completion:completion];
  } else {
    completion(YES);
  }
}

- (void)dismissAnimated:(BOOL)animated {
  [self.presentedViewController willMoveToParentViewController:nil];

  __weak __typeof(self) weakSelf = self;
  auto completion = ^void(BOOL) {
    [weakSelf.presentedViewController.view removeFromSuperview];
    [weakSelf.presentedViewController removeFromParentViewController];
    [weakSelf.backgroundView removeFromSuperview];
    if ([weakSelf.delegate
            respondsToSelector:@selector(containedPresenterDidDismiss:)]) {
      [weakSelf.delegate containedPresenterDidDismiss:weakSelf];
    }
    weakSelf.backgroundView = nil;
    weakSelf.presenting = NO;
    [weakSelf.delegate containedPresenterDidDismiss:weakSelf];
  };
  if (animated) {
    void (^animation)();
    if (ShouldShowCompactToolbar()) {
      CGRect oldFrame = self.backgroundView.frame;
      self.backgroundView.layer.anchorPoint = CGPointMake(0.5, 0);
      self.backgroundView.frame = oldFrame;
      animation = ^{
        self.backgroundView.transform = CGAffineTransformMakeScale(1, 0.05);
        self.backgroundView.alpha = 0;
      };
    } else {
      CGFloat rtlModifier = base::i18n::IsRTL() ? -1 : 1;
      animation = ^{
        self.backgroundView.transform = CGAffineTransformMakeTranslation(
            rtlModifier * self.backgroundView.bounds.size.width, 0);
      };
    }
    [UIView animateWithDuration:kAnimationDuration
                     animations:animation
                     completion:completion];
  } else {
    completion(YES);
  }
}

#pragma mark - Private Helpers

// Positions the view into its initial, pre-animation position on iPhone.
// Specifically, the final position will be on top of the toolbar. The
// pre-animation position is that, but slid upwards so the view is offscreen.
- (void)prepareForPresentationOnIPhone {
  self.animationConstraint = [self.backgroundView.bottomAnchor
      constraintEqualToAnchor:self.baseViewController.view.topAnchor];

  [NSLayoutConstraint activateConstraints:@[
    [self.backgroundView.leadingAnchor
        constraintEqualToAnchor:self.baseViewController.view.leadingAnchor],
    [self.backgroundView.trailingAnchor
        constraintEqualToAnchor:self.baseViewController.view.trailingAnchor],
    [self.presentedViewController.view.topAnchor
        constraintGreaterThanOrEqualToAnchor:self.backgroundView
                                                 .safeAreaLayoutGuide
                                                 .topAnchor],
    self.animationConstraint,
  ]];
}

// Positions the view into its initial, pre-animation position on iPad.
// Specifically, the final position will be a small accessory just below the
// toolbar on the upper right. The pre-animation position is that, but slid
// offscreen to the right.
- (void)prepareForPresentationOnIPad {
  self.backgroundView.layer.cornerRadius =
      kToolbarAccessoryCornerRadiusRegularRegular;

  UIImageView* shadow =
      [[UIImageView alloc] initWithImage:StretchableImageNamed(@"menu_shadow")];
  shadow.translatesAutoresizingMaskIntoConstraints = NO;
  [self.backgroundView addSubview:shadow];

  // The width should be this value, unless that is too wide for the screen.
  // Using a less than required priority means that the view will attempt to be
  // this width, but use the less than or equal constraint below if the view
  // is too narrow.
  NSLayoutConstraint* widthConstraint = [self.backgroundView.widthAnchor
      constraintEqualToConstant:kToolbarAccessoryWidthRegularRegular];
  widthConstraint.priority = UILayoutPriorityRequired - 1;

  self.animationConstraint = [self.backgroundView.leadingAnchor
      constraintEqualToAnchor:self.baseViewController.view.trailingAnchor];

  UILayoutGuide* toolbarLayoutGuide =
      [NamedGuide guideWithName:kPrimaryToolbarGuide
                           view:self.baseViewController.view];

  [NSLayoutConstraint activateConstraints:@[
    // Anchors accessory below the the toolbar.
    [self.backgroundView.topAnchor
        constraintEqualToAnchor:toolbarLayoutGuide.bottomAnchor],
    self.animationConstraint,
    widthConstraint,
    [self.backgroundView.widthAnchor
        constraintLessThanOrEqualToAnchor:self.baseViewController.view
                                              .widthAnchor
                                 constant:-2 * kRegularRegularHorizontalMargin],
    [self.backgroundView.heightAnchor
        constraintEqualToConstant:kAdaptiveToolbarHeight],
  ]];
  // Layouts |shadow| around |self.backgroundView|.
  AddSameConstraintsToSidesWithInsets(
      shadow, self.backgroundView,
      LayoutSides::kTop | LayoutSides::kLeading | LayoutSides::kBottom |
          LayoutSides::kTrailing,
      {-kShadowMargin, -kShadowMargin, -kShadowMargin, -kShadowMargin});
}

// Sets up the constraints on iPhone such that the view is ready to be animated
// to its final position.
- (void)setupFinalConstraintsOnIPhone {
  self.animationConstraint.active = NO;
  [self.backgroundView.topAnchor
      constraintEqualToAnchor:self.baseViewController.view.topAnchor]
      .active = YES;
}

// Sets up the constraints on iPhone such that the view is ready to be animated
// to its final position.
- (void)setupFinalConstraintsOnIPad {
  self.animationConstraint.active = NO;
  [self.backgroundView.trailingAnchor
      constraintEqualToAnchor:self.baseViewController.view.trailingAnchor
                     constant:-kRegularRegularHorizontalMargin]
      .active = YES;
}

// Creates the background view and adds |self.presentedViewController.view| to
// it
- (UIView*)createBackgroundView {
  UIView* backgroundView = [[UIView alloc] init];
  backgroundView.translatesAutoresizingMaskIntoConstraints = NO;
  backgroundView.accessibilityIdentifier = kToolbarAccessoryContainerViewID;
  if (@available(iOS 13, *)) {
    // When iOS 12 is dropped, only the next line is needed for styling.
    // Every other check for |incognitoStyle| can be removed, as well as
    // the incognito specific assets.
    backgroundView.overrideUserInterfaceStyle =
        self.isIncognito ? UIUserInterfaceStyleDark
                         : UIUserInterfaceStyleUnspecified;
  }
  backgroundView.backgroundColor = color::DarkModeDynamicColor(
      [UIColor colorNamed:kBackgroundColor], self.isIncognito,
      [UIColor colorNamed:kBackgroundDarkColor]);

  [backgroundView addSubview:self.presentedViewController.view];

  [NSLayoutConstraint activateConstraints:@[
    [self.presentedViewController.view.trailingAnchor
        constraintEqualToAnchor:backgroundView.trailingAnchor],
    [self.presentedViewController.view.leadingAnchor
        constraintEqualToAnchor:backgroundView.leadingAnchor],
    [self.presentedViewController.view.heightAnchor
        constraintEqualToConstant:kAdaptiveToolbarHeight],
    [self.presentedViewController.view.bottomAnchor
        constraintEqualToAnchor:backgroundView.bottomAnchor],
  ]];

  return backgroundView;
}

@end
