// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_MEDIA_MEDIA_ENGAGEMENT_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MEDIA_MEDIA_ENGAGEMENT_UI_H_

#include <memory>

#include "base/macros.h"
#include "chrome/browser/media/media_engagement_score_details.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

// The UI for chrome://media-engagement/.
class MediaEngagementUI : public ui::MojoWebUIController {
 public:
  explicit MediaEngagementUI(content::WebUI* web_ui);
  ~MediaEngagementUI() override;

  // Instantiates the implementor of the MediaEngagementScoreDetailsProvider
  // mojo interface passing the pending receiver that will be internally bound.
  void BindInterface(
      mojo::PendingReceiver<media::mojom::MediaEngagementScoreDetailsProvider>
          receiver);

 private:
  std::unique_ptr<media::mojom::MediaEngagementScoreDetailsProvider>
      ui_handler_;

  WEB_UI_CONTROLLER_TYPE_DECL();

  DISALLOW_COPY_AND_ASSIGN(MediaEngagementUI);
};

#endif  // CHROME_BROWSER_UI_WEBUI_MEDIA_MEDIA_ENGAGEMENT_UI_H_
