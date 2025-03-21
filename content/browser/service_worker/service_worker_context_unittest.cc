// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/service_worker_context.h"

#include <stdint.h>

#include "base/bind.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/scoped_observer.h"
#include "base/time/time.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/fake_embedded_worker_instance_client.h"
#include "content/browser/service_worker/service_worker_container_host.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_context_core_observer.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_metrics.h"
#include "content/browser/service_worker/service_worker_provider_host.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_storage.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/public/browser/service_worker_context_observer.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/service_worker/service_worker_types.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_event_status.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"

namespace content {

namespace {

void SaveResponseCallback(bool* called,
                          int64_t* store_registration_id,
                          blink::ServiceWorkerStatusCode status,
                          const std::string& status_message,
                          int64_t registration_id) {
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status)
      << blink::ServiceWorkerStatusToString(status);
  *called = true;
  *store_registration_id = registration_id;
}

ServiceWorkerContextCore::RegistrationCallback MakeRegisteredCallback(
    bool* called,
    int64_t* store_registration_id) {
  return base::BindOnce(&SaveResponseCallback, called, store_registration_id);
}

void RegisteredCallback(base::OnceClosure quit_closure,
                        blink::ServiceWorkerStatusCode status,
                        const std::string& status_message,
                        int64_t registration_id) {
  std::move(quit_closure).Run();
}

void CallCompletedCallback(bool* called, blink::ServiceWorkerStatusCode) {
  *called = true;
}

ServiceWorkerContextCore::UnregistrationCallback MakeUnregisteredCallback(
    bool* called) {
  return base::BindOnce(&CallCompletedCallback, called);
}

void ExpectRegisteredWorkers(
    blink::ServiceWorkerStatusCode expect_status,
    bool expect_waiting,
    bool expect_active,
    blink::ServiceWorkerStatusCode status,
    scoped_refptr<ServiceWorkerRegistration> registration) {
  ASSERT_EQ(expect_status, status);
  if (status != blink::ServiceWorkerStatusCode::kOk) {
    EXPECT_FALSE(registration.get());
    return;
  }

  if (expect_waiting) {
    EXPECT_TRUE(registration->waiting_version());
  } else {
    EXPECT_FALSE(registration->waiting_version());
  }

  if (expect_active) {
    EXPECT_TRUE(registration->active_version());
  } else {
    EXPECT_FALSE(registration->active_version());
  }

  EXPECT_EQ(blink::mojom::ServiceWorkerUpdateViaCache::kImports,
            registration->update_via_cache());
}

class InstallActivateWorker : public FakeServiceWorker {
 public:
  explicit InstallActivateWorker(EmbeddedWorkerTestHelper* helper)
      : FakeServiceWorker(helper) {}
  ~InstallActivateWorker() override = default;

  const std::vector<ServiceWorkerMetrics::EventType>& events() const {
    return events_;
  }

  void SetToRejectInstall() { reject_install_ = true; }

  void SetToRejectActivate() { reject_activate_ = true; }

  void DispatchInstallEvent(
      blink::mojom::ServiceWorker::DispatchInstallEventCallback callback)
      override {
    events_.emplace_back(ServiceWorkerMetrics::EventType::INSTALL);
    std::move(callback).Run(
        reject_install_ ? blink::mojom::ServiceWorkerEventStatus::REJECTED
                        : blink::mojom::ServiceWorkerEventStatus::COMPLETED,
        true /* has_fetch_handler */);
  }

  void DispatchActivateEvent(
      blink::mojom::ServiceWorker::DispatchActivateEventCallback callback)
      override {
    events_.emplace_back(ServiceWorkerMetrics::EventType::ACTIVATE);
    std::move(callback).Run(
        reject_activate_ ? blink::mojom::ServiceWorkerEventStatus::REJECTED
                         : blink::mojom::ServiceWorkerEventStatus::COMPLETED);
  }

  void OnConnectionError() override {
    // Do nothing. This allows the object to stay until the test is over, so
    // |events_| can be accessed even after the worker is stopped in the case of
    // rejected install.
  }

 private:
  std::vector<ServiceWorkerMetrics::EventType> events_;
  bool reject_install_ = false;
  bool reject_activate_ = false;
};

enum NotificationType {
  REGISTRATION_COMPLETED,
  REGISTRATION_STORED,
  REGISTRATION_DELETED,
  STORAGE_RECOVERED,
};

struct NotificationLog {
  NotificationType type;
  GURL scope;
  int64_t registration_id;
};

}  // namespace

class ServiceWorkerContextTest : public ServiceWorkerContextCoreObserver,
                                 public testing::Test {
 public:
  ServiceWorkerContextTest()
      : task_environment_(BrowserTaskEnvironment::IO_MAINLOOP) {}

  void SetUp() override {
    helper_.reset(new EmbeddedWorkerTestHelper(base::FilePath()));
    helper_->context_wrapper()->AddObserver(this);
  }

  void TearDown() override {
    helper_.reset();
    // The helper may post tasks to release resources in |temp_dir_|. Allow
    // them to run now so that the directory may be deleted.
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(!temp_dir_.IsValid() || temp_dir_.Delete())
        << temp_dir_.GetPath();
  }

  // ServiceWorkerContextCoreObserver overrides.
  void OnRegistrationCompleted(int64_t registration_id,
                               const GURL& scope) override {
    NotificationLog log;
    log.type = REGISTRATION_COMPLETED;
    log.scope = scope;
    log.registration_id = registration_id;
    notifications_.push_back(log);
  }
  void OnRegistrationStored(int64_t registration_id,
                            const GURL& scope) override {
    NotificationLog log;
    log.type = REGISTRATION_STORED;
    log.scope = scope;
    log.registration_id = registration_id;
    notifications_.push_back(log);
  }
  void OnRegistrationDeleted(int64_t registration_id,
                             const GURL& scope) override {
    NotificationLog log;
    log.type = REGISTRATION_DELETED;
    log.scope = scope;
    log.registration_id = registration_id;
    notifications_.push_back(log);
  }
  void OnStorageWiped() override {
    NotificationLog log;
    log.type = STORAGE_RECOVERED;
    notifications_.push_back(log);
  }

  ServiceWorkerContextCore* context() { return helper_->context(); }
  ServiceWorkerContextWrapper* context_wrapper() {
    return helper_->context_wrapper();
  }
  void GetTemporaryDirectory(base::FilePath* temp_dir) {
    ASSERT_FALSE(temp_dir_.IsValid());
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    *temp_dir = temp_dir_.GetPath();
  }

 protected:
  base::ScopedTempDir temp_dir_;
  BrowserTaskEnvironment task_environment_;
  std::unique_ptr<EmbeddedWorkerTestHelper> helper_;
  std::vector<NotificationLog> notifications_;
};

class RecordableEmbeddedWorkerInstanceClient
    : public FakeEmbeddedWorkerInstanceClient {
 public:
  enum class Message { StartWorker, StopWorker };

  explicit RecordableEmbeddedWorkerInstanceClient(
      EmbeddedWorkerTestHelper* helper)
      : FakeEmbeddedWorkerInstanceClient(helper) {}

  void OnConnectionError() override {
    // Do nothing. This allows the object to stay until the test is over, so
    // |events_| can be accessed even after the worker is stopped in the case of
    // rejected install.
  }

  const std::vector<Message>& events() const { return events_; }

 protected:
  void StartWorker(blink::mojom::EmbeddedWorkerStartParamsPtr params) override {
    events_.push_back(Message::StartWorker);
    FakeEmbeddedWorkerInstanceClient::StartWorker(std::move(params));
  }

  void StopWorker() override {
    events_.push_back(Message::StopWorker);

    // Let stop complete, but don't call the base class's StopWorker(), which
    // would destroy |this| before the test can retrieve events().
    host()->OnStopped();
  }

 private:
  std::vector<Message> events_;
  DISALLOW_COPY_AND_ASSIGN(RecordableEmbeddedWorkerInstanceClient);
};

class TestServiceWorkerContextObserver : public ServiceWorkerContextObserver {
 public:
  enum class EventType {
    RegistrationCompleted,
    RegistrationStored,
    VersionActivated,
    VersionRedundant,
    NoControllees,
    VersionStartedRunning,
    VersionStoppedRunning,
    Destruct
  };
  struct EventLog {
    EventType type;
    base::Optional<GURL> url;
    base::Optional<int64_t> version_id;
    base::Optional<int64_t> registration_id;
    base::Optional<bool> is_running;
  };

  explicit TestServiceWorkerContextObserver(ServiceWorkerContext* context)
      : scoped_observer_(this) {
    scoped_observer_.Add(context);
  }

  ~TestServiceWorkerContextObserver() override = default;

  void OnRegistrationCompleted(const GURL& scope) override {
    EventLog log;
    log.type = EventType::RegistrationCompleted;
    log.url = scope;
    events_.push_back(log);
  }

  void OnRegistrationStored(int64_t registration_id,
                            const GURL& scope) override {
    EventLog log;
    log.type = EventType::RegistrationStored;
    log.registration_id = registration_id;
    log.url = scope;
    events_.push_back(log);
  }

  void OnVersionActivated(int64_t version_id, const GURL& scope) override {
    EventLog log;
    log.type = EventType::VersionActivated;
    log.version_id = version_id;
    log.url = scope;
    events_.push_back(log);
  }

  void OnVersionRedundant(int64_t version_id, const GURL& scope) override {
    EventLog log;
    log.type = EventType::VersionRedundant;
    log.version_id = version_id;
    log.url = scope;
    events_.push_back(log);
  }

  void OnNoControllees(int64_t version_id, const GURL& scope) override {
    EventLog log;
    log.type = EventType::NoControllees;
    log.version_id = version_id;
    log.url = scope;
    events_.push_back(log);
  }

  void OnVersionStartedRunning(
      content::ServiceWorkerContext* context,
      int64_t version_id,
      const ServiceWorkerRunningInfo& running_info) override {
    EventLog log;
    log.type = EventType::VersionStartedRunning;
    log.version_id = version_id;
    events_.push_back(log);
  }

  void OnVersionStoppedRunning(content::ServiceWorkerContext* context,
                               int64_t version_id) override {
    EventLog log;
    log.type = EventType::VersionStoppedRunning;
    log.version_id = version_id;
    events_.push_back(log);
  }

  void OnDestruct(content::ServiceWorkerContext* context) override {
    scoped_observer_.Remove(context);

    EventLog log;
    log.type = EventType::Destruct;
    events_.push_back(log);
  }

  const std::vector<EventLog>& events() { return events_; }

 private:
  ScopedObserver<ServiceWorkerContext, ServiceWorkerContextObserver>
      scoped_observer_;
  std::vector<EventLog> events_;
  DISALLOW_COPY_AND_ASSIGN(TestServiceWorkerContextObserver);
};

// Make sure OnRegistrationCompleted is called on observer.
TEST_F(ServiceWorkerContextTest, RegistrationCompletedObserver) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  TestServiceWorkerContextObserver observer(context_wrapper());

  int64_t registration_id = blink::mojom::kInvalidServiceWorkerRegistrationId;
  bool called = false;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &registration_id));
  base::RunLoop().RunUntilIdle();

  ASSERT_TRUE(called);
  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId, registration_id);

  std::vector<TestServiceWorkerContextObserver::EventLog> events;

  // Filter the events to be verified.
  for (auto event : observer.events()) {
    if (event.type == TestServiceWorkerContextObserver::EventType::
                          RegistrationCompleted ||
        event.type ==
            TestServiceWorkerContextObserver::EventType::RegistrationStored ||
        event.type ==
            TestServiceWorkerContextObserver::EventType::VersionActivated)
      events.push_back(event);
  }
  ASSERT_EQ(3u, events.size());
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::RegistrationCompleted,
            events[0].type);
  EXPECT_EQ(scope, events[0].url);
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::RegistrationStored,
            events[1].type);
  EXPECT_EQ(scope, events[1].url);
  EXPECT_EQ(registration_id, events[1].registration_id);
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::VersionActivated,
            events[2].type);
  EXPECT_EQ(scope, events[2].url);
}

// Make sure OnNoControllees is called on observer.
TEST_F(ServiceWorkerContextTest, NoControlleesObserver) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  auto registration = base::MakeRefCounted<ServiceWorkerRegistration>(
      options, 1l /* dummy registration id */, context()->AsWeakPtr());

  auto version = base::MakeRefCounted<ServiceWorkerVersion>(
      registration.get(), script_url, blink::mojom::ScriptType::kClassic,
      2l /* dummy version id */, context()->AsWeakPtr());
  version->set_fetch_handler_existence(
      ServiceWorkerVersion::FetchHandlerExistence::EXISTS);
  version->SetStatus(ServiceWorkerVersion::ACTIVATED);

  ServiceWorkerRemoteProviderEndpoint endpoint;
  base::WeakPtr<ServiceWorkerContainerHost> container_host =
      CreateContainerHostForWindow(helper_->mock_render_process_id(), true,
                                   context()->AsWeakPtr(), &endpoint);

  version->AddControllee(container_host.get());
  base::RunLoop().RunUntilIdle();

  TestServiceWorkerContextObserver observer(context_wrapper());

  version->RemoveControllee(container_host->client_uuid());
  base::RunLoop().RunUntilIdle();

  ASSERT_EQ(1u, observer.events().size());
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::NoControllees,
            observer.events()[0].type);
  EXPECT_EQ(scope, observer.events()[0].url);
  EXPECT_EQ(2l, observer.events()[0].version_id);
}

// Make sure OnVersionActivated is called on observer.
TEST_F(ServiceWorkerContextTest, VersionActivatedObserver) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  auto registration = base::MakeRefCounted<ServiceWorkerRegistration>(
      options, 1l /* dummy registration id */, context()->AsWeakPtr());

  auto version = base::MakeRefCounted<ServiceWorkerVersion>(
      registration.get(), script_url, blink::mojom::ScriptType::kClassic,
      2l /* dummy version id */, context()->AsWeakPtr());

  TestServiceWorkerContextObserver observer(context_wrapper());

  version->set_fetch_handler_existence(
      ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST);
  version->SetStatus(ServiceWorkerVersion::Status::ACTIVATED);
  base::RunLoop().RunUntilIdle();

  ASSERT_EQ(1u, observer.events().size());
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::VersionActivated,
            observer.events()[0].type);
  EXPECT_EQ(2l, observer.events()[0].version_id);
}

// Make sure OnVersionRedundant is called on observer.
TEST_F(ServiceWorkerContextTest, VersionRedundantObserver) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  auto registration = base::MakeRefCounted<ServiceWorkerRegistration>(
      options, 1l /* dummy registration id */, context()->AsWeakPtr());

  auto version = base::MakeRefCounted<ServiceWorkerVersion>(
      registration.get(), script_url, blink::mojom::ScriptType::kClassic,
      2l /* dummy version id */, context()->AsWeakPtr());

  TestServiceWorkerContextObserver observer(context_wrapper());

  version->set_fetch_handler_existence(
      ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST);
  version->SetStatus(ServiceWorkerVersion::Status::REDUNDANT);
  base::RunLoop().RunUntilIdle();

  ASSERT_EQ(1u, observer.events().size());
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::VersionRedundant,
            observer.events()[0].type);
  EXPECT_EQ(2l, observer.events()[0].version_id);
}

// Make sure OnVersionStartedRunning and OnVersionStoppedRunning are called on
// observer.
TEST_F(ServiceWorkerContextTest, OnVersionRunningStatusChangedObserver) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  TestServiceWorkerContextObserver observer(context_wrapper());
  base::RunLoop run_loop;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      base::BindOnce(&RegisteredCallback, run_loop.QuitClosure()));
  run_loop.Run();

  context_wrapper()->StopAllServiceWorkersForOrigin(scope);
  base::RunLoop().RunUntilIdle();

  std::vector<TestServiceWorkerContextObserver::EventLog> events;

  // Filter the events to be verified.
  for (auto event : observer.events()) {
    if (event.type == TestServiceWorkerContextObserver::EventType::
                          VersionStartedRunning ||
        event.type == TestServiceWorkerContextObserver::EventType::
                          VersionStoppedRunning) {
      events.push_back(event);
    }
  }

  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::VersionStartedRunning,
            events[0].type);
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::VersionStoppedRunning,
            events[1].type);
  EXPECT_EQ(events[0].version_id, events[1].version_id);
}

// Make sure OnDestruct is called on observer.
TEST_F(ServiceWorkerContextTest, OnDestructObserver) {
  ServiceWorkerContextWrapper* context = context_wrapper();
  TestServiceWorkerContextObserver observer(context);
  helper_->ShutdownContext();
  base::RunLoop().RunUntilIdle();

  ASSERT_EQ(1u, observer.events().size());
  EXPECT_EQ(TestServiceWorkerContextObserver::EventType::Destruct,
            observer.events()[0].type);
}

// Make sure basic registration is working.
TEST_F(ServiceWorkerContextTest, Register) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  auto* client =
      helper_
          ->AddNewPendingInstanceClient<RecordableEmbeddedWorkerInstanceClient>(
              helper_.get());
  auto* worker =
      helper_->AddNewPendingServiceWorker<InstallActivateWorker>(helper_.get());

  int64_t registration_id = blink::mojom::kInvalidServiceWorkerRegistrationId;
  bool called = false;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);

  ASSERT_EQ(2u, worker->events().size());
  ASSERT_EQ(1u, client->events().size());
  EXPECT_EQ(RecordableEmbeddedWorkerInstanceClient::Message::StartWorker,
            client->events()[0]);
  EXPECT_EQ(ServiceWorkerMetrics::EventType::INSTALL, worker->events()[0]);
  EXPECT_EQ(ServiceWorkerMetrics::EventType::ACTIVATE, worker->events()[1]);

  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId, registration_id);

  ASSERT_EQ(2u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(scope, notifications_[0].scope);
  EXPECT_EQ(registration_id, notifications_[0].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[1].type);
  EXPECT_EQ(scope, notifications_[1].scope);
  EXPECT_EQ(registration_id, notifications_[1].registration_id);

  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kOk,
                     false /* expect_waiting */, true /* expect_active */));
  base::RunLoop().RunUntilIdle();
}

// Test registration when the service worker rejects the install event. The
// registration callback should indicate success, but there should be no waiting
// or active worker in the registration.
TEST_F(ServiceWorkerContextTest, Register_RejectInstall) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  auto* client =
      helper_
          ->AddNewPendingInstanceClient<RecordableEmbeddedWorkerInstanceClient>(
              helper_.get());
  auto* worker =
      helper_->AddNewPendingServiceWorker<InstallActivateWorker>(helper_.get());
  worker->SetToRejectInstall();

  int64_t registration_id = blink::mojom::kInvalidServiceWorkerRegistrationId;
  bool called = false;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);

  ASSERT_EQ(1u, worker->events().size());
  ASSERT_EQ(2u, client->events().size());
  EXPECT_EQ(RecordableEmbeddedWorkerInstanceClient::Message::StartWorker,
            client->events()[0]);
  EXPECT_EQ(ServiceWorkerMetrics::EventType::INSTALL, worker->events()[0]);
  EXPECT_EQ(RecordableEmbeddedWorkerInstanceClient::Message::StopWorker,
            client->events()[1]);

  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId, registration_id);

  ASSERT_EQ(1u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(scope, notifications_[0].scope);
  EXPECT_EQ(registration_id, notifications_[0].registration_id);

  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kErrorNotFound,
                     false /* expect_waiting */, false /* expect_active */));
  base::RunLoop().RunUntilIdle();
}

// Test registration when the service worker rejects the activate event. The
// worker should be activated anyway.
TEST_F(ServiceWorkerContextTest, Register_RejectActivate) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  auto* client =
      helper_
          ->AddNewPendingInstanceClient<RecordableEmbeddedWorkerInstanceClient>(
              helper_.get());
  auto* worker =
      helper_->AddNewPendingServiceWorker<InstallActivateWorker>(helper_.get());
  worker->SetToRejectActivate();

  int64_t registration_id = blink::mojom::kInvalidServiceWorkerRegistrationId;
  bool called = false;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);

  ASSERT_EQ(2u, worker->events().size());
  ASSERT_EQ(1u, client->events().size());
  EXPECT_EQ(RecordableEmbeddedWorkerInstanceClient::Message::StartWorker,
            client->events()[0]);
  EXPECT_EQ(ServiceWorkerMetrics::EventType::INSTALL, worker->events()[0]);
  EXPECT_EQ(ServiceWorkerMetrics::EventType::ACTIVATE, worker->events()[1]);

  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId, registration_id);

  ASSERT_EQ(2u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(scope, notifications_[0].scope);
  EXPECT_EQ(registration_id, notifications_[0].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[1].type);
  EXPECT_EQ(scope, notifications_[1].scope);
  EXPECT_EQ(registration_id, notifications_[1].registration_id);

  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kOk,
                     false /* expect_waiting */, true /* expect_active */));
  base::RunLoop().RunUntilIdle();
}

// Make sure registrations are cleaned up when they are unregistered.
TEST_F(ServiceWorkerContextTest, Unregister) {
  GURL scope("https://www.example.com/");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  bool called = false;
  int64_t registration_id = blink::mojom::kInvalidServiceWorkerRegistrationId;
  context()->RegisterServiceWorker(
      GURL("https://www.example.com/service_worker.js"), options,
      blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId, registration_id);

  called = false;
  context()->UnregisterServiceWorker(scope, MakeUnregisteredCallback(&called));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(called);

  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kErrorNotFound,
                     false /* expect_waiting */, false /* expect_active */));
  base::RunLoop().RunUntilIdle();

  ASSERT_EQ(3u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(scope, notifications_[0].scope);
  EXPECT_EQ(registration_id, notifications_[0].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[1].type);
  EXPECT_EQ(scope, notifications_[1].scope);
  EXPECT_EQ(registration_id, notifications_[1].registration_id);
  EXPECT_EQ(REGISTRATION_DELETED, notifications_[2].type);
  EXPECT_EQ(scope, notifications_[2].scope);
  EXPECT_EQ(registration_id, notifications_[2].registration_id);
}

// Make sure registrations are cleaned up when they are unregistered in bulk.
TEST_F(ServiceWorkerContextTest, UnregisterMultiple) {
  GURL origin1_s1("https://www.example.com/test");
  GURL origin1_s2("https://www.example.com/hello");
  GURL origin2_s1("https://www.example.com:8080/again");
  GURL origin3_s1("https://www.other.com/");
  int64_t registration_id1 = blink::mojom::kInvalidServiceWorkerRegistrationId;
  int64_t registration_id2 = blink::mojom::kInvalidServiceWorkerRegistrationId;
  int64_t registration_id3 = blink::mojom::kInvalidServiceWorkerRegistrationId;
  int64_t registration_id4 = blink::mojom::kInvalidServiceWorkerRegistrationId;

  {
    bool called = false;
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = origin1_s1;
    context()->RegisterServiceWorker(
        GURL("https://www.example.com/service_worker.js"), options,
        blink::mojom::FetchClientSettingsObject::New(),
        MakeRegisteredCallback(&called, &registration_id1));
    ASSERT_FALSE(called);
    base::RunLoop().RunUntilIdle();
    ASSERT_TRUE(called);
  }

  {
    bool called = false;
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = origin1_s2;
    context()->RegisterServiceWorker(
        GURL("https://www.example.com/service_worker2.js"), options,
        blink::mojom::FetchClientSettingsObject::New(),
        MakeRegisteredCallback(&called, &registration_id2));
    ASSERT_FALSE(called);
    base::RunLoop().RunUntilIdle();
    ASSERT_TRUE(called);
  }

  {
    bool called = false;
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = origin2_s1;
    context()->RegisterServiceWorker(
        GURL("https://www.example.com:8080/service_worker3.js"), options,
        blink::mojom::FetchClientSettingsObject::New(),
        MakeRegisteredCallback(&called, &registration_id3));
    ASSERT_FALSE(called);
    base::RunLoop().RunUntilIdle();
    ASSERT_TRUE(called);
  }

  {
    bool called = false;
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = origin3_s1;
    context()->RegisterServiceWorker(
        GURL("https://www.other.com/service_worker4.js"), options,
        blink::mojom::FetchClientSettingsObject::New(),
        MakeRegisteredCallback(&called, &registration_id4));
    ASSERT_FALSE(called);
    base::RunLoop().RunUntilIdle();
    ASSERT_TRUE(called);
  }

  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId,
            registration_id1);
  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId,
            registration_id2);
  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId,
            registration_id3);
  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId,
            registration_id4);

  bool called = false;
  context()->DeleteForOrigin(origin1_s1.GetOrigin(),
                             MakeUnregisteredCallback(&called));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(called);

  context()->registry()->FindRegistrationForId(
      registration_id1, origin1_s1.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kErrorNotFound,
                     false /* expect_waiting */, false /* expect_active */));
  context()->registry()->FindRegistrationForId(
      registration_id2, origin1_s2.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kErrorNotFound,
                     false /* expect_waiting */, false /* expect_active */));
  context()->registry()->FindRegistrationForId(
      registration_id3, origin2_s1.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kOk,
                     false /* expect_waiting */, true /* expect_active */));

  context()->registry()->FindRegistrationForId(
      registration_id4, origin3_s1.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kOk,
                     false /* expect_waiting */, true /* expect_active */));

  base::RunLoop().RunUntilIdle();

  ASSERT_EQ(10u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(registration_id1, notifications_[0].registration_id);
  EXPECT_EQ(origin1_s1, notifications_[0].scope);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[1].type);
  EXPECT_EQ(registration_id1, notifications_[1].registration_id);
  EXPECT_EQ(origin1_s1, notifications_[1].scope);
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[2].type);
  EXPECT_EQ(origin1_s2, notifications_[2].scope);
  EXPECT_EQ(registration_id2, notifications_[2].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[3].type);
  EXPECT_EQ(origin1_s2, notifications_[3].scope);
  EXPECT_EQ(registration_id2, notifications_[3].registration_id);
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[4].type);
  EXPECT_EQ(origin2_s1, notifications_[4].scope);
  EXPECT_EQ(registration_id3, notifications_[4].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[5].type);
  EXPECT_EQ(origin2_s1, notifications_[5].scope);
  EXPECT_EQ(registration_id3, notifications_[5].registration_id);
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[6].type);
  EXPECT_EQ(origin3_s1, notifications_[6].scope);
  EXPECT_EQ(registration_id4, notifications_[6].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[7].type);
  EXPECT_EQ(origin3_s1, notifications_[7].scope);
  EXPECT_EQ(registration_id4, notifications_[7].registration_id);
  EXPECT_EQ(REGISTRATION_DELETED, notifications_[8].type);
  EXPECT_EQ(origin1_s1, notifications_[8].scope);
  EXPECT_EQ(registration_id1, notifications_[8].registration_id);
  EXPECT_EQ(REGISTRATION_DELETED, notifications_[9].type);
  EXPECT_EQ(origin1_s2, notifications_[9].scope);
  EXPECT_EQ(registration_id2, notifications_[9].registration_id);
}

// Make sure registering a new script shares an existing registration.
TEST_F(ServiceWorkerContextTest, RegisterNewScript) {
  GURL scope("https://www.example.com/");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  bool called = false;
  int64_t old_registration_id =
      blink::mojom::kInvalidServiceWorkerRegistrationId;
  context()->RegisterServiceWorker(
      GURL("https://www.example.com/service_worker.js"), options,
      blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &old_registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId,
            old_registration_id);

  called = false;
  int64_t new_registration_id =
      blink::mojom::kInvalidServiceWorkerRegistrationId;
  context()->RegisterServiceWorker(
      GURL("https://www.example.com/service_worker_new.js"), options,
      blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &new_registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(called);

  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId,
            new_registration_id);
  EXPECT_EQ(old_registration_id, new_registration_id);

  ASSERT_EQ(4u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(scope, notifications_[0].scope);
  EXPECT_EQ(old_registration_id, notifications_[0].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[1].type);
  EXPECT_EQ(scope, notifications_[1].scope);
  EXPECT_EQ(old_registration_id, notifications_[1].registration_id);
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[2].type);
  EXPECT_EQ(scope, notifications_[2].scope);
  EXPECT_EQ(new_registration_id, notifications_[2].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[3].type);
  EXPECT_EQ(scope, notifications_[3].scope);
  EXPECT_EQ(new_registration_id, notifications_[3].registration_id);
}

// Make sure that when registering a duplicate scope+script_url
// combination, that the same registration is used.
TEST_F(ServiceWorkerContextTest, RegisterDuplicateScript) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  bool called = false;
  int64_t old_registration_id =
      blink::mojom::kInvalidServiceWorkerRegistrationId;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &old_registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_NE(blink::mojom::kInvalidServiceWorkerRegistrationId,
            old_registration_id);

  called = false;
  int64_t new_registration_id =
      blink::mojom::kInvalidServiceWorkerRegistrationId;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &new_registration_id));

  ASSERT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(old_registration_id, new_registration_id);

  ASSERT_EQ(3u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(scope, notifications_[0].scope);
  EXPECT_EQ(old_registration_id, notifications_[0].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[1].type);
  EXPECT_EQ(scope, notifications_[1].scope);
  EXPECT_EQ(old_registration_id, notifications_[1].registration_id);
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[2].type);
  EXPECT_EQ(scope, notifications_[2].scope);
  EXPECT_EQ(old_registration_id, notifications_[2].registration_id);
}

TEST_F(ServiceWorkerContextTest, ContainerHostIterator) {
  const int kRenderProcessId1 = 1;
  const int kRenderProcessId2 = 2;
  const GURL kOrigin1 = GURL("https://www.example.com/");
  const GURL kOrigin2 = GURL("https://another-origin.example.net/");
  std::vector<ServiceWorkerRemoteProviderEndpoint> remote_endpoints;

  // Host1 : process_id=1, origin1.
  remote_endpoints.emplace_back();
  base::WeakPtr<ServiceWorkerContainerHost> container_host1 =
      CreateContainerHostForWindow(
          kRenderProcessId1, true /* is_parent_frame_secure */,
          context()->AsWeakPtr(), &remote_endpoints.back());
  container_host1->UpdateUrls(kOrigin1, net::SiteForCookies::FromUrl(kOrigin1),
                              url::Origin::Create(kOrigin1));

  // Host2 : process_id=2, origin2.
  remote_endpoints.emplace_back();
  base::WeakPtr<ServiceWorkerContainerHost> container_host2 =
      CreateContainerHostForWindow(
          kRenderProcessId2, true /* is_parent_frame_secure */,
          context()->AsWeakPtr(), &remote_endpoints.back());
  container_host2->UpdateUrls(kOrigin2, net::SiteForCookies::FromUrl(kOrigin2),
                              url::Origin::Create(kOrigin2));

  // Host3 : process_id=2, origin1.
  remote_endpoints.emplace_back();
  base::WeakPtr<ServiceWorkerContainerHost> container_host3 =
      CreateContainerHostForWindow(
          kRenderProcessId2, true /* is_parent_frame_secure */,
          context()->AsWeakPtr(), &remote_endpoints.back());
  container_host3->UpdateUrls(kOrigin1, net::SiteForCookies::FromUrl(kOrigin1),
                              url::Origin::Create(kOrigin1));

  // Host4 : process_id=2, origin2, for ServiceWorker.
  blink::mojom::ServiceWorkerRegistrationOptions registration_opt;
  registration_opt.scope = GURL("https://another-origin.example.net/test/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      base::MakeRefCounted<ServiceWorkerRegistration>(
          registration_opt, 1L /* registration_id */,
          helper_->context()->AsWeakPtr());
  scoped_refptr<ServiceWorkerVersion> version =
      base::MakeRefCounted<ServiceWorkerVersion>(
          registration.get(),
          GURL("https://another-origin.example.net/test/script_url"),
          blink::mojom::ScriptType::kClassic, 1L /* version_id */,
          helper_->context()->AsWeakPtr());
  remote_endpoints.emplace_back();
  // ServiceWorkrProviderHost creates ServiceWorkerContainerHost for a service
  // worker execution context.
  std::unique_ptr<ServiceWorkerProviderHost> provider_host4 =
      CreateProviderHostForServiceWorkerContext(
          kRenderProcessId2, true /* is_parent_frame_secure */, version.get(),
          context()->AsWeakPtr(), &remote_endpoints.back());
  EXPECT_NE(provider_host4->provider_id(),
            blink::kInvalidServiceWorkerProviderId);

  ASSERT_TRUE(container_host1);
  ASSERT_TRUE(container_host2);
  ASSERT_TRUE(container_host3);
  ASSERT_TRUE(provider_host4->container_host());

  // Iterate over the client container hosts that belong to kOrigin1.
  std::set<ServiceWorkerContainerHost*> results;
  for (auto it = context()->GetClientContainerHostIterator(
           kOrigin1, true /* include_reserved_clients */,
           false /* include_back_forward_cached_clients */);
       !it->IsAtEnd(); it->Advance()) {
    results.insert(it->GetContainerHost());
  }
  EXPECT_EQ(2u, results.size());
  EXPECT_TRUE(base::Contains(results, container_host1.get()));
  EXPECT_TRUE(base::Contains(results, container_host3.get()));

  // Iterate over the container hosts that belong to kOrigin2. This should not
  // include provider_host4->container_host() as it's not for controllee.
  results.clear();
  for (auto it = context()->GetClientContainerHostIterator(
           kOrigin2, true /* include_reserved_clients */,
           false /* include_back_forward_cached_clients */);
       !it->IsAtEnd(); it->Advance()) {
    results.insert(it->GetContainerHost());
  }
  EXPECT_EQ(1u, results.size());
  EXPECT_TRUE(base::Contains(results, container_host2.get()));

  context()->UnregisterContainerHostByClientID(container_host1->client_uuid());
  context()->UnregisterContainerHostByClientID(container_host2->client_uuid());
  context()->UnregisterContainerHostByClientID(container_host3->client_uuid());
}

class ServiceWorkerContextRecoveryTest
    : public ServiceWorkerContextTest,
      public testing::WithParamInterface<bool /* is_storage_on_disk */> {
 public:
  ServiceWorkerContextRecoveryTest() {}
  virtual ~ServiceWorkerContextRecoveryTest() {}

 protected:
  bool is_storage_on_disk() const { return GetParam(); }
};

TEST_P(ServiceWorkerContextRecoveryTest, DeleteAndStartOver) {
  GURL scope("https://www.example.com/");
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;

  if (is_storage_on_disk()) {
    // Reinitialize the helper to test on-disk storage.
    base::FilePath user_data_directory;
    ASSERT_NO_FATAL_FAILURE(GetTemporaryDirectory(&user_data_directory));
    helper_.reset(new EmbeddedWorkerTestHelper(user_data_directory));
    helper_->context_wrapper()->AddObserver(this);
  }

  int64_t registration_id = blink::mojom::kInvalidServiceWorkerRegistrationId;
  bool called = false;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &registration_id));

  ASSERT_FALSE(called);
  content::RunAllTasksUntilIdle();
  EXPECT_TRUE(called);

  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kOk,
                     false /* expect_waiting */, true /* expect_active */));
  content::RunAllTasksUntilIdle();

  context()->ScheduleDeleteAndStartOver();

  // The storage is disabled while the recovery process is running, so the
  // operation should be aborted.
  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kErrorAbort,
                     false /* expect_waiting */, true /* expect_active */));
  content::RunAllTasksUntilIdle();

  // The context started over and the storage was re-initialized, so the
  // registration should not be found.
  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kErrorNotFound,
                     false /* expect_waiting */, true /* expect_active */));
  content::RunAllTasksUntilIdle();

  called = false;
  context()->RegisterServiceWorker(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      MakeRegisteredCallback(&called, &registration_id));

  ASSERT_FALSE(called);
  content::RunAllTasksUntilIdle();
  EXPECT_TRUE(called);

  context()->registry()->FindRegistrationForId(
      registration_id, scope.GetOrigin(),
      base::BindOnce(&ExpectRegisteredWorkers,
                     blink::ServiceWorkerStatusCode::kOk,
                     false /* expect_waiting */, true /* expect_active */));
  content::RunAllTasksUntilIdle();

  ASSERT_EQ(5u, notifications_.size());
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[0].type);
  EXPECT_EQ(scope, notifications_[0].scope);
  EXPECT_EQ(registration_id, notifications_[0].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[1].type);
  EXPECT_EQ(scope, notifications_[1].scope);
  EXPECT_EQ(registration_id, notifications_[1].registration_id);
  EXPECT_EQ(STORAGE_RECOVERED, notifications_[2].type);
  EXPECT_EQ(REGISTRATION_COMPLETED, notifications_[3].type);
  EXPECT_EQ(scope, notifications_[3].scope);
  EXPECT_EQ(registration_id, notifications_[3].registration_id);
  EXPECT_EQ(REGISTRATION_STORED, notifications_[4].type);
  EXPECT_EQ(scope, notifications_[4].scope);
  EXPECT_EQ(registration_id, notifications_[4].registration_id);
}

INSTANTIATE_TEST_SUITE_P(ServiceWorkerContextRecoveryTest,
                         ServiceWorkerContextRecoveryTest,
                         testing::Bool() /* is_storage_on_disk */);

}  // namespace content
