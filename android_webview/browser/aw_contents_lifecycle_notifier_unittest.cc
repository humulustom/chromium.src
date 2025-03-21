// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "android_webview/browser/aw_contents_lifecycle_notifier.h"

#include "base/task/post_task.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace android_webview {

class TestWebViewAppObserver : public WebViewAppStateObserver {
 public:
  TestWebViewAppObserver() = default;
  ~TestWebViewAppObserver() override = default;

  // WebViewAppStateObserver.
  void OnAppStateChanged(State state) override { state_ = state; }

  WebViewAppStateObserver::State state() const { return state_; }

 private:
  WebViewAppStateObserver::State state_;
};

class TestAwContentsLifecycleNotifier : public AwContentsLifecycleNotifier {
 public:
  TestAwContentsLifecycleNotifier() = default;
  ~TestAwContentsLifecycleNotifier() override = default;

  size_t GetAwContentsStateCount(AwContentsState state) const {
    return state_count_[ToIndex(state)];
  }

  bool HasAwContentsInstanceForTesting() const {
    return this->HasAwContentsInstance();
  }
};

class AwContentsLifecycleNotifierTest : public testing::Test {
 public:
  WebViewAppStateObserver::State GetState() const { return observer_->state(); }
  size_t GetAwContentsStateCount(
      AwContentsLifecycleNotifier::AwContentsState state) const {
    return notifier_->GetAwContentsStateCount(state);
  }

  bool HasAwContentsInstance() const {
    return notifier_->HasAwContentsInstanceForTesting();
  }

  bool HasAwContentsEverCreated() const {
    return notifier_->has_aw_contents_ever_created();
  }

  AwContentsLifecycleNotifier* notifier() { return notifier_.get(); }

  void VerifyAwContentsStateCount(size_t unknown_count,
                                  size_t foreground_count,
                                  size_t background_count) {
    ASSERT_EQ(GetAwContentsStateCount(
                  AwContentsLifecycleNotifier::AwContentsState::kDetached),
              unknown_count);
    ASSERT_EQ(GetAwContentsStateCount(
                  AwContentsLifecycleNotifier::AwContentsState::kForeground),
              foreground_count);
    ASSERT_EQ(GetAwContentsStateCount(
                  AwContentsLifecycleNotifier::AwContentsState::kBackground),
              background_count);
  }

 protected:
  // testing::Test.
  void SetUp() override {
    observer_ = std::make_unique<TestWebViewAppObserver>();
    notifier_ = std::make_unique<TestAwContentsLifecycleNotifier>();
    notifier_->AddObserver(observer_.get());
  }

  void TearDown() override { notifier_->RemoveObserver(observer_.get()); }

 private:
  content::BrowserTaskEnvironment task_environment_;
  std::unique_ptr<TestWebViewAppObserver> observer_;
  std::unique_ptr<TestAwContentsLifecycleNotifier> notifier_;
};

TEST_F(AwContentsLifecycleNotifierTest, Created) {
  const AwContents* fake_aw_contents = reinterpret_cast<const AwContents*>(1);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);
  ASSERT_FALSE(HasAwContentsEverCreated());
  ASSERT_FALSE(HasAwContentsInstance());

  notifier()->OnWebViewCreated(fake_aw_contents);
  VerifyAwContentsStateCount(1u, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kUnknown);
  ASSERT_TRUE(HasAwContentsInstance());
  ASSERT_TRUE(HasAwContentsEverCreated());

  notifier()->OnWebViewDestroyed(fake_aw_contents);
  VerifyAwContentsStateCount(0, 0, 0);
  ASSERT_FALSE(HasAwContentsInstance());
  ASSERT_TRUE(HasAwContentsEverCreated());
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);
}

TEST_F(AwContentsLifecycleNotifierTest, AttachToAndDetachFromWindow) {
  const AwContents* fake_aw_contents = reinterpret_cast<const AwContents*>(1);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);
  ASSERT_FALSE(HasAwContentsEverCreated());
  ASSERT_FALSE(HasAwContentsInstance());

  notifier()->OnWebViewCreated(fake_aw_contents);
  notifier()->OnWebViewAttachedToWindow(fake_aw_contents);
  VerifyAwContentsStateCount(0, 0, 1u);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kBackground);
  ASSERT_TRUE(HasAwContentsInstance());
  ASSERT_TRUE(HasAwContentsEverCreated());

  notifier()->OnWebViewDetachedFromWindow(fake_aw_contents);
  VerifyAwContentsStateCount(1u, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kUnknown);
  ASSERT_TRUE(HasAwContentsInstance());
  ASSERT_TRUE(HasAwContentsEverCreated());

  notifier()->OnWebViewDestroyed(fake_aw_contents);
  VerifyAwContentsStateCount(0, 0, 0);
  ASSERT_FALSE(HasAwContentsInstance());
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);
}

TEST_F(AwContentsLifecycleNotifierTest, WindowVisibleAndInvisible) {
  const AwContents* fake_aw_contents = reinterpret_cast<const AwContents*>(1);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);
  ASSERT_FALSE(HasAwContentsEverCreated());

  notifier()->OnWebViewCreated(fake_aw_contents);
  notifier()->OnWebViewAttachedToWindow(fake_aw_contents);
  notifier()->OnWebViewWindowBeVisible(fake_aw_contents);
  VerifyAwContentsStateCount(0, 1u, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kForeground);
  ASSERT_TRUE(HasAwContentsEverCreated());

  notifier()->OnWebViewWindowBeInvisible(fake_aw_contents);
  VerifyAwContentsStateCount(0, 0, 1u);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kBackground);

  notifier()->OnWebViewDetachedFromWindow(fake_aw_contents);
  VerifyAwContentsStateCount(1u, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kUnknown);

  notifier()->OnWebViewDestroyed(fake_aw_contents);
  VerifyAwContentsStateCount(0, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);
  ASSERT_TRUE(HasAwContentsEverCreated());
}

TEST_F(AwContentsLifecycleNotifierTest, MultipleAwContents) {
  const AwContents* fake_aw_contents1 = reinterpret_cast<const AwContents*>(1);
  const AwContents* fake_aw_contents2 = reinterpret_cast<const AwContents*>(2);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);
  ASSERT_FALSE(HasAwContentsEverCreated());

  notifier()->OnWebViewCreated(fake_aw_contents1);
  VerifyAwContentsStateCount(1u, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kUnknown);
  ASSERT_TRUE(HasAwContentsEverCreated());

  notifier()->OnWebViewAttachedToWindow(fake_aw_contents1);
  VerifyAwContentsStateCount(0, 0, 1u);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kBackground);

  notifier()->OnWebViewCreated(fake_aw_contents2);
  VerifyAwContentsStateCount(1u, 0, 1u);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kBackground);

  notifier()->OnWebViewAttachedToWindow(fake_aw_contents2);
  VerifyAwContentsStateCount(0, 0, 2u);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kBackground);

  notifier()->OnWebViewWindowBeVisible(fake_aw_contents2);
  VerifyAwContentsStateCount(0, 1u, 1u);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kForeground);

  notifier()->OnWebViewWindowBeVisible(fake_aw_contents1);
  VerifyAwContentsStateCount(0, 2u, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kForeground);

  notifier()->OnWebViewDestroyed(fake_aw_contents2);
  VerifyAwContentsStateCount(0, 1u, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kForeground);

  notifier()->OnWebViewWindowBeInvisible(fake_aw_contents1);
  VerifyAwContentsStateCount(0, 0, 1u);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kBackground);

  notifier()->OnWebViewDetachedFromWindow(fake_aw_contents1);
  VerifyAwContentsStateCount(1u, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kUnknown);

  notifier()->OnWebViewDestroyed(fake_aw_contents1);
  VerifyAwContentsStateCount(0, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kDestroyed);

  notifier()->OnWebViewCreated(fake_aw_contents1);
  VerifyAwContentsStateCount(1u, 0, 0);
  ASSERT_EQ(GetState(), WebViewAppStateObserver::State::kUnknown);
  ASSERT_TRUE(HasAwContentsEverCreated());
}

}  // namespace android_webview
