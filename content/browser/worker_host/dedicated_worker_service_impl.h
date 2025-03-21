// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_WORKER_HOST_DEDICATED_WORKER_SERVICE_IMPL_H_
#define CONTENT_BROWSER_WORKER_HOST_DEDICATED_WORKER_SERVICE_IMPL_H_

#include "base/observer_list.h"
#include "content/public/browser/dedicated_worker_service.h"

namespace content {

class CONTENT_EXPORT DedicatedWorkerServiceImpl
    : public DedicatedWorkerService {
 public:
  DedicatedWorkerServiceImpl();
  ~DedicatedWorkerServiceImpl() override;

  DedicatedWorkerServiceImpl(const DedicatedWorkerServiceImpl& other) = delete;

  // DedicatedWorkerService:
  void AddObserver(Observer* observer) override;
  void RemoveObserver(Observer* observer) override;

  DedicatedWorkerId GenerateNextDedicatedWorkerId();

  void NotifyWorkerStarted(DedicatedWorkerId dedicated_worker_id,
                           int worker_process_id,
                           GlobalFrameRoutingId ancestor_render_frame_host_id);

  void NotifyWorkerTerminating(
      DedicatedWorkerId dedicated_worker_id,
      GlobalFrameRoutingId ancestor_render_frame_host_id);

 private:
  // Generates IDs for new dedicated workers.
  DedicatedWorkerId::Generator dedicated_worker_id_generator_;

  base::ObserverList<Observer> observers_;
};

}  // namespace content

#endif  // CONTENT_BROWSER_WORKER_HOST_DEDICATED_WORKER_SERVICE_IMPL_H_
