// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/installedapp/installed_app_provider_impl.h"
#include "build/build_config.h"
#include "content/browser/installedapp/installed_app_provider_impl_win.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/content_features.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

namespace content {

InstalledAppProviderImpl::InstalledAppProviderImpl(
    RenderFrameHost* render_frame_host)
    : render_frame_host_(render_frame_host) {
  DCHECK(render_frame_host_);
}

void InstalledAppProviderImpl::FilterInstalledApps(
    std::vector<blink::mojom::RelatedApplicationPtr> related_apps,
    const GURL& manifest_url,
    FilterInstalledAppsCallback callback) {
  if (render_frame_host_->GetProcess()->GetBrowserContext()->IsOffTheRecord()) {
    std::move(callback).Run(std::vector<blink::mojom::RelatedApplicationPtr>());
    return;
  }

  bool is_implemented = false;
  if (base::FeatureList::IsEnabled(features::kInstalledAppProvider)) {
#if defined(OS_WIN)
    is_implemented = true;
    installed_app_provider_win::FilterInstalledAppsForWin(
        std::move(related_apps), std::move(callback), render_frame_host_);
#endif
  }

  if (!is_implemented) {
    // Do not return any results.
    std::move(callback).Run(std::vector<blink::mojom::RelatedApplicationPtr>());
  }
}

// static
void InstalledAppProviderImpl::Create(
    RenderFrameHost* host,
    mojo::PendingReceiver<blink::mojom::InstalledAppProvider> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<InstalledAppProviderImpl>(host),
                              std::move(receiver));
}

}  // namespace content
