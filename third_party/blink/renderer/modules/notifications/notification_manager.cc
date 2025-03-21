// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/notifications/notification_manager.h"

#include <utility>

#include "base/numerics/safe_conversions.h"
#include "third_party/blink/public/common/browser_interface_broker_proxy.h"
#include "third_party/blink/public/mojom/notifications/notification.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/modules/notifications/notification.h"
#include "third_party/blink/renderer/modules/permissions/permission_utils.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/heap/heap_allocator.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/instrumentation/histogram.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

// static
NotificationManager* NotificationManager::From(ExecutionContext* context) {
  DCHECK(context);
  DCHECK(context->IsContextThread());

  NotificationManager* manager =
      Supplement<ExecutionContext>::From<NotificationManager>(context);
  if (!manager) {
    manager = MakeGarbageCollected<NotificationManager>(*context);
    Supplement<ExecutionContext>::ProvideTo(*context, manager);
  }

  return manager;
}

// static
const char NotificationManager::kSupplementName[] = "NotificationManager";

NotificationManager::NotificationManager(ExecutionContext& context)
    : Supplement<ExecutionContext>(context) {}

NotificationManager::~NotificationManager() = default;

mojom::blink::PermissionStatus NotificationManager::GetPermissionStatus() {
  if (GetSupplementable()->IsContextDestroyed())
    return mojom::blink::PermissionStatus::DENIED;

  mojom::blink::PermissionStatus permission_status;
  if (!GetNotificationService()->GetPermissionStatus(&permission_status)) {
    NOTREACHED();
    return mojom::blink::PermissionStatus::DENIED;
  }

  return permission_status;
}

ScriptPromise NotificationManager::RequestPermission(
    ScriptState* script_state,
    V8NotificationPermissionCallback* deprecated_callback) {
  ExecutionContext* context = ExecutionContext::From(script_state);

  if (!permission_service_) {
    // See https://bit.ly/2S0zRAS for task types
    scoped_refptr<base::SingleThreadTaskRunner> task_runner =
        context->GetTaskRunner(TaskType::kMiscPlatformAPI);
    ConnectToPermissionService(
        context,
        permission_service_.BindNewPipeAndPassReceiver(std::move(task_runner)));
    permission_service_.set_disconnect_handler(
        WTF::Bind(&NotificationManager::OnPermissionServiceConnectionError,
                  WrapWeakPersistent(this)));
  }

  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  ScriptPromise promise = resolver->Promise();

  Document* doc = DynamicTo<Document>(context);
  permission_service_->RequestPermission(
      CreatePermissionDescriptor(mojom::blink::PermissionName::NOTIFICATIONS),
      LocalFrame::HasTransientUserActivation(doc ? doc->GetFrame() : nullptr),
      WTF::Bind(&NotificationManager::OnPermissionRequestComplete,
                WrapPersistent(this), WrapPersistent(resolver),
                WrapPersistent(deprecated_callback)));

  return promise;
}

void NotificationManager::OnPermissionRequestComplete(
    ScriptPromiseResolver* resolver,
    V8NotificationPermissionCallback* deprecated_callback,
    mojom::blink::PermissionStatus status) {
  String status_string = Notification::PermissionString(status);
  if (deprecated_callback)
    deprecated_callback->InvokeAndReportException(nullptr, status_string);

  resolver->Resolve(status_string);
}

void NotificationManager::OnNotificationServiceConnectionError() {
  notification_service_.reset();
}

void NotificationManager::OnPermissionServiceConnectionError() {
  permission_service_.reset();
}

void NotificationManager::DisplayNonPersistentNotification(
    const String& token,
    mojom::blink::NotificationDataPtr notification_data,
    mojom::blink::NotificationResourcesPtr notification_resources,
    mojo::PendingRemote<mojom::blink::NonPersistentNotificationListener>
        event_listener) {
  DCHECK(!token.IsEmpty());
  DCHECK(notification_resources);
  GetNotificationService()->DisplayNonPersistentNotification(
      token, std::move(notification_data), std::move(notification_resources),
      std::move(event_listener));
}

void NotificationManager::CloseNonPersistentNotification(const String& token) {
  DCHECK(!token.IsEmpty());
  GetNotificationService()->CloseNonPersistentNotification(token);
}

void NotificationManager::DisplayPersistentNotification(
    int64_t service_worker_registration_id,
    mojom::blink::NotificationDataPtr notification_data,
    mojom::blink::NotificationResourcesPtr notification_resources,
    ScriptPromiseResolver* resolver) {
  DCHECK(notification_data);
  DCHECK(notification_resources);
  DCHECK_EQ(notification_data->actions.has_value()
                ? notification_data->actions->size()
                : 0,
            notification_resources->action_icons.has_value()
                ? notification_resources->action_icons->size()
                : 0);

  // Verify that the author-provided payload size does not exceed our limit.
  // This is an implementation-defined limit to prevent abuse of notification
  // data as a storage mechanism. A UMA histogram records the requested sizes,
  // which enables us to track how much data authors are attempting to store.
  //
  // If the size exceeds this limit, reject the showNotification() promise. This
  // is outside of the boundaries set by the specification, but it gives authors
  // an indication that something has gone wrong.
  size_t author_data_size =
      notification_data->data.has_value() ? notification_data->data->size() : 0;

  DEFINE_THREAD_SAFE_STATIC_LOCAL(
      CustomCountHistogram, notification_data_size_histogram,
      ("Notifications.AuthorDataSize", 1, 1000, 50));
  notification_data_size_histogram.Count(
      base::saturated_cast<base::HistogramBase::Sample>(author_data_size));

  if (author_data_size >
      mojom::blink::NotificationData::kMaximumDeveloperDataSize) {
    resolver->Reject();
    return;
  }

  GetNotificationService()->DisplayPersistentNotification(
      service_worker_registration_id, std::move(notification_data),
      std::move(notification_resources),
      WTF::Bind(&NotificationManager::DidDisplayPersistentNotification,
                WrapPersistent(this), WrapPersistent(resolver)));
}

void NotificationManager::DidDisplayPersistentNotification(
    ScriptPromiseResolver* resolver,
    mojom::blink::PersistentNotificationError error) {
  switch (error) {
    case mojom::blink::PersistentNotificationError::NONE:
      resolver->Resolve();
      return;
    case mojom::blink::PersistentNotificationError::INTERNAL_ERROR:
    case mojom::blink::PersistentNotificationError::PERMISSION_DENIED:
      // TODO(https://crbug.com/832944): Throw a TypeError if permission denied.
      resolver->Reject();
      return;
  }
  NOTREACHED();
}

void NotificationManager::ClosePersistentNotification(
    const WebString& notification_id) {
  GetNotificationService()->ClosePersistentNotification(notification_id);
}

void NotificationManager::GetNotifications(
    int64_t service_worker_registration_id,
    const WebString& filter_tag,
    bool include_triggered,
    ScriptPromiseResolver* resolver) {
  GetNotificationService()->GetNotifications(
      service_worker_registration_id, filter_tag, include_triggered,
      WTF::Bind(&NotificationManager::DidGetNotifications, WrapPersistent(this),
                WrapPersistent(resolver)));
}

void NotificationManager::DidGetNotifications(
    ScriptPromiseResolver* resolver,
    const Vector<String>& notification_ids,
    Vector<mojom::blink::NotificationDataPtr> notification_datas) {
  DCHECK_EQ(notification_ids.size(), notification_datas.size());
  ExecutionContext* context = resolver->GetExecutionContext();
  if (!context)
    return;

  HeapVector<Member<Notification>> notifications;
  notifications.ReserveInitialCapacity(notification_ids.size());

  for (wtf_size_t i = 0; i < notification_ids.size(); ++i) {
    notifications.push_back(Notification::Create(
        context, notification_ids[i], std::move(notification_datas[i]),
        true /* showing */));
  }

  resolver->Resolve(notifications);
}

const mojo::Remote<mojom::blink::NotificationService>&
NotificationManager::GetNotificationService() {
  if (!notification_service_) {
    // See https://bit.ly/2S0zRAS for task types
    scoped_refptr<base::SingleThreadTaskRunner> task_runner =
        GetSupplementable()->GetTaskRunner(TaskType::kMiscPlatformAPI);
    GetSupplementable()->GetBrowserInterfaceBroker().GetInterface(
        notification_service_.BindNewPipeAndPassReceiver(task_runner));

    notification_service_.set_disconnect_handler(
        WTF::Bind(&NotificationManager::OnNotificationServiceConnectionError,
                  WrapWeakPersistent(this)));
  }

  return notification_service_;
}

void NotificationManager::Trace(blink::Visitor* visitor) {
  Supplement<ExecutionContext>::Trace(visitor);
}

}  // namespace blink
