/*
 * Copyright (C) 2015 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "third_party/blink/renderer/core/frame/browser_controls.h"

#include "build/build_config.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_coalesced_input_event.h"
#include "third_party/blink/public/platform/web_url_loader_mock_factory.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/frame/frame_test_helpers.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/geometry/dom_rect.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/paint/compositing/composited_layer_mapping.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/testing/scoped_mock_overlay_scrollbars.h"
#include "third_party/blink/renderer/core/testing/sim/sim_request.h"
#include "third_party/blink/renderer/core/testing/sim/sim_test.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/url_test_helpers.h"

namespace blink {

// These tests cover browser controls scrolling on main-thread.
// The animation for completing a partial show/hide is done in compositor so
// it is not covered here.
class BrowserControlsTest : public testing::Test,
                            public ScopedMockOverlayScrollbars {
 public:
  BrowserControlsTest() : base_url_("http://www.test.com/") {
    RegisterMockedHttpURLLoad("large-div.html");
    RegisterMockedHttpURLLoad("overflow-scrolling.html");
    RegisterMockedHttpURLLoad("iframe-scrolling.html");
    RegisterMockedHttpURLLoad("iframe-scrolling-inner.html");
    RegisterMockedHttpURLLoad("percent-height.html");
    RegisterMockedHttpURLLoad("vh-height.html");
    RegisterMockedHttpURLLoad("vh-height-width-800.html");
    RegisterMockedHttpURLLoad("95-vh.html");
    RegisterMockedHttpURLLoad("vh-height-width-800-extra-wide.html");
  }

  ~BrowserControlsTest() override {
    platform_->GetURLLoaderMockFactory()
        ->UnregisterAllURLsAndClearMemoryCache();
  }

  WebViewImpl* Initialize(const std::string& page_name = "large-div.html") {
    // Load a page with large body and set viewport size to 400x400 to ensure
    // main frame is scrollable.
    helper_.InitializeAndLoad(base_url_ + page_name, nullptr, nullptr, nullptr,
                              &ConfigureSettings);

    GetWebView()->MainFrameWidget()->Resize(IntSize(400, 400));
    return GetWebView();
  }

  static void ConfigureSettings(WebSettings* settings) {
    settings->SetJavaScriptEnabled(true);
    settings->SetPreferCompositingToLCDTextEnabled(true);
    // Android settings
    settings->SetViewportEnabled(true);
    settings->SetViewportMetaEnabled(true);
    settings->SetShrinksViewportContentToFit(true);
    settings->SetMainFrameResizesAreOrientationChanges(true);
  }

  void RegisterMockedHttpURLLoad(const std::string& file_name) {
    url_test_helpers::RegisterMockedURLLoadFromBase(
        WebString::FromUTF8(base_url_), test::CoreTestDataPath(),
        WebString::FromUTF8(file_name));
  }

  WebCoalescedInputEvent GenerateEvent(WebInputEvent::Type type,
                                       int delta_x = 0,
                                       int delta_y = 0) {
    WebGestureEvent event(type, WebInputEvent::kNoModifiers,
                          WebInputEvent::GetStaticTimeStampForTests(),
                          WebGestureDevice::kTouchscreen);
    event.SetPositionInWidget(FloatPoint(100, 100));
    if (type == WebInputEvent::kGestureScrollUpdate) {
      event.data.scroll_update.delta_x = delta_x;
      event.data.scroll_update.delta_y = delta_y;
    } else if (type == WebInputEvent::kGestureScrollBegin) {
      event.data.scroll_begin.delta_x_hint = delta_x;
      event.data.scroll_begin.delta_y_hint = delta_y;
    }
    return WebCoalescedInputEvent(event);
  }

  void VerticalScroll(float delta_y) {
    GetWebView()->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, delta_y));
    GetWebView()->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, delta_y));
    GetWebView()->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollEnd));
  }

  Element* GetElementById(const WebString& id) {
    return static_cast<Element*>(
        GetWebView()->MainFrameImpl()->GetDocument().GetElementById(id));
  }

  WebViewImpl* GetWebView() const { return helper_.GetWebView(); }
  LocalFrame* GetFrame() const { return helper_.LocalMainFrame()->GetFrame(); }
  VisualViewport& GetVisualViewport() const {
    return helper_.GetWebView()->GetPage()->GetVisualViewport();
  }

  void UpdateAllLifecyclePhases() {
    GetWebView()->MainFrameWidget()->UpdateAllLifecyclePhases(
        DocumentUpdateReason::kTest);
  }

 private:
  ScopedTestingPlatformSupport<TestingPlatformSupport> platform_;
  std::string base_url_;
  frame_test_helpers::WebViewHelper helper_;
};

class BrowserControlsSimTest : public SimTest {
 public:
  BrowserControlsSimTest() {}

  void SetUp() override {
    SimTest::SetUp();

    // Use settings that resemble the Android configuration.
    WebView().GetSettings()->SetViewportEnabled(true);
    WebView().GetSettings()->SetPreferCompositingToLCDTextEnabled(true);
    WebView().GetSettings()->SetViewportMetaEnabled(true);
    WebView().GetSettings()->SetViewportEnabled(true);
    WebView().GetSettings()->SetMainFrameResizesAreOrientationChanges(true);
    WebView().GetSettings()->SetShrinksViewportContentToFit(true);
    WebView().SetDefaultPageScaleLimits(0.25f, 5);
    Compositor().layer_tree_host().UpdateBrowserControlsState(
        cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown,
        false);
    WebView().ResizeWithBrowserControls(WebSize(412, 604), 56.f, 50.f, true);
  }

  WebCoalescedInputEvent GenerateEvent(WebInputEvent::Type type,
                                       int delta_x = 0,
                                       int delta_y = 0) {
    WebGestureEvent event(type, WebInputEvent::kNoModifiers,
                          WebInputEvent::GetStaticTimeStampForTests(),
                          WebGestureDevice::kTouchscreen);
    event.SetPositionInWidget(FloatPoint(100, 100));
    if (type == WebInputEvent::kGestureScrollUpdate) {
      event.data.scroll_update.delta_x = delta_x;
      event.data.scroll_update.delta_y = delta_y;
    } else if (type == WebInputEvent::kGestureScrollBegin) {
      event.data.scroll_begin.delta_x_hint = delta_x;
      event.data.scroll_begin.delta_y_hint = delta_y;
    }
    return WebCoalescedInputEvent(event);
  }

  void VerticalScroll(float delta_y) {
    WebView().MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, delta_y));
    WebView().MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, delta_y));
    WebView().MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollEnd));
  }
};

// Disable these tests on Mac OSX until further investigation.
// Local build on Mac is OK but the bot fails. This is not an issue as
// Browser Controls are currently only used on Android.
#if defined(OS_MACOSX)
#define MAYBE(test) DISABLED_##test
#else
#define MAYBE(test) test
#endif

// Scrolling down should hide browser controls.
TEST_F(BrowserControlsTest, MAYBE(HideOnScrollDown)) {
  WebViewImpl* web_view = Initialize();
  // initialize browser controls to be shown.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());

  // Browser controls should be scrolled partially and page should not scroll.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -25.f));
  EXPECT_FLOAT_EQ(25.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 0),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Browser controls should consume 25px and become hidden. Excess scroll
  // should be
  // consumed by the page.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -40.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 15),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Only page should consume scroll
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -20.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 35),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Scrolling down should hide bottom browser controls.
TEST_F(BrowserControlsTest, MAYBE(HideBottomControlsOnScrollDown)) {
  WebViewImpl* web_view = Initialize();
  // initialize browser controls to be shown.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 0,
                                      50.f, true);
  web_view->GetBrowserControls().SetShownRatio(0.0, 1);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  // Bottom controls and page content should both scroll and there should be
  // no content offset.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -25.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_FLOAT_EQ(0.5f, web_view->GetBrowserControls().BottomShownRatio());
  EXPECT_EQ(ScrollOffset(0, 25.f),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Browser controls should become completely hidden.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -40.f));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().BottomShownRatio());
  EXPECT_EQ(ScrollOffset(0, 65.f),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Scrolling up should show browser controls.
TEST_F(BrowserControlsTest, MAYBE(ShowOnScrollUp)) {
  WebViewImpl* web_view = Initialize();
  // initialize browser controls to be hidden.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, false);
  web_view->GetBrowserControls().SetShownRatio(0, 0);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 10.f));
  EXPECT_FLOAT_EQ(10.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 0),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 50.f));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 0),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Scrolling up should show the bottom browser controls.
TEST_F(BrowserControlsTest, MAYBE(ShowBottomControlsOnScrollUp)) {
  WebViewImpl* web_view = Initialize();
  // initialize browser controls to be hidden.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 0,
                                      50.f, false);
  web_view->GetBrowserControls().SetShownRatio(0, 0);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  // Allow some space to scroll up.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -50.f));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 25.f));
  EXPECT_FLOAT_EQ(0.5f, web_view->GetBrowserControls().BottomShownRatio());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_FLOAT_EQ(1.f, web_view->GetBrowserControls().BottomShownRatio());
  EXPECT_EQ(ScrollOffset(0, 25),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Scrolling up after previous scroll downs should cause browser controls to be
// shown only after all previously scrolled down amount is compensated.
TEST_F(BrowserControlsTest, MAYBE(ScrollDownThenUp)) {
  WebViewImpl* web_view = Initialize();
  // initialize browser controls to be shown and position page at 100px.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  GetFrame()->View()->GetScrollableArea()->SetScrollOffset(
      ScrollOffset(0, 100),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());

  // Scroll down to completely hide browser controls. Excess deltaY (100px)
  // should be consumed by the page.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -150.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 200),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Scroll up and ensure the browser controls does not move until we recover
  // 100px previously scrolled.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 40.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 160),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 60.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 100),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Now we have hit the threshold so further scroll up should be consumed by
  // browser controls.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 30.f));
  EXPECT_FLOAT_EQ(30.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 100),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Once top control is fully shown then page should consume any excess scroll.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 70.f));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Scrolling down should always cause visible browser controls to start hiding
// even if we have been scrolling up previously.
TEST_F(BrowserControlsTest, MAYBE(ScrollUpThenDown)) {
  WebViewImpl* web_view = Initialize();
  // initialize browser controls to be hidden and position page at 100px.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, false);
  web_view->GetBrowserControls().SetShownRatio(0, 0);
  GetFrame()->View()->GetScrollableArea()->SetScrollOffset(
      ScrollOffset(0, 100),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  // Scroll up to completely show browser controls. Excess deltaY (50px) should
  // be consumed by the page.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 100.f));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Scroll down and ensure only browser controls is scrolled
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -40.f));
  EXPECT_FLOAT_EQ(10.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -60.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 100),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Browser controls should not consume horizontal scroll.
TEST_F(BrowserControlsTest, MAYBE(HorizontalScroll)) {
  WebViewImpl* web_view = Initialize();
  // initialize browser controls to be shown.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());

  // Browser controls should not consume horizontal scroll.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, -110.f, -100.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(110, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, -40.f, 0));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(150, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Page scale should not impact browser controls scrolling
TEST_F(BrowserControlsTest, MAYBE(PageScaleHasNoImpact)) {
  WebViewImpl* web_view = Initialize();
  GetWebView()->SetDefaultPageScaleLimits(0.25f, 5);
  web_view->SetPageScaleFactor(2.0);

  // Initialize browser controls to be shown.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());

  // Browser controls should be scrolled partially and page should not scroll.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -20.f));
  EXPECT_FLOAT_EQ(30.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 0),
            GetFrame()->View()->GetScrollableArea()->GetScrollOffset());

  // Browser controls should consume 30px and become hidden. Excess scroll
  // should be consumed by the page at 2x scale.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -70.f));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 20),
            GetFrame()->View()->GetScrollableArea()->GetScrollOffset());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));

  // Change page scale and test.
  web_view->SetPageScaleFactor(0.5);

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 20),
            GetFrame()->View()->GetScrollableArea()->GetScrollOffset());

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 50.f));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 20),
            GetFrame()->View()->GetScrollableArea()->GetScrollOffset());

  // At 0.5x scale scrolling 10px should take us to the top of the page.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 10.f));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 0),
            GetFrame()->View()->GetScrollableArea()->GetScrollOffset());
}

// Some scroll deltas result in a shownRatio that can't be realized in a
// floating-point number. Make sure that if the browser controls aren't fully
// scrolled, scrollBy doesn't return any excess delta. i.e. There should be no
// slippage between the content and browser controls.
TEST_F(BrowserControlsTest, MAYBE(FloatingPointSlippage)) {
  WebViewImpl* web_view = Initialize();
  GetWebView()->SetDefaultPageScaleLimits(0.25f, 5);
  web_view->SetPageScaleFactor(2.0);

  // Initialize browser controls to be shown.
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);

  web_view->GetBrowserControls().ScrollBegin();
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());

  // This will result in a 20px scroll to the browser controls so the show ratio
  // will be 30/50 == 0.6 which is not representible in a float. Make sure
  // that scroll still consumes the whole delta.
  FloatSize remaining_delta =
      web_view->GetBrowserControls().ScrollBy(FloatSize(0, 10));
  EXPECT_EQ(0, remaining_delta.Height());
}

// Scrollable subregions should scroll before browser controls
TEST_F(BrowserControlsTest, MAYBE(ScrollableSubregionScrollFirst)) {
  WebViewImpl* web_view = Initialize("overflow-scrolling.html");
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  GetFrame()->View()->GetScrollableArea()->SetScrollOffset(
      ScrollOffset(0, 50),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  // Test scroll down
  // A full scroll down should scroll the overflow div first but browser
  // controls and main frame should not scroll.
  VerticalScroll(-800.f);
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Now scroll down should start hiding browser controls but main frame
  // should not scroll.
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, -40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -40.f));
  EXPECT_FLOAT_EQ(10.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Continued scroll down should scroll down the main frame
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 80),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Test scroll up
  // A full scroll up should scroll overflow div first
  VerticalScroll(800.f);
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 80),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Now scroll up should start showing browser controls but main frame
  // should not scroll.
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, 40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 40.f));
  EXPECT_FLOAT_EQ(40.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 80),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Continued scroll up scroll up the main frame
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Scrollable iframes should scroll before browser controls
TEST_F(BrowserControlsTest, MAYBE(ScrollableIframeScrollFirst)) {
  WebViewImpl* web_view = Initialize("iframe-scrolling.html");
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  GetFrame()->View()->GetScrollableArea()->SetScrollOffset(
      ScrollOffset(0, 50),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  // Test scroll down
  // A full scroll down should scroll the iframe first but browser controls and
  // main frame should not scroll.
  VerticalScroll(-800.f);
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Now scroll down should start hiding browser controls but main frame
  // should not scroll.
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, -40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -40.f));
  EXPECT_FLOAT_EQ(10.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Continued scroll down should scroll down the main frame
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 80),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Test scroll up
  // A full scroll up should scroll iframe first
  VerticalScroll(800.f);
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 80),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Now scroll up should start showing browser controls but main frame
  // should not scroll.
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, 40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 40.f));
  EXPECT_FLOAT_EQ(40.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 80),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  // Continued scroll up scroll up the main frame
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 40.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 50),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());
}

// Browser controls visibility should remain consistent when height is changed.
TEST_F(BrowserControlsTest, MAYBE(HeightChangeMaintainsVisibility)) {
  WebViewImpl* web_view = Initialize();
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 20.f,
                                      0, false);
  web_view->GetBrowserControls().SetShownRatio(0, 0);

  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 20.f,
                                      0, false);
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 40.f,
                                      0, false);
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  // Scroll up to show browser controls.
  VerticalScroll(40.f);
  EXPECT_FLOAT_EQ(40.f, web_view->GetBrowserControls().ContentOffset());

  // Changing height of a fully shown browser controls should correctly adjust
  // content offset
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 30.f,
                                      0, false);
  EXPECT_FLOAT_EQ(30.f, web_view->GetBrowserControls().ContentOffset());
}

// Zero delta should not have any effect on browser controls.
TEST_F(BrowserControlsTest, MAYBE(ZeroHeightMeansNoEffect)) {
  WebViewImpl* web_view = Initialize();
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 0, 0,
                                      false);
  web_view->GetBrowserControls().SetShownRatio(0, 0);
  GetFrame()->View()->GetScrollableArea()->SetScrollOffset(
      ScrollOffset(0, 100),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  VerticalScroll(20.f);
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 80),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  VerticalScroll(-30.f);
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 110),
            GetFrame()->View()->LayoutViewport()->GetScrollOffset());

  web_view->GetBrowserControls().SetShownRatio(1, 1);
  EXPECT_FLOAT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
}

// Browser controls should not hide when scrolling up past limit
TEST_F(BrowserControlsTest, MAYBE(ScrollUpPastLimitDoesNotHide)) {
  WebViewImpl* web_view = Initialize();
  // Initialize browser controls to be shown
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  // Use 2x scale so that both visual viewport and frameview are scrollable
  web_view->SetPageScaleFactor(2.0);

  // Fully scroll frameview but visualviewport remains scrollable
  web_view->MainFrameImpl()->SetScrollOffset(WebSize(0, 10000));
  GetVisualViewport().SetLocation(FloatPoint(0, 0));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, -10.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -10.f));
  EXPECT_FLOAT_EQ(40, web_view->GetBrowserControls().ContentOffset());

  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(50, web_view->GetBrowserControls().ContentOffset());

  web_view->GetBrowserControls().SetShownRatio(1, 1);
  // Fully scroll visual veiwport but frameview remains scrollable
  web_view->MainFrameImpl()->SetScrollOffset(WebSize(0, 0));
  GetVisualViewport().SetLocation(FloatPoint(0, 10000));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, -20.f));
  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -20.f));
  EXPECT_FLOAT_EQ(30, web_view->GetBrowserControls().ContentOffset());

  GetWebView()->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(50, web_view->GetBrowserControls().ContentOffset());

  web_view->GetBrowserControls().SetShownRatio(1, 1);
  // Fully scroll both frameview and visual viewport
  web_view->MainFrameImpl()->SetScrollOffset(WebSize(0, 10000));
  GetVisualViewport().SetLocation(FloatPoint(0, 10000));
  VerticalScroll(-30.f);
  // Browser controls should not move because neither frameview nor visual
  // viewport
  // are scrollable
  EXPECT_FLOAT_EQ(50.f, web_view->GetBrowserControls().ContentOffset());
}

// Browser controls should honor its constraints
TEST_F(BrowserControlsSimTest, MAYBE(StateConstraints)) {
  SimRequest request("https://example.com/test.html", "text/html");
  LoadURL("https://example.com/test.html");
  request.Complete(R"HTML(
        <!DOCTYPE html>
        <meta name="viewport" content="width=device-width">
        <style>
          body {
            margin: 0;
            height: 1000px;
          }
        </style>
      )HTML");
  Compositor().BeginFrame();

  WebView().ResizeWithBrowserControls(WebSize(400, 400), 50.f, 0, false);
  Compositor().BeginFrame();

  GetDocument().View()->GetScrollableArea()->SetScrollOffset(
      ScrollOffset(0, 100),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);
  // Setting permitted state should change the content offset to match the
  // constraint.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kShown, cc::BrowserControlsState::kShown,
      false);
  Compositor().BeginFrame();
  EXPECT_FLOAT_EQ(50.f, WebView().GetBrowserControls().ContentOffset());

  WebView().ResizeWithBrowserControls(WebSize(400, 400), 50.f, 50.f, false);
  Compositor().BeginFrame();
  // Bottom controls shouldn't affect the content offset.
  EXPECT_FLOAT_EQ(50.f, WebView().GetBrowserControls().ContentOffset());

  // Only shown state is permitted so controls cannot hide.
  VerticalScroll(-20.f);
  EXPECT_FLOAT_EQ(50, WebView().GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 120),
            GetDocument().View()->LayoutViewport()->GetScrollOffset());

  // Setting permitted state should change content offset to match the
  // constraint.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kHidden,
      false);
  Compositor().BeginFrame();
  EXPECT_FLOAT_EQ(0, WebView().GetBrowserControls().ContentOffset());

  // Only hidden state is permitted so controls cannot show
  VerticalScroll(30.f);
  EXPECT_FLOAT_EQ(0, WebView().GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 90),
            GetDocument().View()->LayoutViewport()->GetScrollOffset());

  // Setting permitted state to "both" should not change content offset.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kBoth, false);
  Compositor().BeginFrame();
  EXPECT_FLOAT_EQ(0, WebView().GetBrowserControls().ContentOffset());

  // Both states are permitted so controls can either show or hide
  VerticalScroll(50.f);
  EXPECT_FLOAT_EQ(50, WebView().GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 90),
            GetDocument().View()->LayoutViewport()->GetScrollOffset());

  VerticalScroll(-50.f);
  EXPECT_FLOAT_EQ(0, WebView().GetBrowserControls().ContentOffset());
  EXPECT_EQ(ScrollOffset(0, 90),
            GetDocument().View()->LayoutViewport()->GetScrollOffset());

  // Setting permitted state to "both" should not change an in-flight offset.
  WebView().MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin, 0, 20.f));
  WebView().MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 20.f));
  EXPECT_FLOAT_EQ(20, WebView().GetBrowserControls().ContentOffset());

  WebView().MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_FLOAT_EQ(0, WebView().GetBrowserControls().ContentOffset());
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kBoth, false);
  Compositor().BeginFrame();
  EXPECT_FLOAT_EQ(0, WebView().GetBrowserControls().ContentOffset());

  // Setting just the constraint should affect the content offset.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kBoth,
      false);
  Compositor().BeginFrame();
  EXPECT_FLOAT_EQ(0, WebView().GetBrowserControls().ContentOffset());

  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kShown, cc::BrowserControlsState::kBoth, false);
  Compositor().BeginFrame();
  EXPECT_FLOAT_EQ(50, WebView().GetBrowserControls().ContentOffset());
}

// Ensure that browser controls do not affect the layout by showing and hiding
// except for position: fixed elements.
TEST_F(BrowserControlsTest, MAYBE(DontAffectLayoutHeight)) {
  // Initialize with the browser controls showing.
  WebViewImpl* web_view = Initialize("percent-height.html");
  web_view->ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  web_view->GetBrowserControls().UpdateConstraintsAndState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  UpdateAllLifecyclePhases();

  ASSERT_EQ(100.f, web_view->GetBrowserControls().ContentOffset());

  // When the browser controls are showing, there's 300px for the layout height
  // so
  // 50% should result in both the position:fixed and position: absolute divs
  // having 150px of height.
  Element* abs_pos = GetElementById(WebString::FromUTF8("abs"));
  Element* fixed_pos = GetElementById(WebString::FromUTF8("fixed"));
  EXPECT_FLOAT_EQ(150.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(150.f, fixed_pos->getBoundingClientRect()->height());

  // The layout size on the LocalFrameView should not include the browser
  // controls.
  EXPECT_EQ(300, GetFrame()->View()->GetLayoutSize().Height());

  // Hide the browser controls.
  VerticalScroll(-100.f);
  web_view->ResizeWithBrowserControls(WebSize(400, 400), 100.f, 0, false);
  UpdateAllLifecyclePhases();

  ASSERT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  // Hiding the browser controls shouldn't change the height of the initial
  // containing block for non-position: fixed. Position: fixed however should
  // use the entire height of the viewport however.
  EXPECT_FLOAT_EQ(150.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());

  // The layout size should not change as a result of browser controls hiding.
  EXPECT_EQ(300, GetFrame()->View()->GetLayoutSize().Height());
}

// Ensure that browser controls do not affect the layout by showing and hiding
// except for position: fixed elements.
TEST_F(BrowserControlsSimTest, MAYBE(AffectLayoutHeightWhenConstrained)) {
  SimRequest request("https://example.com/test.html", "text/html");
  LoadURL("https://example.com/test.html");
  request.Complete(R"HTML(
        <!DOCTYPE html>
          <style>
            #abs {
              position: absolute;
              left: 0px;
              top: 0px;
              width: 100px;
              height: 50%;
            }

            #fixed {
              position: fixed;
              right: 0px;
              top: 0px;
              width: 100px;
              height: 50%;
            }

            #spacer {
              height: 1000px;
            }
          </style>
        <div id="abs"></div>
        <div id="fixed"></div>
        <div id="spacer"></div>
      )HTML");
  Compositor().BeginFrame();

  WebView().ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown, false);
  Compositor().BeginFrame();

  Element* abs_pos = GetDocument().getElementById(WebString::FromUTF8("abs"));
  Element* fixed_pos =
      GetDocument().getElementById(WebString::FromUTF8("fixed"));

  ASSERT_EQ(100.f, WebView().GetBrowserControls().ContentOffset());

  // Hide the browser controls.
  VerticalScroll(-100.f);
  WebView().ResizeWithBrowserControls(WebSize(400, 400), 100.f, 0, false);
  Compositor().BeginFrame();
  ASSERT_EQ(300, GetDocument().GetFrame()->View()->GetLayoutSize().Height());

  // Now lock the controls in a hidden state. The layout and elements should
  // resize without a WebView::resize.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kBoth,
      false);
  Compositor().BeginFrame();

  EXPECT_FLOAT_EQ(200.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());

  EXPECT_EQ(400, GetDocument().GetFrame()->View()->GetLayoutSize().Height());

  // Unlock the controls, the sizes should change even though the controls are
  // still hidden.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kBoth, false);
  Compositor().BeginFrame();

  EXPECT_FLOAT_EQ(150.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());

  EXPECT_EQ(300, GetDocument().GetFrame()->View()->GetLayoutSize().Height());

  // Now lock the controls in a shown state.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kShown, cc::BrowserControlsState::kBoth, false);
  WebView().ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  Compositor().BeginFrame();

  EXPECT_FLOAT_EQ(150.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(150.f, fixed_pos->getBoundingClientRect()->height());

  EXPECT_EQ(300, GetDocument().GetFrame()->View()->GetLayoutSize().Height());

  // Shown -> Hidden
  WebView().ResizeWithBrowserControls(WebSize(400, 400), 100.f, 0, false);
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kBoth,
      false);
  Compositor().BeginFrame();

  EXPECT_FLOAT_EQ(200.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());

  EXPECT_EQ(400, GetDocument().GetFrame()->View()->GetLayoutSize().Height());

  // Go from Unlocked and showing, to locked and hidden but issue the resize
  // before the constraint update to check for race issues.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown, false);
  WebView().ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  Compositor().BeginFrame();
  ASSERT_EQ(300, GetDocument().GetFrame()->View()->GetLayoutSize().Height());

  WebView().ResizeWithBrowserControls(WebSize(400, 400), 100.f, 0, false);
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kHidden,
      false);
  Compositor().BeginFrame();

  EXPECT_FLOAT_EQ(200.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());

  EXPECT_EQ(400, GetDocument().GetFrame()->View()->GetLayoutSize().Height());
}

// Ensure that browser controls do not affect vh units.
TEST_F(BrowserControlsTest, MAYBE(DontAffectVHUnits)) {
  // Initialize with the browser controls showing.
  WebViewImpl* web_view = Initialize("vh-height.html");
  web_view->ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  web_view->GetBrowserControls().UpdateConstraintsAndState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  UpdateAllLifecyclePhases();

  ASSERT_EQ(100.f, web_view->GetBrowserControls().ContentOffset());

  // 'vh' units should be based on the viewport when the browser controls are
  // hidden.
  Element* abs_pos = GetElementById(WebString::FromUTF8("abs"));
  Element* fixed_pos = GetElementById(WebString::FromUTF8("fixed"));
  EXPECT_FLOAT_EQ(200.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());

  // The size used for viewport units should not be reduced by the top
  // controls.
  EXPECT_EQ(400, GetFrame()->View()->ViewportSizeForViewportUnits().Height());

  // Hide the browser controls.
  VerticalScroll(-100.f);
  web_view->ResizeWithBrowserControls(WebSize(400, 400), 100.f, 0, false);
  UpdateAllLifecyclePhases();

  ASSERT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  // vh units should be static with respect to the browser controls so neighter
  // <div> should change size are a result of the browser controls hiding.
  EXPECT_FLOAT_EQ(200.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());

  // The viewport size used for vh units should not change as a result of top
  // controls hiding.
  EXPECT_EQ(400, GetFrame()->View()->ViewportSizeForViewportUnits().Height());
}

// Ensure that on a legacy page (there's a non-1 minimum scale) 100vh units fill
// the viewport, with browser controls hidden, when the viewport encompasses the
// layout width.
TEST_F(BrowserControlsTest, MAYBE(DontAffectVHUnitsWithScale)) {
  // Initialize with the browser controls showing.
  WebViewImpl* web_view = Initialize("vh-height-width-800.html");
  web_view->ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  web_view->GetBrowserControls().UpdateConstraintsAndState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  UpdateAllLifecyclePhases();

  ASSERT_EQ(100.f, web_view->GetBrowserControls().ContentOffset());

  // Device viewport is 400px but the page is width=800 so minimum-scale
  // should be 0.5. This is also the scale at which the viewport fills the
  // layout width.
  ASSERT_EQ(0.5f, web_view->MinimumPageScaleFactor());

  // We should size vh units so that 100vh fills the viewport at min-scale so
  // we have to account for the minimum page scale factor. Since both boxes
  // are 50vh, and layout scale = 0.5, we have a vh viewport of 400 / 0.5 = 800
  // so we expect 50vh to be 400px.
  Element* abs_pos = GetElementById(WebString::FromUTF8("abs"));
  Element* fixed_pos = GetElementById(WebString::FromUTF8("fixed"));
  EXPECT_FLOAT_EQ(400.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(400.f, fixed_pos->getBoundingClientRect()->height());

  // The size used for viewport units should not be reduced by the top
  // controls.
  EXPECT_EQ(800, GetFrame()->View()->ViewportSizeForViewportUnits().Height());

  // Hide the browser controls.
  VerticalScroll(-100.f);
  web_view->ResizeWithBrowserControls(WebSize(400, 400), 100.f, 0, false);
  UpdateAllLifecyclePhases();

  ASSERT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  // vh units should be static with respect to the browser controls so neighter
  // <div> should change size are a result of the browser controls hiding.
  EXPECT_FLOAT_EQ(400.f, abs_pos->getBoundingClientRect()->height());
  EXPECT_FLOAT_EQ(400.f, fixed_pos->getBoundingClientRect()->height());

  // The viewport size used for vh units should not change as a result of top
  // controls hiding.
  EXPECT_EQ(800, GetFrame()->View()->ViewportSizeForViewportUnits().Height());
}

// Ensure that on a legacy page (there's a non-1 minimum scale) whose viewport
// at minimum-scale is larger than the layout size, 100vh units fill the
// viewport, with browser controls hidden, when the viewport is scaled such that
// its width equals the layout width.
TEST_F(BrowserControlsTest, MAYBE(DontAffectVHUnitsUseLayoutSize)) {
  // Initialize with the browser controls showing.
  WebViewImpl* web_view = Initialize("vh-height-width-800-extra-wide.html");
  web_view->ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  web_view->GetBrowserControls().UpdateConstraintsAndState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  UpdateAllLifecyclePhases();

  ASSERT_EQ(100.f, web_view->GetBrowserControls().ContentOffset());

  // Device viewport is 400px and page is width=800 but there's an element
  // that's 1600px wide so the minimum scale is 0.25 to encompass that.
  ASSERT_EQ(0.25f, web_view->MinimumPageScaleFactor());

  // The viewport will match the layout width at scale=0.5 so the height used
  // for vh should be (300 / 0.5) for the layout height + (100 / 0.5) for top
  // controls = 800.
  EXPECT_EQ(800, GetFrame()->View()->ViewportSizeForViewportUnits().Height());
}

// This tests that the viewport remains anchored when browser controls are
// brought in while the document is fully scrolled. This normally causes
// clamping of the visual viewport to keep it bounded by the layout viewport
// so we're testing that the viewport anchoring logic is working to keep the
// view unchanged.
TEST_F(BrowserControlsTest,
       MAYBE(AnchorViewportDuringbrowserControlsAdjustment)) {
  int content_height = 1016;
  int layout_viewport_height = 500;
  int visual_viewport_height = 500;
  int browser_controls_height = 100;
  int page_scale = 2;
  int min_scale = 1;

  // Initialize with the browser controls showing.
  WebViewImpl* web_view = Initialize("large-div.html");
  GetWebView()->SetDefaultPageScaleLimits(min_scale, 5);
  web_view->ResizeWithBrowserControls(WebSize(800, layout_viewport_height),
                                      browser_controls_height, 0, true);
  web_view->GetBrowserControls().UpdateConstraintsAndState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  UpdateAllLifecyclePhases();

  LocalFrameView* view = GetFrame()->View();
  ScrollableArea* root_viewport = GetFrame()->View()->GetScrollableArea();

  int expected_visual_offset =
      ((layout_viewport_height + browser_controls_height / min_scale) *
           page_scale -
       (visual_viewport_height + browser_controls_height)) /
      page_scale;
  int expected_layout_offset =
      content_height -
      (layout_viewport_height + browser_controls_height / min_scale);
  int expected_root_offset = expected_visual_offset + expected_layout_offset;

  // Zoom in to 2X and fully scroll both viewports.
  web_view->SetPageScaleFactor(page_scale);
  {
    web_view->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollBegin));
    web_view->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -10000));

    ASSERT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

    EXPECT_EQ(expected_visual_offset,
              GetVisualViewport().GetScrollOffset().Height());
    EXPECT_EQ(expected_layout_offset,
              view->LayoutViewport()->GetScrollOffset().Height());
    EXPECT_EQ(expected_root_offset, root_viewport->GetScrollOffset().Height());

    web_view->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollEnd));
  }

  // Commit the browser controls resize so that the browser controls do not
  // shrink the layout size. This should not have moved any of the viewports.
  web_view->ResizeWithBrowserControls(
      WebSize(800, layout_viewport_height + browser_controls_height),
      browser_controls_height, 0, false);
  UpdateAllLifecyclePhases();
  ASSERT_EQ(expected_visual_offset,
            GetVisualViewport().GetScrollOffset().Height());
  ASSERT_EQ(expected_layout_offset,
            view->LayoutViewport()->GetScrollOffset().Height());
  ASSERT_EQ(expected_root_offset, root_viewport->GetScrollOffset().Height());

  // Now scroll back up just enough to show the browser controls. The browser
  // controls should shrink both viewports but the layout viewport by a greater
  // amount. This means the visual viewport's offset must be clamped to keep it
  // within the layout viewport. Make sure we adjust the scroll position to
  // account for this and keep the visual viewport at the same location relative
  // to the document (i.e. the user shouldn't see a movement).
  {
    web_view->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollBegin));
    web_view->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 80));

    GetVisualViewport().ClampToBoundaries();
    view->LayoutViewport()->SetScrollOffset(
        view->LayoutViewport()->GetScrollOffset(),
        mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

    ASSERT_EQ(80.f, web_view->GetBrowserControls().ContentOffset());
    EXPECT_EQ(expected_root_offset, root_viewport->GetScrollOffset().Height());

    web_view->MainFrameWidget()->HandleInputEvent(
        GenerateEvent(WebInputEvent::kGestureScrollEnd));
  }
}

// Ensure that vh units are correct when browser controls are in a locked
// state. That is, normally we need to add the browser controls height to vh
// units since 100vh includes the browser controls even if they're hidden while
// the ICB height does not. When the controls are locked hidden, the ICB size
// is the full viewport height so there's no need to add the browser controls
// height.  crbug.com/688738.
TEST_F(BrowserControlsSimTest, MAYBE(ViewportUnitsWhenControlsLocked)) {
  // Initialize with the browser controls showing.
  SimRequest request("https://example.com/test.html", "text/html");
  LoadURL("https://example.com/test.html");
  request.Complete(R"HTML(
          <!DOCTYPE html>
            <style>
              #abs {
                position: absolute;
                left: 0px;
                top: 0px;
                width: 100px;
                height: 50vh;
              }

              #fixed {
                position: fixed;
                right: 0px;
                top: 0px;
                width: 100px;
                height: 50vh;
              }

              #spacer {
                height: 1000px;
              }
            </style>
            <div id="abs"></div>
            <div id="fixed"></div>
            <div id="spacer"></div>
      )HTML");
  WebView().ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown, false);
  Compositor().BeginFrame();

  ASSERT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());
  ASSERT_EQ(100.f, WebView().GetBrowserControls().ContentOffset());
  ASSERT_EQ(300, GetDocument().View()->GetLayoutSize().Height());

  Element* abs_pos = GetDocument().getElementById("abs");
  Element* fixed_pos = GetDocument().getElementById("fixed");

  // Lock the browser controls to hidden.
  {
    Compositor().layer_tree_host().UpdateBrowserControlsState(
        cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kHidden,
        false);
    WebView().ResizeWithBrowserControls(WebSize(400, 400), 100.f, 0, false);
    Compositor().BeginFrame();

    ASSERT_EQ(0.f, WebView().GetBrowserControls().ContentOffset());
    ASSERT_EQ(400, GetDocument().View()->GetLayoutSize().Height());

    // Make sure we're not adding the browser controls height to the vh units
    // as when they're locked to hidden, the ICB fills the entire viewport
    // already.
    EXPECT_FLOAT_EQ(200.f, abs_pos->getBoundingClientRect()->height());
    EXPECT_FLOAT_EQ(200.f, fixed_pos->getBoundingClientRect()->height());
    EXPECT_EQ(400,
              GetDocument().View()->ViewportSizeForViewportUnits().Height());
  }

  // Lock the browser controls to shown. This should cause the vh units to
  // behave as usual by including the browser controls region in 100vh.
  {
    Compositor().layer_tree_host().UpdateBrowserControlsState(
        cc::BrowserControlsState::kShown, cc::BrowserControlsState::kShown,
        false);
    WebView().ResizeWithBrowserControls(WebSize(400, 300), 100.f, 0, true);
    Compositor().BeginFrame();

    ASSERT_EQ(100.f, WebView().GetBrowserControls().ContentOffset());
    ASSERT_EQ(300, GetDocument().View()->GetLayoutSize().Height());

    // Make sure we're not adding the browser controls height to the vh units as
    // when they're locked to shown, the ICB fills the entire viewport already.
    EXPECT_FLOAT_EQ(150.f, abs_pos->getBoundingClientRect()->height());
    EXPECT_FLOAT_EQ(150.f, fixed_pos->getBoundingClientRect()->height());
    EXPECT_EQ(400,
              GetDocument().View()->ViewportSizeForViewportUnits().Height());
  }
}

// Test the size adjustment sent to the viewport when top controls exist.
TEST_F(BrowserControlsTest, MAYBE(TopControlsSizeAdjustment)) {
  WebViewImpl* web_view = Initialize();
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      0, false);
  web_view->GetBrowserControls().SetShownRatio(1, 0.0);
  EXPECT_FLOAT_EQ(-50.f,
                  web_view->GetBrowserControls().UnreportedSizeAdjustment());

  web_view->GetBrowserControls().SetShownRatio(0.5, 0.0);
  EXPECT_FLOAT_EQ(-25.f,
                  web_view->GetBrowserControls().UnreportedSizeAdjustment());

  web_view->GetBrowserControls().SetShownRatio(0.0, 0.0);
  EXPECT_FLOAT_EQ(0.f,
                  web_view->GetBrowserControls().UnreportedSizeAdjustment());
}

// Test the size adjustment sent to the viewport when bottom controls exist.
// There should never be an adjustment since the bottom controls do not change
// the content offset.
TEST_F(BrowserControlsTest, MAYBE(BottomControlsSizeAdjustment)) {
  WebViewImpl* web_view = Initialize();
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 0,
                                      50.f, false);
  web_view->GetBrowserControls().SetShownRatio(0.0, 1);
  EXPECT_FLOAT_EQ(0.f,
                  web_view->GetBrowserControls().UnreportedSizeAdjustment());

  web_view->GetBrowserControls().SetShownRatio(0.0, 0.5);
  EXPECT_FLOAT_EQ(0.f,
                  web_view->GetBrowserControls().UnreportedSizeAdjustment());

  web_view->GetBrowserControls().SetShownRatio(0.0, 0.0);
  EXPECT_FLOAT_EQ(0.f,
                  web_view->GetBrowserControls().UnreportedSizeAdjustment());
}

TEST_F(BrowserControlsTest, MAYBE(GrowingHeightKeepsTopControlsHidden)) {
  WebViewImpl* web_view = Initialize();
  float bottom_height = web_view->GetBrowserControls().BottomHeight();
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 1.f,
                                      bottom_height, false);

  web_view->GetBrowserControls().UpdateConstraintsAndState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kHidden);

  // As we expand the top controls height while hidden, the content offset
  // shouldn't change.
  EXPECT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(), 50.f,
                                      bottom_height, false);
  EXPECT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());

  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(),
                                      100.f, bottom_height, false);
  EXPECT_EQ(0.f, web_view->GetBrowserControls().ContentOffset());
}

TEST_F(BrowserControlsTest,
       MAYBE(HidingBrowserControlsInvalidatesGraphicsLayer)) {
  // Initialize with the browser controls showing.
  WebViewImpl* web_view = Initialize("95-vh.html");
  web_view->ResizeWithBrowserControls(WebSize(412, 604), 56.f, 0, true);
  web_view->GetBrowserControls().SetShownRatio(1, 1);
  UpdateAllLifecyclePhases();

  GetFrame()->GetDocument()->View()->SetTracksRasterInvalidations(true);

  // Hide the browser controls.
  VerticalScroll(-100.f);
  web_view->ResizeWithBrowserControls(WebSize(412, 660), 56.f, 0, false);
  UpdateAllLifecyclePhases();

  // Ensure there is a raster invalidation of the bottom of the layer.
  const auto& raster_invalidations = GetFrame()
                                         ->ContentLayoutObject()
                                         ->Layer()
                                         ->GetCompositedLayerMapping()
                                         ->ScrollingContentsLayer()
                                         ->GetRasterInvalidationTracking()
                                         ->Invalidations();
  EXPECT_EQ(1u, raster_invalidations.size());
  EXPECT_EQ(IntRect(0, 643, 412, 17), raster_invalidations[0].rect);
  EXPECT_EQ(PaintInvalidationReason::kIncremental,
            raster_invalidations[0].reason);

  GetFrame()->GetDocument()->View()->SetTracksRasterInvalidations(false);
}

// Test that the browser controls have different shown ratios when scrolled with
// a minimum height set for only top controls.
TEST_F(BrowserControlsTest, MAYBE(ScrollWithMinHeightSetForTopControlsOnly)) {
  WebViewImpl* web_view = Initialize();
  float top_height = 56;
  float bottom_height = 50;
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(),
                                      top_height, bottom_height, false);
  web_view->GetBrowserControls().SetShownRatio(1.f, 1.f);
  web_view->GetBrowserControls().SetParams(
      {top_height, 20, bottom_height, 0, false, true});
  // Scroll down to hide the controls.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -100));

  // The bottom controls should be completely hidden while the top controls are
  // at the minimum height.
  EXPECT_EQ(0.f, web_view->GetBrowserControls().BottomShownRatio());
  EXPECT_GT(web_view->GetBrowserControls().TopShownRatio(), 0);
  EXPECT_EQ(20, web_view->GetBrowserControls().ContentOffset());

  // Scrolling back up should bring the browser controls shown ratios back to 1.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 100));
  EXPECT_EQ(1.f, web_view->GetBrowserControls().BottomShownRatio());
  EXPECT_EQ(1.f, web_view->GetBrowserControls().TopShownRatio());
  EXPECT_EQ(top_height, web_view->GetBrowserControls().ContentOffset());
}

// Test that the browser controls don't scroll off when a minimum height is set.
TEST_F(BrowserControlsTest, MAYBE(ScrollWithMinHeightSet)) {
  WebViewImpl* web_view = Initialize();
  float top_height = 56;
  float bottom_height = 50;
  web_view->ResizeWithBrowserControls(web_view->MainFrameWidget()->Size(),
                                      top_height, bottom_height, false);
  web_view->GetBrowserControls().SetShownRatio(1.f, 1.f);
  web_view->GetBrowserControls().SetParams(
      {top_height, 20, bottom_height, 10, false, true});

  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -100));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));

  // Browser controls don't scroll off completely, and stop scrolling at the min
  // height.
  EXPECT_EQ(20, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(10, web_view->GetBrowserControls().BottomContentOffset());

  // Ending the scroll then scrolling again shouldn't make any difference.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, -50));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollEnd));
  EXPECT_EQ(20, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(10, web_view->GetBrowserControls().BottomContentOffset());

  // Finally, scroll back up to show the controls completely.
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollBegin));
  web_view->MainFrameWidget()->HandleInputEvent(
      GenerateEvent(WebInputEvent::kGestureScrollUpdate, 0, 100));
  EXPECT_EQ(top_height, web_view->GetBrowserControls().ContentOffset());
  EXPECT_EQ(bottom_height,
            web_view->GetBrowserControls().BottomContentOffset());
}

#undef MAYBE

// Test that sending both an animated and non-animated browser control update
// doesn't cause the animated one to squash the non-animated.
// https://crbug.com/861618.
TEST_F(BrowserControlsSimTest, MixAnimatedAndNonAnimatedUpdateState) {
  SimRequest request("https://example.com/test.html", "text/html");
  LoadURL("https://example.com/test.html");
  request.Complete(R"HTML(
          <!DOCTYPE html>
          <meta name="viewport" content="width=device-width">
          <style>
            body {
              height: 2000px;
            }
          </style>
      )HTML");
  Compositor().BeginFrame();

  ASSERT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());

  // Kick off a non-animated clamp to hide the top controls.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kBoth,
      false /* animated */);

  // Now kick off an animated one to do the same thing.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kBoth,
      true /* animated */);

  // Advance time. In https://crbug.com/861618, the animation didn't realize
  // yet we're already at 0, so it would play the compositor-side up to 80ms,
  // somewhere mid-way hidden. Later on in this BeginFrame the changes from the
  // main thread are committed so the top controls shown ratio will set to 0.
  Compositor().BeginFrame(0.080);

  EXPECT_EQ(0.f, WebView().GetBrowserControls().TopShownRatio());

  // Tick the animation again. The animation should have been stopped. In
  // https://crbug.com/861618, the animation would continue to play since it
  // was kicked off after the non-animated call as far as the compositor could
  // see. This means this animation tick would set the delta to some non-0 value
  // again. This value will be committed to the main thread causing the controls
  // to show.
  Compositor().BeginFrame();

  EXPECT_EQ(0.f, WebView().GetBrowserControls().TopShownRatio());
}

// Test that requesting an animated hide on the top controls actually
// animates rather than happening instantly.
TEST_F(BrowserControlsSimTest, HideAnimated) {
  SimRequest request("https://example.com/test.html", "text/html");
  LoadURL("https://example.com/test.html");
  request.Complete(R"HTML(
          <!DOCTYPE html>
          <meta name="viewport" content="width=device-width">
          <style>
            body {
              height: 2000px;
            }
          </style>
      )HTML");
  Compositor().BeginFrame();

  ASSERT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());
  ASSERT_EQ(1.f, WebView().GetBrowserControls().BottomShownRatio());

  // Kick off an animated hide.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kHidden,
      true /* animated */);

  Compositor().BeginFrame();

  ASSERT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());
  ASSERT_EQ(1.f, WebView().GetBrowserControls().BottomShownRatio());

  // Advance time.
  Compositor().BeginFrame(0.080);

  EXPECT_NE(0.f, WebView().GetBrowserControls().TopShownRatio());
  EXPECT_NE(1.f, WebView().GetBrowserControls().TopShownRatio());
  EXPECT_EQ(WebView().GetBrowserControls().TopShownRatio(),
            WebView().GetBrowserControls().BottomShownRatio());
}

// Test that requesting an animated show on the top controls actually
// animates rather than happening instantly.
TEST_F(BrowserControlsSimTest, ShowAnimated) {
  SimRequest request("https://example.com/test.html", "text/html");
  LoadURL("https://example.com/test.html");
  request.Complete(R"HTML(
          <!DOCTYPE html>
          <meta name="viewport" content="width=device-width">
          <style>
            body {
              height: 2000px;
            }
          </style>
      )HTML");
  Compositor().BeginFrame();

  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kHidden,
      false);

  Compositor().BeginFrame();

  ASSERT_EQ(0.f, WebView().GetBrowserControls().TopShownRatio());
  ASSERT_EQ(0.f, WebView().GetBrowserControls().BottomShownRatio());

  // Kick off an animated show.
  Compositor().layer_tree_host().UpdateBrowserControlsState(
      cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown,
      true /* animated */);

  Compositor().BeginFrame();

  ASSERT_EQ(0.f, WebView().GetBrowserControls().TopShownRatio());
  ASSERT_EQ(0.f, WebView().GetBrowserControls().BottomShownRatio());

  // Advance time.
  Compositor().BeginFrame(0.080);

  EXPECT_NE(0.f, WebView().GetBrowserControls().TopShownRatio());
  EXPECT_NE(1.f, WebView().GetBrowserControls().TopShownRatio());

  // The bottom controls shown ratio should follow the top controls.
  EXPECT_EQ(WebView().GetBrowserControls().TopShownRatio(),
            WebView().GetBrowserControls().BottomShownRatio());
}

// Test that setting a constraint inside Blink doesn't clamp the ratio to the
// constraint. This is required since the CC-side will set the ratio correctly.
// If we did clamp the ratio, an animation running in CC would get clobbered
// when we commit.
TEST_F(BrowserControlsSimTest, ConstraintDoesntClampRatioInBlink) {
  SimRequest request("https://example.com/test.html", "text/html");
  LoadURL("https://example.com/test.html");
  request.Complete(R"HTML(
          <!DOCTYPE html>
          <meta name="viewport" content="width=device-width">
          <style>
            body {
              height: 2000px;
            }
          </style>
      )HTML");
  Compositor().BeginFrame();

  ASSERT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());
  ASSERT_EQ(1.f, WebView().GetBrowserControls().BottomShownRatio());

  {
    // Pass a hidden constraint to Blink (without going through CC). Make sure
    // the shown ratio doesn't change since CC is responsible for updating the
    // ratio.
    WebView().GetBrowserControls().UpdateConstraintsAndState(
        cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kBoth);
    EXPECT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());
    EXPECT_EQ(1.f, WebView().GetBrowserControls().BottomShownRatio());
    WebView().GetBrowserControls().UpdateConstraintsAndState(
        cc::BrowserControlsState::kHidden, cc::BrowserControlsState::kBoth);
    EXPECT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());
    EXPECT_EQ(1.f, WebView().GetBrowserControls().BottomShownRatio());

    // Constrain the controls to hidden from the compositor. This should
    // actually cause the controls to hide when we commit.
    Compositor().layer_tree_host().UpdateBrowserControlsState(
        cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kHidden,
        false /* animated */);
    Compositor().BeginFrame();

    EXPECT_EQ(0.f, WebView().GetBrowserControls().TopShownRatio());
    EXPECT_EQ(0.f, WebView().GetBrowserControls().BottomShownRatio());
  }

  {
    // Pass a shown constraint to Blink (without going through CC). Make sure
    // the shown ratio doesn't change.
    WebView().GetBrowserControls().UpdateConstraintsAndState(
        cc::BrowserControlsState::kShown, cc::BrowserControlsState::kBoth);
    EXPECT_EQ(0.f, WebView().GetBrowserControls().TopShownRatio());
    EXPECT_EQ(0.f, WebView().GetBrowserControls().BottomShownRatio());
    WebView().GetBrowserControls().UpdateConstraintsAndState(
        cc::BrowserControlsState::kShown, cc::BrowserControlsState::kBoth);
    EXPECT_EQ(0.f, WebView().GetBrowserControls().TopShownRatio());
    EXPECT_EQ(0.f, WebView().GetBrowserControls().BottomShownRatio());

    // Constrain the controls to hidden from the compositor. This should
    // actually cause the controls to hide when we commit.
    Compositor().layer_tree_host().UpdateBrowserControlsState(
        cc::BrowserControlsState::kBoth, cc::BrowserControlsState::kShown,
        false /* animated */);
    Compositor().BeginFrame();

    EXPECT_EQ(1.f, WebView().GetBrowserControls().TopShownRatio());
    EXPECT_EQ(1.f, WebView().GetBrowserControls().BottomShownRatio());
  }
}

}  // namespace blink
