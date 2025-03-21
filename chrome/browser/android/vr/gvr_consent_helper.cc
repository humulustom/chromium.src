// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/vr/gvr_consent_helper.h"

#include <utility>

#include "base/android/jni_string.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/macros.h"
#include "chrome/android/features/vr/jni_headers/VrConsentDialog_jni.h"
#include "chrome/browser/android/vr/android_vr_utils.h"

using base::android::AttachCurrentThread;

namespace vr {

GvrConsentHelper::GvrConsentHelper() : XrConsentHelper() {}

GvrConsentHelper::~GvrConsentHelper() {
  if (!jdelegate_.is_null()) {
    Java_VrConsentDialog_onNativeDestroy(AttachCurrentThread(), jdelegate_);
  }
}

void GvrConsentHelper::ShowConsentPrompt(
    int render_process_id,
    int render_frame_id,
    XrConsentPromptLevel consent_level,
    OnUserConsentCallback on_user_consent_callback) {
  DCHECK(!on_user_consent_callback_);
  on_user_consent_callback_ = std::move(on_user_consent_callback);
  render_process_id_ = render_process_id;
  render_frame_id_ = render_frame_id;
  consent_level_ = consent_level;

  JNIEnv* env = AttachCurrentThread();
  jdelegate_ = Java_VrConsentDialog_promptForUserConsent(
      env, reinterpret_cast<jlong>(this),
      GetTabFromRenderer(render_process_id_, render_frame_id_),
      static_cast<jint>(consent_level));
  if (jdelegate_.is_null()) {
    std::move(on_user_consent_callback_).Run(consent_level_, false);
    return;
  }
}

void GvrConsentHelper::OnUserConsentResult(JNIEnv* env, jboolean is_granted) {
  jdelegate_.Reset();
  if (!on_user_consent_callback_)
    return;

  if (!is_granted) {
    std::move(on_user_consent_callback_).Run(consent_level_, false);
    return;
  }

  // Now that we know consent was granted, check if the VRModule is installed,
  // and install it if not. Treat failing to install the module the same as if
  // consent were denied.
  if (!module_delegate_) {
    module_delegate_ = VrModuleProviderFactory::CreateModuleProvider(
        render_process_id_, render_frame_id_);
  }

  if (!module_delegate_) {
    std::move(on_user_consent_callback_).Run(consent_level_, false);
    return;
  }

  if (!module_delegate_->ModuleInstalled()) {
    module_delegate_->InstallModule(base::BindOnce(
        &GvrConsentHelper::OnModuleInstalled, weak_ptr_.GetWeakPtr()));
    return;
  }

  std::move(on_user_consent_callback_).Run(consent_level_, true);
}

void GvrConsentHelper::OnModuleInstalled(bool success) {
  if (!success) {
    std::move(on_user_consent_callback_).Run(consent_level_, false);
    return;
  }

  std::move(on_user_consent_callback_).Run(consent_level_, true);
}

}  // namespace vr
