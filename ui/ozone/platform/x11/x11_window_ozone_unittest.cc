// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/x11/x11_window_ozone.h"

#include <memory>
#include <utility>

#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/events/devices/x11/touch_factory_x11.h"
#include "ui/events/event.h"
#include "ui/events/platform/x11/x11_event_source.h"
#include "ui/events/test/events_test_utils_x11.h"
#include "ui/ozone/test/mock_platform_window_delegate.h"
#include "ui/platform_window/platform_window_delegate.h"
#include "ui/platform_window/platform_window_init_properties.h"
#include "ui/platform_window/x11/x11_window_manager.h"

namespace ui {

namespace {

using ::testing::_;

constexpr int kPointerDeviceId = 1;

ACTION_P(StoreWidget, widget_ptr) {
  *widget_ptr = arg0;
}

ACTION_P(CloneEvent, event_ptr) {
  *event_ptr = Event::Clone(*arg0);
}

// TestScreen implementation. We need to set a screen instance, because
// X11Window requires it. And as long as depending on views is a dependency
// violation, keep own implementation here. Otherwise, we could just use
// ScreenOzone, but it is impossible.
// We are not really interested in sending back real displays. Thus, default one
// is more than enough.
class TestScreen : public display::Screen {
 public:
  TestScreen() : displays_({}) {}
  ~TestScreen() override = default;

  // display::Screen interface.
  gfx::Point GetCursorScreenPoint() override { return {}; }
  bool IsWindowUnderCursor(gfx::NativeWindow window) override { return false; }
  gfx::NativeWindow GetWindowAtScreenPoint(const gfx::Point& point) override {
    return gfx::NativeWindow();
  }
  int GetNumDisplays() const override { return GetAllDisplays().size(); }
  const std::vector<display::Display>& GetAllDisplays() const override {
    return displays_;
  }
  display::Display GetDisplayNearestWindow(
      gfx::NativeWindow window) const override {
    return {};
  }
  display::Display GetDisplayNearestPoint(
      const gfx::Point& point) const override {
    return {};
  }
  display::Display GetDisplayMatching(
      const gfx::Rect& match_rect) const override {
    return {};
  }
  display::Display GetPrimaryDisplay() const override { return {}; }
  void AddObserver(display::DisplayObserver* observer) override {}
  void RemoveObserver(display::DisplayObserver* observer) override {}

 private:
  std::vector<display::Display> displays_;

  DISALLOW_COPY_AND_ASSIGN(TestScreen);
};  // namespace

}  // namespace

class X11WindowOzoneTest : public testing::Test {
 public:
  X11WindowOzoneTest()
      : task_env_(std::make_unique<base::test::TaskEnvironment>(
            base::test::TaskEnvironment::MainThreadType::UI)) {}

  ~X11WindowOzoneTest() override = default;

  void SetUp() override {
    XDisplay* display = gfx::GetXDisplay();
    event_source_ = std::make_unique<X11EventSource>(display);

    display::Screen::SetScreenInstance(new TestScreen());

    TouchFactory::GetInstance()->SetPointerDeviceForTest({kPointerDeviceId});
  }

 protected:
  std::unique_ptr<PlatformWindow> CreatePlatformWindow(
      MockPlatformWindowDelegate* delegate,
      const gfx::Rect& bounds,
      gfx::AcceleratedWidget* widget) {
    EXPECT_CALL(*delegate, OnAcceleratedWidgetAvailable(_))
        .WillOnce(StoreWidget(widget));
    PlatformWindowInitProperties init_params(bounds);
    auto window = std::make_unique<X11WindowOzone>(delegate);
    window->Initialize(std::move(init_params));
    return std::move(window);
  }

  void DispatchXEvent(XEvent* event, gfx::AcceleratedWidget widget) {
    DCHECK_EQ(GenericEvent, event->type);
    XIDeviceEvent* device_event =
        static_cast<XIDeviceEvent*>(event->xcookie.data);
    device_event->event = widget;
    event_source_->ProcessXEvent(event);
  }

  X11WindowManager* window_manager() const {
    auto* window_manager = X11WindowManager::GetInstance();
    DCHECK(window_manager);
    return window_manager;
  }

 private:
  std::unique_ptr<base::test::TaskEnvironment> task_env_;
  std::unique_ptr<X11EventSource> event_source_;

  DISALLOW_COPY_AND_ASSIGN(X11WindowOzoneTest);
};

// This test ensures that events are handled by a right target(widget).
TEST_F(X11WindowOzoneTest, SendPlatformEventToRightTarget) {
  MockPlatformWindowDelegate delegate;
  gfx::AcceleratedWidget widget;
  constexpr gfx::Rect bounds(30, 80, 800, 600);
  auto window = CreatePlatformWindow(&delegate, bounds, &widget);

  ScopedXI2Event xi_event;
  xi_event.InitGenericButtonEvent(kPointerDeviceId, ET_MOUSE_PRESSED,
                                  gfx::Point(218, 290), EF_NONE);

  // First check events can be received by a target window.
  std::unique_ptr<Event> event;
  EXPECT_CALL(delegate, DispatchEvent(_)).WillOnce(CloneEvent(&event));

  DispatchXEvent(xi_event, widget);
  EXPECT_EQ(ET_MOUSE_PRESSED, event->type());
  testing::Mock::VerifyAndClearExpectations(&delegate);

  MockPlatformWindowDelegate delegate_2;
  gfx::AcceleratedWidget widget_2;
  gfx::Rect bounds_2(525, 155, 296, 407);

  auto window_2 = CreatePlatformWindow(&delegate_2, bounds_2, &widget_2);

  // Check event goes to right target without capture being set.
  event.reset();
  EXPECT_CALL(delegate, DispatchEvent(_)).Times(0);
  EXPECT_CALL(delegate_2, DispatchEvent(_)).WillOnce(CloneEvent(&event));

  DispatchXEvent(xi_event, widget_2);
  EXPECT_EQ(ET_MOUSE_PRESSED, event->type());

  EXPECT_CALL(delegate, OnClosed()).Times(1);
  EXPECT_CALL(delegate_2, OnClosed()).Times(1);
}

// This test case ensures that events are consumed by a window with explicit
// capture, even though the event is sent to other window.
TEST_F(X11WindowOzoneTest, SendPlatformEventToCapturedWindow) {
  MockPlatformWindowDelegate delegate;
  gfx::AcceleratedWidget widget;
  constexpr gfx::Rect bounds(30, 80, 800, 600);
  EXPECT_CALL(delegate, OnClosed()).Times(1);
  auto window = CreatePlatformWindow(&delegate, bounds, &widget);

  MockPlatformWindowDelegate delegate_2;
  gfx::AcceleratedWidget widget_2;
  gfx::Rect bounds_2(525, 155, 296, 407);
  EXPECT_CALL(delegate_2, OnClosed()).Times(1);
  auto window_2 = CreatePlatformWindow(&delegate_2, bounds_2, &widget_2);

  ScopedXI2Event xi_event;
  xi_event.InitGenericButtonEvent(kPointerDeviceId, ET_MOUSE_PRESSED,
                                  gfx::Point(218, 290), EF_NONE);

  // Set capture to the second window, but send an event to another window
  // target. The event must have its location converted and received by the
  // captured window instead.
  window_2->SetCapture();
  std::unique_ptr<Event> event;
  EXPECT_CALL(delegate, DispatchEvent(_)).Times(0);
  EXPECT_CALL(delegate_2, DispatchEvent(_)).WillOnce(CloneEvent(&event));

  DispatchXEvent(xi_event, widget);
  EXPECT_EQ(ET_MOUSE_PRESSED, event->type());
  EXPECT_EQ(gfx::Point(-277, 215), event->AsLocatedEvent()->location());
}

// This test case ensures window_manager properly provides X11WindowOzone
// instances as they are created/destroyed.
TEST_F(X11WindowOzoneTest, GetWindowFromAcceleratedWigets) {
  MockPlatformWindowDelegate delegate;
  gfx::Rect bounds(0, 0, 100, 100);
  gfx::AcceleratedWidget widget_1;
  auto window_1 = CreatePlatformWindow(&delegate, bounds, &widget_1);
  EXPECT_EQ(window_1.get(), window_manager()->GetWindow(widget_1));

  gfx::AcceleratedWidget widget_2;
  auto window_2 = CreatePlatformWindow(&delegate, bounds, &widget_2);
  EXPECT_EQ(window_2.get(), window_manager()->GetWindow(widget_2));
  EXPECT_EQ(window_1.get(), window_manager()->GetWindow(widget_1));

  window_1->Close();
  window_1.reset();
  EXPECT_EQ(nullptr, window_manager()->GetWindow(widget_1));
  EXPECT_EQ(window_2.get(), window_manager()->GetWindow(widget_2));

  window_2.reset();
  EXPECT_EQ(nullptr, window_manager()->GetWindow(widget_1));
  EXPECT_EQ(nullptr, window_manager()->GetWindow(widget_2));
}

// This test case ensures that OnMouseEnter is called once when a mouse location
// moved to the window, and |window_mouse_currently_on_| is properly reset when
// the window is deleted.
TEST_F(X11WindowOzoneTest, MouseEnterAndDelete) {
  gfx::Rect bounds_1(0, 0, 100, 100);
  MockPlatformWindowDelegate delegate_1;
  gfx::AcceleratedWidget widget_1;
  auto window_1 = CreatePlatformWindow(&delegate_1, bounds_1, &widget_1);

  MockPlatformWindowDelegate delegate_2;
  gfx::AcceleratedWidget widget_2;
  gfx::Rect bounds_2(0, 100, 100, 100);
  auto window_2 = CreatePlatformWindow(&delegate_2, bounds_2, &widget_2);

  EXPECT_CALL(delegate_1, OnMouseEnter()).Times(1);
  window_manager()->MouseOnWindow(static_cast<X11WindowOzone*>(window_1.get()));
  // The mouse is already on window_1, and this should not call OnMouseEnter.
  window_manager()->MouseOnWindow(static_cast<X11WindowOzone*>(window_1.get()));

  EXPECT_CALL(delegate_2, OnMouseEnter()).Times(1);
  window_manager()->MouseOnWindow(static_cast<X11WindowOzone*>(window_2.get()));

  EXPECT_EQ(window_2.get(),
            window_manager()->window_mouse_currently_on_for_test());

  // Dispatch Event on window 1 while event is captured on window 2.
  ::testing::Mock::VerifyAndClearExpectations(&delegate_1);
  EXPECT_CALL(delegate_1, OnMouseEnter()).Times(1);
  window_2->SetCapture();
  ScopedXI2Event xi_event;
  xi_event.InitGenericButtonEvent(kPointerDeviceId, ET_MOUSE_PRESSED,
                                  gfx::Point(0, 0), EF_NONE);
  DispatchXEvent(xi_event, widget_1);
  EXPECT_EQ(window_1.get(),
            window_manager()->window_mouse_currently_on_for_test());

  // Removing the window should reset the |window_mouse_currently_on_|.
  window_1.reset();
  EXPECT_FALSE(window_manager()->window_mouse_currently_on_for_test());
}

}  // namespace ui
