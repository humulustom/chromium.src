// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/preferences/cookie_controls_bridge.h"

#include <memory>
#include "chrome/android/chrome_jni_headers/CookieControlsBridge_jni.h"

using base::android::JavaParamRef;

CookieControlsBridge::CookieControlsBridge(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    const base::android::JavaParamRef<jobject>& jweb_contents_android)
    : jobject_(obj) {
  content::WebContents* web_contents =
      content::WebContents::FromJavaWebContents(jweb_contents_android);
  controller_ = std::make_unique<CookieControlsController>(web_contents);
  observer_.Add(controller_.get());
  controller_->Update(web_contents);
}

void CookieControlsBridge::OnStatusChanged(
    CookieControlsController::Status new_status,
    int blocked_cookies) {
  if (status_ != new_status) {
    status_ = new_status;
    JNIEnv* env = base::android::AttachCurrentThread();
    // Only call status callback if status has changed
    Java_CookieControlsBridge_onCookieBlockingStatusChanged(
        env, jobject_, static_cast<int>(status_));
  }

  OnBlockedCookiesCountChanged(blocked_cookies);
}

void CookieControlsBridge::OnBlockedCookiesCountChanged(int blocked_cookies) {
  // The blocked cookie count changes quite frequently, so avoid unnecessary
  // UI updates if possible.
  if (blocked_cookies_ == blocked_cookies)
    return;

  blocked_cookies_ = blocked_cookies;
  JNIEnv* env = base::android::AttachCurrentThread();
  Java_CookieControlsBridge_onBlockedCookiesCountChanged(
      env, jobject_, blocked_cookies_.value_or(0));
}

CookieControlsBridge::~CookieControlsBridge() = default;

void CookieControlsBridge::Destroy(JNIEnv* env,
                                   const JavaParamRef<jobject>& obj) {
  delete this;
}

static jlong JNI_CookieControlsBridge_Init(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    const base::android::JavaParamRef<jobject>& jweb_contents_android) {
  return reinterpret_cast<intptr_t>(
      new CookieControlsBridge(env, obj, jweb_contents_android));
}
