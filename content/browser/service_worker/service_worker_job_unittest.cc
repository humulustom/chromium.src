// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <tuple>

#include "base/barrier_closure.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/stl_util.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_simple_task_runner.h"
#include "base/time/time.h"
#include "content/browser/frame_host/frame_tree_node.h"
#include "content/browser/service_worker/embedded_worker_status.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/fake_embedded_worker_instance_client.h"
#include "content/browser/service_worker/service_worker_consts.h"
#include "content/browser/service_worker/service_worker_container_host.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_disk_cache.h"
#include "content/browser/service_worker/service_worker_job_coordinator.h"
#include "content/browser/service_worker/service_worker_object_host.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_registration_object_host.h"
#include "content/browser/service_worker/service_worker_registration_status.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/browser/service_worker/test_service_worker_observer.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/url_loader_interceptor.h"
#include "content/test/fake_network.h"
#include "ipc/ipc_test_sink.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/service_worker/service_worker.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_event_status.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_object.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"

using net::IOBuffer;
using net::TestCompletionCallback;
using net::WrappedIOBuffer;

// Unit tests for testing all job registration tasks.
namespace content {

namespace {

void SaveRegistrationCallback(
    blink::ServiceWorkerStatusCode expected_status,
    scoped_refptr<ServiceWorkerRegistration>* registration_out,
    base::OnceClosure quit_closure,
    blink::ServiceWorkerStatusCode status,
    const std::string& status_message,
    ServiceWorkerRegistration* registration) {
  EXPECT_EQ(expected_status, status);
  *registration_out = registration;
  std::move(quit_closure).Run();
}

void SaveFoundRegistrationCallback(
    blink::ServiceWorkerStatusCode expected_status,
    scoped_refptr<ServiceWorkerRegistration>* registration,
    base::OnceClosure quit_closure,
    blink::ServiceWorkerStatusCode status,
    scoped_refptr<ServiceWorkerRegistration> result) {
  EXPECT_EQ(expected_status, status);
  *registration = std::move(result);
  std::move(quit_closure).Run();
}

// Creates a callback which keeps track of the resulting registration.
// When the callback is fired, it ensures that the resulting status
// matches the expectation.
ServiceWorkerRegisterJob::RegistrationCallback SaveRegistration(
    blink::ServiceWorkerStatusCode expected_status,
    scoped_refptr<ServiceWorkerRegistration>* registration,
    base::OnceClosure quit_closure) {
  return base::BindOnce(&SaveRegistrationCallback, expected_status,
                        registration, std::move(quit_closure));
}

ServiceWorkerRegistry::FindRegistrationCallback SaveFoundRegistration(
    blink::ServiceWorkerStatusCode expected_status,
    scoped_refptr<ServiceWorkerRegistration>* registration,
    base::OnceClosure quit_closure) {
  return base::BindOnce(&SaveFoundRegistrationCallback, expected_status,
                        registration, std::move(quit_closure));
}

void SaveUnregistrationCallback(blink::ServiceWorkerStatusCode expected_status,
                                base::OnceClosure quit_closure,
                                int64_t registration_id,
                                blink::ServiceWorkerStatusCode status) {
  EXPECT_EQ(expected_status, status);
  std::move(quit_closure).Run();
}

ServiceWorkerUnregisterJob::UnregistrationCallback SaveUnregistration(
    blink::ServiceWorkerStatusCode expected_status,
    base::OnceClosure quit_closure) {
  return base::BindOnce(&SaveUnregistrationCallback, expected_status,
                        std::move(quit_closure));
}

void RequestTermination(
    mojo::AssociatedRemote<blink::mojom::EmbeddedWorkerInstanceHost>* host) {
  // We can't wait for the callback since StopWorker() arrives before it which
  // severs the Mojo connection.
  (*host)->RequestTermination(base::DoNothing());
}

class EmbeddedWorkerStatusObserver : public ServiceWorkerVersion::Observer {
 public:
  EmbeddedWorkerStatusObserver(base::OnceClosure quit_closure,
                               EmbeddedWorkerStatus running_status)
      : quit_closure_(std::move(quit_closure)),
        expected_running_status_(running_status) {}

  void OnRunningStateChanged(ServiceWorkerVersion* version) override {
    if (!quit_closure_)
      return;

    if (version->running_status() == expected_running_status_)
      std::move(quit_closure_).Run();
  }

 private:
  EmbeddedWorkerStatusObserver(const EmbeddedWorkerStatusObserver&) = delete;
  EmbeddedWorkerStatusObserver& operator=(const EmbeddedWorkerStatusObserver&) =
      delete;

  base::OnceClosure quit_closure_;

  EmbeddedWorkerStatus expected_running_status_;
};

}  // namespace

class ServiceWorkerJobTest : public testing::Test {
 public:
  ServiceWorkerJobTest()
      : task_environment_(base::test::TaskEnvironment::TimeSource::MOCK_TIME,
                          BrowserTaskEnvironment::IO_MAINLOOP) {}

  void SetUp() override {
    helper_.reset(new EmbeddedWorkerTestHelper(base::FilePath()));
  }

  void TearDown() override { helper_.reset(); }

  ServiceWorkerContextCore* context() const { return helper_->context(); }

  ServiceWorkerJobCoordinator* job_coordinator() const {
    return context()->job_coordinator();
  }
  ServiceWorkerRegistry* registry() const { return context()->registry(); }
  ServiceWorkerStorage* storage() const { return context()->storage(); }

 protected:
  scoped_refptr<ServiceWorkerRegistration> RunRegisterJob(
      const GURL& script_url,
      const blink::mojom::ServiceWorkerRegistrationOptions& options,
      blink::ServiceWorkerStatusCode expected_status =
          blink::ServiceWorkerStatusCode::kOk);
  void RunUnregisterJob(const GURL& scope,
                        blink::ServiceWorkerStatusCode expected_status =
                            blink::ServiceWorkerStatusCode::kOk);
  void WaitForVersionRunningStatus(scoped_refptr<ServiceWorkerVersion> version,
                                   EmbeddedWorkerStatus running_status);
  scoped_refptr<ServiceWorkerRegistration> FindRegistrationForScope(
      const GURL& scope,
      blink::ServiceWorkerStatusCode expected_status =
          blink::ServiceWorkerStatusCode::kOk);
  ServiceWorkerContainerHost* CreateControllee();

  BrowserTaskEnvironment task_environment_;
  std::unique_ptr<EmbeddedWorkerTestHelper> helper_;
  std::vector<ServiceWorkerRemoteProviderEndpoint> remote_endpoints_;
};

scoped_refptr<ServiceWorkerRegistration> ServiceWorkerJobTest::RunRegisterJob(
    const GURL& script_url,
    const blink::mojom::ServiceWorkerRegistrationOptions& options,
    blink::ServiceWorkerStatusCode expected_status) {
  scoped_refptr<ServiceWorkerRegistration> registration;
  base::RunLoop run_loop;
  auto outside_fetch_client_settings_object =
      blink::mojom::FetchClientSettingsObject::New();
  outside_fetch_client_settings_object->outgoing_referrer = script_url;
  job_coordinator()->Register(
      script_url, options, std::move(outside_fetch_client_settings_object),
      SaveRegistration(expected_status, &registration, run_loop.QuitClosure()));
  run_loop.Run();
  return registration;
}

void ServiceWorkerJobTest::RunUnregisterJob(
    const GURL& scope,
    blink::ServiceWorkerStatusCode expected_status) {
  base::RunLoop run_loop;
  job_coordinator()->Unregister(
      scope, SaveUnregistration(expected_status, run_loop.QuitClosure()));
  run_loop.Run();
}

void ServiceWorkerJobTest::WaitForVersionRunningStatus(
    scoped_refptr<ServiceWorkerVersion> version,
    EmbeddedWorkerStatus running_status) {
  if (version->running_status() == running_status)
    return;

  base::RunLoop run_loop;
  EmbeddedWorkerStatusObserver observer(run_loop.QuitClosure(), running_status);
  version->AddObserver(&observer);
  run_loop.Run();
  version->RemoveObserver(&observer);
}

scoped_refptr<ServiceWorkerRegistration>
ServiceWorkerJobTest::FindRegistrationForScope(
    const GURL& scope,
    blink::ServiceWorkerStatusCode expected_status) {
  scoped_refptr<ServiceWorkerRegistration> registration;
  base::RunLoop run_loop;
  registry()->FindRegistrationForScope(
      scope, SaveFoundRegistration(expected_status, &registration,
                                   run_loop.QuitClosure()));
  run_loop.Run();
  return registration;
}

ServiceWorkerContainerHost* ServiceWorkerJobTest::CreateControllee() {
  remote_endpoints_.emplace_back();
  base::WeakPtr<ServiceWorkerContainerHost> container_host =
      CreateContainerHostForWindow(
          33 /* dummy render process id */, true /* is_parent_frame_secure */,
          helper_->context()->AsWeakPtr(), &remote_endpoints_.back());
  return container_host.get();
}

TEST_F(ServiceWorkerJobTest, SameDocumentSameRegistration) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> original_registration =
      RunRegisterJob(GURL("https://www.example.com/service_worker.js"),
                     options);
  scoped_refptr<ServiceWorkerRegistration> registration1;
  base::RunLoop run_loop;
  base::RepeatingClosure barrier_closure =
      base::BarrierClosure(2, run_loop.QuitClosure());
  registry()->FindRegistrationForClientUrl(
      GURL("https://www.example.com/"),
      SaveFoundRegistration(blink::ServiceWorkerStatusCode::kOk, &registration1,
                            barrier_closure));
  scoped_refptr<ServiceWorkerRegistration> registration2;
  registry()->FindRegistrationForClientUrl(
      GURL("https://www.example.com/"),
      SaveFoundRegistration(blink::ServiceWorkerStatusCode::kOk, &registration2,
                            barrier_closure));
  run_loop.Run();
  ASSERT_TRUE(registration1.get());
  ASSERT_EQ(registration1, original_registration);
  ASSERT_EQ(registration1, registration2);
}

TEST_F(ServiceWorkerJobTest, SameMatchSameRegistration) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> original_registration =
      RunRegisterJob(GURL("https://www.example.com/service_worker.js"),
                     options);
  ASSERT_NE(static_cast<ServiceWorkerRegistration*>(nullptr),
            original_registration.get());

  scoped_refptr<ServiceWorkerRegistration> registration1;
  base::RunLoop run_loop;
  base::RepeatingClosure barrier_closure =
      base::BarrierClosure(2, run_loop.QuitClosure());
  registry()->FindRegistrationForClientUrl(
      GURL("https://www.example.com/one"),
      SaveFoundRegistration(blink::ServiceWorkerStatusCode::kOk, &registration1,
                            barrier_closure));

  scoped_refptr<ServiceWorkerRegistration> registration2;
  registry()->FindRegistrationForClientUrl(
      GURL("https://www.example.com/two"),
      SaveFoundRegistration(blink::ServiceWorkerStatusCode::kOk, &registration2,
                            barrier_closure));
  run_loop.Run();
  ASSERT_EQ(registration1, original_registration);
  ASSERT_EQ(registration1, registration2);
}

TEST_F(ServiceWorkerJobTest, DifferentMatchDifferentRegistration) {
  const GURL scope1("https://www.example.com/one");
  const GURL scope2("https://www.example.com/two");
  const GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options1;
  options1.scope = scope1;
  blink::mojom::ServiceWorkerRegistrationOptions options2;
  options2.scope = scope2;

  RunRegisterJob(script_url, options1);
  RunRegisterJob(script_url, options2);

  scoped_refptr<ServiceWorkerRegistration> registration1;
  base::RunLoop run_loop;
  base::RepeatingClosure barrier_closure =
      base::BarrierClosure(2, run_loop.QuitClosure());
  registry()->FindRegistrationForClientUrl(
      scope1, SaveFoundRegistration(blink::ServiceWorkerStatusCode::kOk,
                                    &registration1, barrier_closure));
  scoped_refptr<ServiceWorkerRegistration> registration2;
  registry()->FindRegistrationForClientUrl(
      scope2, SaveFoundRegistration(blink::ServiceWorkerStatusCode::kOk,
                                    &registration2, barrier_closure));

  run_loop.Run();
  ASSERT_NE(registration1, registration2);
}

class RecordInstallActivateWorker : public FakeServiceWorker {
 public:
  RecordInstallActivateWorker(EmbeddedWorkerTestHelper* helper)
      : FakeServiceWorker(helper) {}
  ~RecordInstallActivateWorker() override = default;

  const std::vector<ServiceWorkerMetrics::EventType>& events() const {
    return events_;
  }

  void DispatchInstallEvent(
      blink::mojom::ServiceWorker::DispatchInstallEventCallback callback)
      override {
    events_.emplace_back(ServiceWorkerMetrics::EventType::INSTALL);
    FakeServiceWorker::DispatchInstallEvent(std::move(callback));
  }

  void DispatchActivateEvent(
      blink::mojom::ServiceWorker::DispatchActivateEventCallback callback)
      override {
    events_.emplace_back(ServiceWorkerMetrics::EventType::ACTIVATE);
    FakeServiceWorker::DispatchActivateEvent(std::move(callback));
  }

 private:
  std::vector<ServiceWorkerMetrics::EventType> events_;
};

// Make sure basic registration is working.
TEST_F(ServiceWorkerJobTest, Register) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  auto* worker =
      helper_->AddNewPendingServiceWorker<RecordInstallActivateWorker>(
          helper_.get());
  scoped_refptr<ServiceWorkerRegistration> registration = RunRegisterJob(
      GURL("https://www.example.com/service_worker.js"), options);
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(registration->installing_version(), runner);
  scoped_refptr<ServiceWorkerVersion> version = registration->active_version();

  EXPECT_TRUE(registration);
  ASSERT_EQ(2u, worker->events().size());
  EXPECT_EQ(ServiceWorkerMetrics::EventType::INSTALL, worker->events()[0]);
  EXPECT_EQ(ServiceWorkerMetrics::EventType::ACTIVATE, worker->events()[1]);
}

// Make sure registrations are cleaned up when they are unregistered.
TEST_F(ServiceWorkerJobTest, Unregister) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration = RunRegisterJob(
      GURL("https://www.example.com/service_worker.js"), options);
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(registration->installing_version(), runner);
  scoped_refptr<ServiceWorkerVersion> version = registration->active_version();

  ServiceWorkerProviderHost* provider_host =
      registration->active_version()->provider_host();
  ASSERT_NE(nullptr, provider_host);
  ServiceWorkerContainerHost* container_host = provider_host->container_host();
  // One ServiceWorkerRegistrationObjectHost should have been created for the
  // new registration.
  EXPECT_EQ(1UL, container_host->registration_object_hosts_.size());
  // One ServiceWorkerObjectHost should have been created for the new service
  // worker.
  EXPECT_EQ(1UL, container_host->service_worker_object_hosts_.size());

  RunUnregisterJob(options.scope);

  WaitForVersionRunningStatus(version, EmbeddedWorkerStatus::STOPPED);

  // The service worker registration object host and service worker object host
  // have been destroyed together with |provider_host| by the above
  // unregistration. Then |registration| and |version| should be the last one
  // reference to the corresponding instance.
  EXPECT_TRUE(registration->HasOneRef());
  EXPECT_TRUE(version->HasOneRef());
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());

  registration = FindRegistrationForScope(
      options.scope, blink::ServiceWorkerStatusCode::kErrorNotFound);

  EXPECT_FALSE(registration);
}

TEST_F(ServiceWorkerJobTest, Unregister_NothingRegistered) {
  GURL scope("https://www.example.com/");

  RunUnregisterJob(scope, blink::ServiceWorkerStatusCode::kErrorNotFound);
}

// Make sure registering a new script creates a new version and shares an
// existing registration.
TEST_F(ServiceWorkerJobTest, RegisterNewScript) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> old_registration = RunRegisterJob(
      GURL("https://www.example.com/service_worker.js"), options);
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  old_registration->SetTaskRunnerForTest(runner);
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(old_registration->installing_version(), runner);

  scoped_refptr<ServiceWorkerRegistration> old_registration_by_scope =
      FindRegistrationForScope(options.scope);

  ASSERT_EQ(old_registration, old_registration_by_scope);
  old_registration_by_scope = nullptr;

  scoped_refptr<ServiceWorkerRegistration> new_registration = RunRegisterJob(
      GURL("https://www.example.com/service_worker_new.js"), options);

  ASSERT_EQ(old_registration, new_registration);

  scoped_refptr<ServiceWorkerRegistration> new_registration_by_scope =
      FindRegistrationForScope(options.scope);

  ASSERT_EQ(new_registration, new_registration_by_scope);
}

// Make sure that when registering a duplicate scope+script_url
// combination, that the same registration is used.
TEST_F(ServiceWorkerJobTest, RegisterDuplicateScript) {
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");

  scoped_refptr<ServiceWorkerRegistration> old_registration =
      RunRegisterJob(script_url, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();

  // During the above registration, a service worker registration object host
  // for ServiceWorkerGlobalScope#registration has been created/added into
  // |provider_host|.
  ServiceWorkerProviderHost* provider_host =
      old_registration->active_version()->provider_host();
  ASSERT_NE(nullptr, provider_host);
  ServiceWorkerContainerHost* container_host = provider_host->container_host();

  // Clear all service worker object hosts.
  container_host->service_worker_object_hosts_.clear();
  // Ensure that the registration's object host doesn't have the reference.
  EXPECT_EQ(1UL, container_host->registration_object_hosts_.size());
  container_host->registration_object_hosts_.clear();
  EXPECT_EQ(0UL, container_host->registration_object_hosts_.size());
  ASSERT_TRUE(old_registration->HasOneRef());

  scoped_refptr<ServiceWorkerRegistration> old_registration_by_scope =
      FindRegistrationForScope(options.scope);

  ASSERT_TRUE(old_registration_by_scope.get());

  scoped_refptr<ServiceWorkerRegistration> new_registration =
      RunRegisterJob(script_url, options);

  ASSERT_EQ(old_registration, new_registration);

  ASSERT_FALSE(old_registration->HasOneRef());

  scoped_refptr<ServiceWorkerRegistration> new_registration_by_scope =
      FindRegistrationForScope(options.scope);

  EXPECT_EQ(new_registration_by_scope, old_registration);
}

// An instance client that breaks the Mojo connection upon receiving the
// Start() message.
class FailStartInstanceClient : public FakeEmbeddedWorkerInstanceClient {
 public:
  FailStartInstanceClient(EmbeddedWorkerTestHelper* helper)
      : FakeEmbeddedWorkerInstanceClient(helper) {}

  void StartWorker(blink::mojom::EmbeddedWorkerStartParamsPtr params) override {
    // Don't save the Mojo ptrs. The connection breaks.
  }
};

TEST_F(ServiceWorkerJobTest, Register_FailToStartWorker) {
  helper_->AddPendingInstanceClient(
      std::make_unique<FailStartInstanceClient>(helper_.get()));

  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("https://www.example.com/service_worker.js"), options,
                     blink::ServiceWorkerStatusCode::kErrorStartWorkerFailed);

  ASSERT_EQ(scoped_refptr<ServiceWorkerRegistration>(nullptr), registration);
}

// Register and then unregister the scope, in parallel. Job coordinator should
// process jobs until the last job.
TEST_F(ServiceWorkerJobTest, ParallelRegUnreg) {
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script_url, options);

  RunUnregisterJob(options.scope);

  registration = FindRegistrationForScope(
      options.scope, blink::ServiceWorkerStatusCode::kErrorNotFound);

  ASSERT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration);
}

// Register conflicting scripts for the same scope. The most recent
// registration should win, and the old registration should have been
// shutdown.
TEST_F(ServiceWorkerJobTest, ParallelRegNewScript) {
  GURL scope("https://www.example.com/");

  GURL script_url1("https://www.example.com/service_worker1.js");
  scoped_refptr<ServiceWorkerRegistration> registration1 = RunRegisterJob(
      script_url1, blink::mojom::ServiceWorkerRegistrationOptions(
                       scope, blink::mojom::ScriptType::kClassic,
                       blink::mojom::ServiceWorkerUpdateViaCache::kNone));

  GURL script_url2("https://www.example.com/service_worker2.js");
  scoped_refptr<ServiceWorkerRegistration> registration2 = RunRegisterJob(
      script_url2, blink::mojom::ServiceWorkerRegistrationOptions(
                       scope, blink::mojom::ScriptType::kClassic,
                       blink::mojom::ServiceWorkerUpdateViaCache::kAll));

  scoped_refptr<ServiceWorkerRegistration> registration =
      FindRegistrationForScope(scope);

  ASSERT_EQ(registration2, registration);
}

// Register the exact same scope + script. Requests should be
// coalesced such that both callers get the exact same registration
// object.
TEST_F(ServiceWorkerJobTest, ParallelRegSameScript) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");

  GURL script_url("https://www.example.com/service_worker1.js");
  scoped_refptr<ServiceWorkerRegistration> registration1 =
      RunRegisterJob(script_url, options);

  scoped_refptr<ServiceWorkerRegistration> registration2 =
      RunRegisterJob(script_url, options);

  ASSERT_EQ(registration1, registration2);

  scoped_refptr<ServiceWorkerRegistration> registration =
      FindRegistrationForScope(options.scope);

  ASSERT_EQ(registration, registration1);
}

// Call simulataneous unregister calls.
TEST_F(ServiceWorkerJobTest, ParallelUnreg) {
  GURL scope("https://www.example.com/");

  GURL script_url("https://www.example.com/service_worker.js");
  RunUnregisterJob(scope, blink::ServiceWorkerStatusCode::kErrorNotFound);

  RunUnregisterJob(scope, blink::ServiceWorkerStatusCode::kErrorNotFound);

  // There isn't really a way to test that they are being coalesced,
  // but we can make sure they can exist simultaneously without
  // crashing.
  scoped_refptr<ServiceWorkerRegistration> registration =
      FindRegistrationForScope(scope,
                               blink::ServiceWorkerStatusCode::kErrorNotFound);

  ASSERT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration);
}

TEST_F(ServiceWorkerJobTest, AbortAll_Register) {
  GURL script_url1("https://www1.example.com/service_worker.js");
  GURL script_url2("https://www2.example.com/service_worker.js");

  blink::mojom::ServiceWorkerRegistrationOptions options1;
  options1.scope = GURL("https://www1.example.com/");
  blink::mojom::ServiceWorkerRegistrationOptions options2;
  options2.scope = GURL("https://www2.example.com/");

  scoped_refptr<ServiceWorkerRegistration> registration1;
  base::RunLoop run_loop;
  base::RepeatingClosure barrier_closure =
      base::BarrierClosure(2, run_loop.QuitClosure());
  job_coordinator()->Register(
      script_url1, options1, blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kErrorAbort,
                       &registration1, barrier_closure));

  scoped_refptr<ServiceWorkerRegistration> registration2;
  job_coordinator()->Register(
      script_url2, options2, blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kErrorAbort,
                       &registration2, barrier_closure));

  job_coordinator()->AbortAll();

  run_loop.Run();

  registration1 = FindRegistrationForScope(
      options1.scope, blink::ServiceWorkerStatusCode::kErrorNotFound);

  registration2 = FindRegistrationForScope(
      options2.scope, blink::ServiceWorkerStatusCode::kErrorNotFound);

  EXPECT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration1);
  EXPECT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration2);
}

TEST_F(ServiceWorkerJobTest, AbortAll_Unregister) {
  GURL scope1("https://www1.example.com/");
  GURL scope2("https://www2.example.com/");

  scoped_refptr<ServiceWorkerRegistration> registration1;
  base::RunLoop run_loop;
  base::RepeatingClosure barrier_closure =
      base::BarrierClosure(2, run_loop.QuitClosure());
  job_coordinator()->Unregister(
      scope1, SaveUnregistration(blink::ServiceWorkerStatusCode::kErrorAbort,
                                 barrier_closure));

  job_coordinator()->Unregister(
      scope2, SaveUnregistration(blink::ServiceWorkerStatusCode::kErrorAbort,
                                 barrier_closure));

  job_coordinator()->AbortAll();

  run_loop.Run();
}

TEST_F(ServiceWorkerJobTest, AbortAll_RegUnreg) {
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");

  scoped_refptr<ServiceWorkerRegistration> registration;
  base::RunLoop run_loop;
  base::RepeatingClosure barrier_closure =
      base::BarrierClosure(2, run_loop.QuitClosure());
  job_coordinator()->Register(
      script_url, options, blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kErrorAbort,
                       &registration, barrier_closure));

  job_coordinator()->Unregister(
      options.scope,
      SaveUnregistration(blink::ServiceWorkerStatusCode::kErrorAbort,
                         barrier_closure));

  job_coordinator()->AbortAll();

  run_loop.Run();

  registration = FindRegistrationForScope(
      options.scope, blink::ServiceWorkerStatusCode::kErrorNotFound);

  EXPECT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration);
}

TEST_F(ServiceWorkerJobTest, AbortScope) {
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options1;
  options1.scope = GURL("https://www.example.com/1");
  blink::mojom::ServiceWorkerRegistrationOptions options2;
  options2.scope = GURL("https://www.example.com/2");

  scoped_refptr<ServiceWorkerRegistration> registration1;
  base::RunLoop run_loop;
  base::RepeatingClosure barrier_closure =
      base::BarrierClosure(2, run_loop.QuitClosure());
  job_coordinator()->Register(
      script_url, options1, blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kErrorAbort,
                       &registration1, barrier_closure));

  scoped_refptr<ServiceWorkerRegistration> registration2;
  job_coordinator()->Register(
      script_url, options2, blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kOk, &registration2,
                       barrier_closure));

  job_coordinator()->Abort(options1.scope);

  run_loop.Run();

  registration1 = FindRegistrationForScope(
      options1.scope, blink::ServiceWorkerStatusCode::kErrorNotFound);
  EXPECT_EQ(nullptr, registration1);

  registration2 = FindRegistrationForScope(options2.scope,
                                           blink::ServiceWorkerStatusCode::kOk);
  EXPECT_NE(nullptr, registration2);
}

// Tests that the waiting worker enters the 'redundant' state upon
// unregistration.
TEST_F(ServiceWorkerJobTest, UnregisterWaitingSetsRedundant) {
  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script_url, options);
  ASSERT_TRUE(registration.get());

  // Manually create the waiting worker since there is no way to become a
  // waiting worker until Update is implemented.
  scoped_refptr<ServiceWorkerVersion> version = new ServiceWorkerVersion(
      registration.get(), script_url, blink::mojom::ScriptType::kClassic, 1L,
      helper_->context()->AsWeakPtr());
  ASSERT_EQ(blink::ServiceWorkerStatusCode::kOk,
            StartServiceWorker(version.get()));

  version->set_fetch_handler_existence(
      ServiceWorkerVersion::FetchHandlerExistence::EXISTS);
  version->SetStatus(ServiceWorkerVersion::INSTALLED);
  registration->SetWaitingVersion(version);
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, version->status());

  RunUnregisterJob(GURL("https://www.example.com/"));
  WaitForVersionRunningStatus(version, EmbeddedWorkerStatus::STOPPED);

  // The version should be stopped since there is no controllee after
  // unregistration.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());
}

// Tests that the active worker enters the 'redundant' state upon
// unregistration.
TEST_F(ServiceWorkerJobTest, UnregisterActiveSetsRedundant) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration = RunRegisterJob(
      GURL("https://www.example.com/service_worker.js"), options);
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(registration->installing_version(), runner);
  ASSERT_TRUE(registration.get());

  scoped_refptr<ServiceWorkerVersion> version = registration->active_version();
  observer.RunUntilStatusChange(version.get(), ServiceWorkerVersion::ACTIVATED);
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, version->status());

  RunUnregisterJob(GURL("https://www.example.com/"));

  WaitForVersionRunningStatus(version, EmbeddedWorkerStatus::STOPPED);

  // The version should be stopped since there is no controllee after
  // unregistration.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());
}

// Tests that the active worker enters the 'redundant' state upon
// unregistration.
TEST_F(ServiceWorkerJobTest,
       UnregisterActiveSetsRedundant_WaitForNoControllee) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration = RunRegisterJob(
      GURL("https://www.example.com/service_worker.js"), options);
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(registration->installing_version(), runner);
  ASSERT_TRUE(registration.get());

  ServiceWorkerContainerHost* container_host = CreateControllee();
  registration->active_version()->AddControllee(container_host);

  scoped_refptr<ServiceWorkerVersion> version = registration->active_version();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, version->status());

  RunUnregisterJob(GURL("https://www.example.com/"));

  // The version should be running since there is still a controllee.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, version->status());

  registration->active_version()->RemoveControllee(
      container_host->client_uuid());
  WaitForVersionRunningStatus(version, EmbeddedWorkerStatus::STOPPED);

  // The version should be stopped since there is no controllee.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());
}

TEST_F(ServiceWorkerJobTest, RegisterWhileUninstalling) {
  GURL script1("https://www.example.com/service_worker.js");
  GURL script2("https://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/one/");

  auto* initial_client =
      helper_->AddNewPendingInstanceClient<FakeEmbeddedWorkerInstanceClient>(
          helper_.get());
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee and queue an unregister to force the uninstalling state.
  ServiceWorkerContainerHost* container_host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(container_host);
  RunUnregisterJob(options.scope);

  // Register another script.
  EXPECT_EQ(registration, RunRegisterJob(script2, options));
  // Wait until the worker becomes installed.
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_EQ(old_version, registration->active_version());
  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();

  // Verify the new version is installed but not activated yet.
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_TRUE(new_version);
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, new_version->status());

  // Make the old version eligible for eviction.
  old_version->RemoveControllee(container_host->client_uuid());
  RequestTermination(&initial_client->host());

  // Wait for activated.
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(new_version.get(), runner);

  // Verify state after the new version is activated.
  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(new_version, registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, new_version->status());
}

TEST_F(ServiceWorkerJobTest, RegisterAndUnregisterWhileUninstalling) {
  GURL script1("https://www.example.com/service_worker.js");
  GURL script2("https://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();

  // Add a controllee and queue an unregister to force the uninstalling state.
  ServiceWorkerContainerHost* container_host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(container_host);
  RunUnregisterJob(options.scope);

  EXPECT_EQ(registration, RunRegisterJob(script2, options));
  // Wait until the worker becomes installed.
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(registration, FindRegistrationForScope(options.scope));
  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();
  ASSERT_TRUE(new_version);

  // Unregister the registration (but it's still live).
  RunUnregisterJob(options.scope);
  // Now it's not found in the storage.
  RunUnregisterJob(options.scope,
                   blink::ServiceWorkerStatusCode::kErrorNotFound);

  FindRegistrationForScope(options.scope,
                           blink::ServiceWorkerStatusCode::kErrorNotFound);
  EXPECT_TRUE(registration->is_uninstalling());
  EXPECT_EQ(old_version, registration->active_version());

  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, old_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, old_version->status());
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, new_version->status());

  old_version->RemoveControllee(container_host->client_uuid());

  WaitForVersionRunningStatus(old_version, EmbeddedWorkerStatus::STOPPED);
  WaitForVersionRunningStatus(new_version, EmbeddedWorkerStatus::STOPPED);

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_TRUE(registration->is_uninstalled());

  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, old_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, old_version->status());
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, new_version->status());
}

TEST_F(ServiceWorkerJobTest, RegisterSameScriptMultipleTimesWhileUninstalling) {
  GURL script1("https://www.example.com/service_worker.js");
  GURL script2("https://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/one/");

  auto* initial_client =
      helper_->AddNewPendingInstanceClient<FakeEmbeddedWorkerInstanceClient>(
          helper_.get());
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee and queue an unregister to force the uninstalling state.
  ServiceWorkerContainerHost* container_host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(container_host);
  RunUnregisterJob(options.scope);

  EXPECT_EQ(registration, RunRegisterJob(script2, options));
  // Wait until the worker becomes installed.
  base::RunLoop().RunUntilIdle();

  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();
  ASSERT_TRUE(new_version);

  RunUnregisterJob(options.scope);

  EXPECT_TRUE(registration->is_uninstalling());

  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_EQ(new_version, registration->waiting_version());

  old_version->RemoveControllee(container_host->client_uuid());
  RequestTermination(&initial_client->host());

  // Wait for activated.
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(new_version.get(), runner);

  // Verify state after the new version is activated.
  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(new_version, registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, new_version->status());
}

// A fake service worker for toggling whether a fetch event handler exists.
class FetchHandlerWorker : public FakeServiceWorker {
 public:
  FetchHandlerWorker(EmbeddedWorkerTestHelper* helper)
      : FakeServiceWorker(helper) {}
  ~FetchHandlerWorker() override = default;

  void set_has_fetch_handler(bool has_fetch_handler) {
    has_fetch_handler_ = has_fetch_handler;
  }

  void DispatchInstallEvent(
      blink::mojom::ServiceWorker::DispatchInstallEventCallback callback)
      override {
    std::move(callback).Run(blink::mojom::ServiceWorkerEventStatus::COMPLETED,
                            has_fetch_handler_);
  }

 private:
  bool has_fetch_handler_ = false;
};

TEST_F(ServiceWorkerJobTest, HasFetchHandler) {
  GURL script("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration;

  auto* fetch_handler_worker =
      helper_->AddNewPendingServiceWorker<FetchHandlerWorker>(helper_.get());
  fetch_handler_worker->set_has_fetch_handler(true);
  RunRegisterJob(script, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();
  registration = FindRegistrationForScope(options.scope);
  EXPECT_EQ(ServiceWorkerVersion::FetchHandlerExistence::EXISTS,
            registration->active_version()->fetch_handler_existence());
  RunUnregisterJob(options.scope);

  auto* no_fetch_handler_worker =
      helper_->AddNewPendingServiceWorker<FetchHandlerWorker>(helper_.get());
  no_fetch_handler_worker->set_has_fetch_handler(false);
  RunRegisterJob(script, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();
  registration = FindRegistrationForScope(options.scope);
  EXPECT_EQ(ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST,
            registration->active_version()->fetch_handler_existence());
  RunUnregisterJob(options.scope);
}

// Test that clients are alerted of new registrations if they are
// in-scope, so that Clients.claim() or ServiceWorkerContainer.ready work
// correctly.
TEST_F(ServiceWorkerJobTest, AddRegistrationToMatchingProviderHosts) {
  GURL scope("https://www.example.com/scope/");
  GURL in_scope("https://www.example.com/scope/page");
  GURL out_scope("https://www.example.com/page");

  // Make an in-scope client.
  ServiceWorkerContainerHost* client = CreateControllee();
  client->UpdateUrls(in_scope, net::SiteForCookies::FromUrl(in_scope),
                     url::Origin::Create(in_scope));

  // Make an in-scope reserved client.
  std::unique_ptr<ServiceWorkerContainerHostAndInfo> host_and_info =
      CreateContainerHostAndInfoForWindow(helper_->context()->AsWeakPtr(),
                                          /*are_ancestors_secure=*/true);
  base::WeakPtr<ServiceWorkerContainerHost> reserved_client =
      host_and_info->host;
  reserved_client->UpdateUrls(in_scope, net::SiteForCookies::FromUrl(in_scope),
                              url::Origin::Create(in_scope));

  // Make an out-scope client.
  ServiceWorkerContainerHost* out_scope_client = CreateControllee();
  out_scope_client->UpdateUrls(out_scope,
                               net::SiteForCookies::FromUrl(out_scope),
                               url::Origin::Create(out_scope));

  // Make a new registration.
  GURL script("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script, options);

  EXPECT_EQ(registration.get(), client->MatchRegistration());
  EXPECT_EQ(registration.get(), reserved_client->MatchRegistration());
  EXPECT_NE(registration.get(), out_scope_client->MatchRegistration());
}

namespace {  // Helpers for the update job tests.

const char kScope[] = "scope/";
const char kScript[] = "script.js";

const char kHeaders[] =
    "HTTP/1.1 200 OK\n"
    "Content-Type: application/javascript\n\n";
const char kBody[] = "/* old body */";
const char kNewBody[] = "/* new body */";

// TODO(https://crbug.com/1042727): Fix test GURL scoping and remove this getter
// function.
GURL NoChangeOrigin() {
  return GURL("https://nochange/");
}
GURL NewVersionOrigin() {
  return GURL("https://newversion/");
}

void RunNestedUntilIdle() {
  base::RunLoop(base::RunLoop::Type::kNestableTasksAllowed).RunUntilIdle();
}

void OnIOComplete(int* rv_out, int rv) {
  *rv_out = rv;
}

void WriteResponse(ServiceWorkerStorage* storage,
                   int64_t id,
                   const std::string& headers,
                   IOBuffer* body,
                   int length) {
  std::unique_ptr<ServiceWorkerResponseWriter> writer =
      storage->CreateResponseWriter(id);

  std::unique_ptr<net::HttpResponseInfo> info =
      std::make_unique<net::HttpResponseInfo>();
  info->request_time = base::Time::Now();
  info->response_time = base::Time::Now();
  info->was_cached = false;
  info->headers = new net::HttpResponseHeaders(headers);
  scoped_refptr<HttpResponseInfoIOBuffer> info_buffer =
      base::MakeRefCounted<HttpResponseInfoIOBuffer>(std::move(info));

  int rv = -1234;
  writer->WriteInfo(info_buffer.get(), base::BindOnce(&OnIOComplete, &rv));
  RunNestedUntilIdle();
  EXPECT_LT(0, rv);

  rv = -1234;
  writer->WriteData(body, length, base::BindOnce(&OnIOComplete, &rv));
  RunNestedUntilIdle();
  EXPECT_EQ(length, rv);
}

void WriteStringResponse(ServiceWorkerStorage* storage,
                         int64_t id,
                         const std::string& body) {
  scoped_refptr<IOBuffer> body_buffer =
      base::MakeRefCounted<WrappedIOBuffer>(body.data());
  const char kHttpHeaders[] = "HTTP/1.0 200 HONKYDORY\0\0";
  std::string headers(kHttpHeaders, base::size(kHttpHeaders));
  WriteResponse(storage, id, headers, body_buffer.get(), body.length());
}

class UpdateJobTestHelper : public EmbeddedWorkerTestHelper,
                            public ServiceWorkerRegistration::Listener,
                            public ServiceWorkerContextCoreObserver {
 public:
  struct AttributeChangeLogEntry {
    int64_t registration_id;
    blink::mojom::ChangedServiceWorkerObjectsMaskPtr mask;
    ServiceWorkerRegistrationInfo info;
  };

  struct StateChangeLogEntry {
    int64_t version_id;
    ServiceWorkerVersion::Status status;
  };

  UpdateJobTestHelper() : EmbeddedWorkerTestHelper(base::FilePath()) {
    context_wrapper()->AddObserver(this);
    interceptor_ = std::make_unique<URLLoaderInterceptor>(base::BindRepeating(
        &FakeNetwork::HandleRequest, base::Unretained(&fake_network_)));
    fake_network_.SetDefaultResponse(kHeaders, kBody,
                                     /*network_accessed=*/true, net::OK);
  }
  ~UpdateJobTestHelper() override {
    context_wrapper()->RemoveObserver(this);
    if (observed_registration_.get())
      observed_registration_->RemoveListener(this);
  }

  class UpdateJobEmbeddedWorkerInstanceClient
      : public FakeEmbeddedWorkerInstanceClient {
   public:
    UpdateJobEmbeddedWorkerInstanceClient(UpdateJobTestHelper* helper)
        : FakeEmbeddedWorkerInstanceClient(helper) {}
    ~UpdateJobEmbeddedWorkerInstanceClient() override = default;

    void set_force_start_worker_failure(bool force_start_worker_failure) {
      force_start_worker_failure_ = force_start_worker_failure;
    }

    void ResumeAfterDownload() override {
      if (force_start_worker_failure_) {
        host()->OnScriptEvaluationStart();
        host()->OnStarted(
            blink::mojom::ServiceWorkerStartStatus::kAbruptCompletion,
            helper()->GetNextThreadId(),
            blink::mojom::EmbeddedWorkerStartTiming::New());
        return;
      }
      FakeEmbeddedWorkerInstanceClient::ResumeAfterDownload();
    }

   private:
    bool force_start_worker_failure_ = false;
  };

  ServiceWorkerStorage* storage() { return context()->storage(); }
  ServiceWorkerJobCoordinator* job_coordinator() {
    return context()->job_coordinator();
  }

  scoped_refptr<ServiceWorkerRegistration> SetupInitialRegistration(
      const GURL& test_origin) {
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = test_origin.Resolve(kScope);
    scoped_refptr<ServiceWorkerRegistration> registration;

    auto client = std::make_unique<UpdateJobEmbeddedWorkerInstanceClient>(this);
    initial_embedded_worker_instance_client_ = client.get();
    AddPendingInstanceClient(std::move(client));
    base::RunLoop run_loop;
    job_coordinator()->Register(
        test_origin.Resolve(kScript), options,
        blink::mojom::FetchClientSettingsObject::New(),
        SaveRegistration(blink::ServiceWorkerStatusCode::kOk, &registration,
                         run_loop.QuitClosure()));
    run_loop.Run();
    // Wait until the worker becomes active.
    base::RunLoop().RunUntilIdle();
    EXPECT_TRUE(registration.get());
    EXPECT_TRUE(registration->active_version());
    EXPECT_FALSE(registration->installing_version());
    EXPECT_FALSE(registration->waiting_version());
    observed_registration_ = registration;
    return registration;
  }

  // EmbeddedWorkerTestHelper overrides:
  void PopulateScriptCacheMap(int64_t version_id,
                              base::OnceClosure callback) override {
    ServiceWorkerVersion* version = context()->GetLiveVersion(version_id);
    ASSERT_TRUE(version);
    ServiceWorkerRegistration* registration =
        context()->GetLiveRegistration(version->registration_id());
    ASSERT_TRUE(registration);
    GURL script = version->script_url();
    bool is_update = registration->active_version() &&
                     version != registration->active_version();

    int64_t resource_id = storage()->NewResourceId();
    version->script_cache_map()->NotifyStartedCaching(script, resource_id);
    if (!is_update) {
      // Spoof caching the script for the initial version.
      WriteStringResponse(storage(), resource_id, kBody);
      version->script_cache_map()->NotifyFinishedCaching(
          script, sizeof(kBody) / sizeof(char), net::OK, std::string());
    } else {
      EXPECT_NE(NoChangeOrigin(), script.GetOrigin());
      // The script must be changed.
      WriteStringResponse(storage(), resource_id, kNewBody);
      version->script_cache_map()->NotifyFinishedCaching(
          script, sizeof(kNewBody) / sizeof(char), net::OK, std::string());
    }

    version->SetMainScriptHttpResponseInfo(CreateHttpResponseInfo());
    std::move(callback).Run();
  }

  // ServiceWorkerContextCoreObserver overrides
  void OnVersionStateChanged(int64_t version_id,
                             const GURL& scope,
                             ServiceWorkerVersion::Status status) override {
    StateChangeLogEntry entry;
    entry.version_id = version_id;
    entry.status = status;
    state_change_log_.push_back(std::move(entry));
  }

  // ServiceWorkerRegistration::Listener overrides
  void OnVersionAttributesChanged(
      ServiceWorkerRegistration* registration,
      blink::mojom::ChangedServiceWorkerObjectsMaskPtr changed_mask,
      const ServiceWorkerRegistrationInfo& info) override {
    AttributeChangeLogEntry entry;
    entry.registration_id = registration->id();
    entry.mask = std::move(changed_mask);
    entry.info = info;
    attribute_change_log_.push_back(std::move(entry));
  }

  void OnRegistrationFailed(ServiceWorkerRegistration* registration) override {
    registration_failed_ = true;
  }

  void OnUpdateFound(ServiceWorkerRegistration* registration) override {
    update_found_ = true;
  }

  UpdateJobEmbeddedWorkerInstanceClient*
      initial_embedded_worker_instance_client_ = nullptr;
  scoped_refptr<ServiceWorkerRegistration> observed_registration_;
  std::vector<AttributeChangeLogEntry> attribute_change_log_;
  std::vector<StateChangeLogEntry> state_change_log_;
  bool update_found_ = false;
  bool registration_failed_ = false;
  bool force_start_worker_failure_ = false;
  base::Optional<bool> will_be_terminated_;

  // These are used only when ServiceWorkerImportedScriptUpdateCheck is enabled.
  FakeNetwork fake_network_;
  std::unique_ptr<URLLoaderInterceptor> interceptor_;

  base::WeakPtrFactory<UpdateJobTestHelper> weak_factory_{this};
};

}  // namespace

// This class is for cases that can be impacted by different update check
// types.
class ServiceWorkerUpdateJobTest : public ServiceWorkerJobTest {
 public:
  void SetUp() override {
    update_helper_ = new UpdateJobTestHelper();
    helper_.reset(update_helper_);

    // Create a StoragePartition with the testing browser context so that the
    // ServiceWorkerUpdateChecker can find the BrowserContext through it.
    storage_partition_impl_ = StoragePartitionImpl::Create(
        helper_->browser_context(), /* in_memory= */ true, base::FilePath(),
        /* partition_domain= */ "");
    storage_partition_impl_->Initialize();
    helper_->context_wrapper()->set_storage_partition(
        storage_partition_impl_.get());
  }

 protected:
  std::unique_ptr<StoragePartitionImpl> storage_partition_impl_;
  UpdateJobTestHelper* update_helper_;
};

// Make sure that the same registration is used and the update_via_cache value
// is updated when registering a service worker with the same parameter except
// for updateViaCache.
TEST_F(ServiceWorkerUpdateJobTest, RegisterWithDifferentUpdateViaCache) {
  const GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");

  scoped_refptr<ServiceWorkerRegistration> old_registration =
      RunRegisterJob(script_url, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(blink::mojom::ServiceWorkerUpdateViaCache::kImports,
            old_registration->update_via_cache());

  // During the above registration, a service worker registration object host
  // for ServiceWorkerGlobalScope#registration has been created/added into
  // |provider_host|.
  ServiceWorkerProviderHost* provider_host =
      old_registration->active_version()->provider_host();
  ASSERT_TRUE(provider_host);
  ServiceWorkerContainerHost* container_host = provider_host->container_host();

  // Remove references to |old_registration| so that |old_registration| is the
  // only reference to the registration.
  container_host->service_worker_object_hosts_.clear();
  EXPECT_EQ(1UL, container_host->registration_object_hosts_.size());
  container_host->registration_object_hosts_.clear();
  EXPECT_EQ(0UL, container_host->registration_object_hosts_.size());
  EXPECT_TRUE(old_registration->HasOneRef());

  EXPECT_TRUE(FindRegistrationForScope(options.scope));

  base::HistogramTester histogram_tester;
  options.update_via_cache = blink::mojom::ServiceWorkerUpdateViaCache::kNone;
  scoped_refptr<ServiceWorkerRegistration> new_registration =
      RunRegisterJob(script_url, options);

  // Update check succeeds but no update is found.
  histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                     blink::ServiceWorkerStatusCode::kOk, 1);
  histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                     false, 1);

  // Ensure that the registration object is not copied.
  EXPECT_EQ(old_registration, new_registration);
  EXPECT_EQ(blink::mojom::ServiceWorkerUpdateViaCache::kNone,
            new_registration->update_via_cache());

  scoped_refptr<ServiceWorkerRegistration> new_registration_by_scope =
      FindRegistrationForScope(options.scope);

  EXPECT_EQ(new_registration_by_scope, old_registration);
}

TEST_F(ServiceWorkerUpdateJobTest, Update_NoChange) {
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper_->SetupInitialRegistration(NoChangeOrigin());
  ASSERT_TRUE(registration.get());
  ASSERT_EQ(4u, update_helper_->state_change_log_.size());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLING,
            update_helper_->state_change_log_[0].status);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED,
            update_helper_->state_change_log_[1].status);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING,
            update_helper_->state_change_log_[2].status);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED,
            update_helper_->state_change_log_[3].status);
  update_helper_->state_change_log_.clear();

  // Run the update job.
  base::HistogramTester histogram_tester;
  registration->AddListener(update_helper_);
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->StartUpdate();
  base::RunLoop().RunUntilIdle();

  // Update check succeeds but no update is found.
  histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                     blink::ServiceWorkerStatusCode::kOk, 1);
  histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                     false, 1);

  // Verify results.
  ASSERT_TRUE(registration->active_version());
  EXPECT_EQ(first_version.get(), registration->active_version());
  EXPECT_FALSE(registration->installing_version());
  EXPECT_FALSE(registration->waiting_version());
  EXPECT_TRUE(update_helper_->attribute_change_log_.empty());
  EXPECT_FALSE(update_helper_->update_found_);
}

TEST_F(ServiceWorkerUpdateJobTest, Update_BumpLastUpdateCheckTime) {
  const base::Time kToday = base::Time::Now();
  const base::Time kYesterday =
      kToday - base::TimeDelta::FromDays(1) - base::TimeDelta::FromHours(1);

  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper_->SetupInitialRegistration(NoChangeOrigin());
  ASSERT_TRUE(registration.get());

  registration->AddListener(update_helper_);

  // Run an update where the script did not change and the network was not
  // accessed. The check time should not be updated.
  // Set network not accessed.
  update_helper_->fake_network_.SetResponse(
      NoChangeOrigin().Resolve(kScript), kHeaders, kBody,
      /*network_accessed=*/false, net::OK);

  {
    base::HistogramTester histogram_tester;
    registration->set_last_update_check(kToday);
    registration->active_version()->StartUpdate();
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ(kToday, registration->last_update_check());
    EXPECT_FALSE(update_helper_->update_found_);

    // Update check succeeds but no update is found.
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                       blink::ServiceWorkerStatusCode::kOk, 1);
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                       false, 1);
  }

  // Run an update where the script did not change and the network was
  // accessed. The check time should be updated.
  // Set network accessed.
  update_helper_->fake_network_.SetResponse(NoChangeOrigin().Resolve(kScript),
                                            kHeaders, kBody,
                                            /*network_accessed=*/true, net::OK);

  {
    base::HistogramTester histogram_tester;
    registration->set_last_update_check(kYesterday);
    registration->active_version()->StartUpdate();
    base::RunLoop().RunUntilIdle();
    EXPECT_LT(kYesterday, registration->last_update_check());
    EXPECT_FALSE(update_helper_->update_found_);
    registration->RemoveListener(update_helper_);
    registration = update_helper_->SetupInitialRegistration(NewVersionOrigin());
    ASSERT_TRUE(registration.get());
    // Update check succeeds but no update is found.
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                       blink::ServiceWorkerStatusCode::kOk, 1);
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                       false, 1);
  }

  registration->AddListener(update_helper_);

  // Run an update where the script changed. The check time should be updated.
  // Change script body.
  update_helper_->fake_network_.SetResponse(NewVersionOrigin().Resolve(kScript),
                                            kHeaders, kNewBody,
                                            /*network_accessed=*/true, net::OK);
  {
    base::HistogramTester histogram_tester;
    registration->set_last_update_check(kYesterday);
    registration->active_version()->StartUpdate();
    base::RunLoop().RunUntilIdle();
    EXPECT_LT(kYesterday, registration->last_update_check());
    // Update check succeeds and update is found.
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                       blink::ServiceWorkerStatusCode::kOk, 1);
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                       true, 1);
  }

  // Run an update to a worker that loads successfully but fails to start up
  // (script evaluation failure). The check time should be updated.
  auto* embedded_worker_instance_client =
      update_helper_->AddNewPendingInstanceClient<
          UpdateJobTestHelper::UpdateJobEmbeddedWorkerInstanceClient>(
          update_helper_);
  embedded_worker_instance_client->set_force_start_worker_failure(true);
  registration->set_last_update_check(kYesterday);
  // Change script body.
  update_helper_->fake_network_.SetResponse(NewVersionOrigin().Resolve(kScript),
                                            kHeaders, kBody,
                                            /*network_accessed=*/true, net::OK);
  {
    base::HistogramTester histogram_tester;
    registration->active_version()->StartUpdate();
    base::RunLoop().RunUntilIdle();
    EXPECT_LT(kYesterday, registration->last_update_check());
    // Update check succeeds and update is found even when starting a worker
    // fails.
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                       blink::ServiceWorkerStatusCode::kOk, 1);
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                       true, 1);
  }
}

TEST_F(ServiceWorkerUpdateJobTest, Update_NewVersion) {
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper_->SetupInitialRegistration(NewVersionOrigin());
  ASSERT_TRUE(registration.get());
  update_helper_->state_change_log_.clear();
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);

  // Run the update job and an update is found.
  // Change script body.
  update_helper_->fake_network_.SetResponse(NewVersionOrigin().Resolve(kScript),
                                            kHeaders, kNewBody,
                                            /*network_accessed=*/true, net::OK);

  base::HistogramTester histogram_tester;
  registration->AddListener(update_helper_);
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->StartUpdate();
  base::RunLoop().RunUntilIdle();
  // Update check succeeds and update is found.
  histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                     blink::ServiceWorkerStatusCode::kOk, 1);
  histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                     true, 1);

  // The worker is updated after RequestTermination() is called from the
  // renderer. Until then, the active version stays active.
  EXPECT_EQ(first_version.get(), registration->active_version());
  // The new worker is installed but not yet to be activated.
  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();
  EXPECT_EQ(2u, update_helper_->attribute_change_log_.size());
  UpdateJobTestHelper::UpdateJobEmbeddedWorkerInstanceClient* client =
      update_helper_->initial_embedded_worker_instance_client_;
  RequestTermination(&client->host());

  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(new_version.get(), runner);

  // Pump the loop again. This ensures |update_helper_| observes all
  // the status changes, since RunUntilActivated() only ensured
  // ServiceWorkerJobTest did.
  base::RunLoop().RunUntilIdle();

  // Verify results.
  ASSERT_TRUE(registration->active_version());
  EXPECT_NE(first_version.get(), registration->active_version());
  EXPECT_FALSE(registration->installing_version());
  EXPECT_FALSE(registration->waiting_version());
  ASSERT_EQ(3u, update_helper_->attribute_change_log_.size());

  {
    const UpdateJobTestHelper::AttributeChangeLogEntry& entry =
        update_helper_->attribute_change_log_[0];
    EXPECT_TRUE(entry.mask->installing);
    EXPECT_FALSE(entry.mask->waiting);
    EXPECT_FALSE(entry.mask->active);
    EXPECT_NE(entry.info.installing_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
    EXPECT_EQ(entry.info.waiting_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
    EXPECT_NE(entry.info.active_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
  }

  {
    const UpdateJobTestHelper::AttributeChangeLogEntry& entry =
        update_helper_->attribute_change_log_[1];
    EXPECT_TRUE(entry.mask->installing);
    EXPECT_TRUE(entry.mask->waiting);
    EXPECT_FALSE(entry.mask->active);
    EXPECT_EQ(entry.info.installing_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
    EXPECT_NE(entry.info.waiting_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
    EXPECT_NE(entry.info.active_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
  }

  {
    const UpdateJobTestHelper::AttributeChangeLogEntry& entry =
        update_helper_->attribute_change_log_[2];
    EXPECT_FALSE(entry.mask->installing);
    EXPECT_TRUE(entry.mask->waiting);
    EXPECT_TRUE(entry.mask->active);
    EXPECT_EQ(entry.info.installing_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
    EXPECT_EQ(entry.info.waiting_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
    EXPECT_NE(entry.info.active_version.version_id,
              blink::mojom::kInvalidServiceWorkerVersionId);
  }

  // expected version state transitions:
  // new.installing, new.installed,
  // old.redundant,
  // new.activating, new.activated
  ASSERT_EQ(5u, update_helper_->state_change_log_.size());

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper_->state_change_log_[0].version_id);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLING,
            update_helper_->state_change_log_[0].status);

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper_->state_change_log_[1].version_id);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED,
            update_helper_->state_change_log_[1].status);

  EXPECT_EQ(first_version->version_id(),
            update_helper_->state_change_log_[2].version_id);
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT,
            update_helper_->state_change_log_[2].status);

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper_->state_change_log_[3].version_id);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING,
            update_helper_->state_change_log_[3].status);

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper_->state_change_log_[4].version_id);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED,
            update_helper_->state_change_log_[4].status);

  EXPECT_TRUE(update_helper_->update_found_);
}

// Test that the update job uses the script URL of the newest worker when the
// job starts, rather than when it is scheduled.
TEST_F(ServiceWorkerUpdateJobTest, Update_ScriptUrlChanged) {
  const GURL old_script("https://www.example.com/service_worker.js");
  const GURL new_script("https://www.example.com/new_worker.js");

  // Create a registration with an active version.
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/one/");
  auto* initial_client =
      helper_->AddNewPendingInstanceClient<FakeEmbeddedWorkerInstanceClient>(
          helper_.get());
  // Setup the old script response.
  update_helper_->fake_network_.SetResponse(old_script, kHeaders, kBody,
                                            /*network_accessed=*/true, net::OK);
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(old_script, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);

  // Queue an Update. When this runs, it will use the waiting version's script.
  job_coordinator()->Update(registration.get(), false);

  // Add a waiting version with a new script.
  scoped_refptr<ServiceWorkerVersion> version = new ServiceWorkerVersion(
      registration.get(), new_script, blink::mojom::ScriptType::kClassic,
      2L /* dummy version id */, helper_->context()->AsWeakPtr());
  registration->SetWaitingVersion(version);

  // Setup the new script response.
  update_helper_->fake_network_.SetResponse(new_script, kHeaders, kNewBody,
                                            /*network_accessed=*/true, net::OK);

  // Make sure the storage has the data of the current waiting version.
  const int64_t resource_id = 2;
  version->script_cache_map()->NotifyStartedCaching(new_script, resource_id);
  WriteStringResponse(update_helper_->storage(), resource_id, kBody);
  version->script_cache_map()->NotifyFinishedCaching(
      new_script, sizeof(kBody) / sizeof(char), net::OK, std::string());

  // Run the update job.
  base::RunLoop().RunUntilIdle();

  // The worker is activated after RequestTermination() is called from the
  // renderer. Until then, the active version stays active.
  // Still waiting, but the waiting version isn't |version| since another
  // ServiceWorkerVersion is created during the update job and the job wipes
  // out the older waiting version.
  ServiceWorkerVersion* waiting_version = registration->waiting_version();
  EXPECT_TRUE(registration->active_version());
  EXPECT_TRUE(waiting_version);
  EXPECT_NE(version.get(), waiting_version);

  RequestTermination(&initial_client->host());
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(waiting_version, runner);

  // The update job should have created a new version with the new script,
  // and promoted it to the active version.
  EXPECT_EQ(new_script, registration->active_version()->script_url());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(nullptr, registration->installing_version());
}

// Test that update fails if the incumbent worker was evicted
// during the update job (this can happen on disk cache failure).
TEST_F(ServiceWorkerUpdateJobTest, Update_EvictedIncumbent) {
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper_->SetupInitialRegistration(NewVersionOrigin());
  ASSERT_TRUE(registration.get());
  update_helper_->state_change_log_.clear();

  registration->AddListener(update_helper_);
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  auto* instance_client = helper_->AddNewPendingInstanceClient<
      DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get());

  // Start the update job and make it block on the worker starting.
  // Evict the incumbent during that time.
  // Change script body.
  update_helper_->fake_network_.SetResponse(NewVersionOrigin().Resolve(kScript),
                                            kHeaders, kNewBody,
                                            /*network_accessed=*/true, net::OK);

  first_version->StartUpdate();
  instance_client->RunUntilStartWorker();
  registration->ForceDelete();

  // Finish the update job.
  instance_client->UnblockStartWorker();
  base::RunLoop().RunUntilIdle();

  // Verify results.
  EXPECT_FALSE(registration->GetNewestVersion());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, first_version->status());
  EXPECT_TRUE(update_helper_->attribute_change_log_.empty());
  EXPECT_FALSE(update_helper_->update_found_);
  EXPECT_TRUE(update_helper_->registration_failed_);
  EXPECT_TRUE(registration->is_uninstalled());
}

TEST_F(ServiceWorkerUpdateJobTest, Update_UninstallingRegistration) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/one/");
  scoped_refptr<ServiceWorkerRegistration> registration = RunRegisterJob(
      GURL("https://www.example.com/service_worker.js"), options);
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilActivated(registration->installing_version(), runner);

  // Add a controllee and queue an unregister to force the uninstalling state.
  ServiceWorkerContainerHost* container_host = CreateControllee();
  ServiceWorkerVersion* active_version = registration->active_version();
  active_version->AddControllee(container_host);
  base::RunLoop run_loop;
  job_coordinator()->Unregister(
      GURL("https://www.example.com/one/"),
      SaveUnregistration(blink::ServiceWorkerStatusCode::kOk,
                         run_loop.QuitClosure()));

  // Update should abort after it starts and sees uninstalling.
  job_coordinator()->Update(registration.get(), false);

  run_loop.Run();

  // Verify the registration was not modified by the Update.
  EXPECT_TRUE(registration->is_uninstalling());
  EXPECT_EQ(active_version, registration->active_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(nullptr, registration->installing_version());
}

TEST_F(ServiceWorkerUpdateJobTest, RegisterMultipleTimesWhileUninstalling) {
  GURL script1("https://www.example.com/service_worker.js?first");
  GURL script2("https://www.example.com/service_worker.js?second");
  GURL script3("https://www.example.com/service_worker.js?third");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/one/");

  auto* initial_client =
      helper_->AddNewPendingInstanceClient<FakeEmbeddedWorkerInstanceClient>(
          helper_.get());
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);
  // Wait until the worker becomes actvie.
  base::RunLoop().RunUntilIdle();
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee and queue an unregister to force the uninstalling state.
  ServiceWorkerContainerHost* container_host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->AddControllee(container_host);
  RunUnregisterJob(options.scope);

  EXPECT_EQ(registration, RunRegisterJob(script2, options));
  // Wait until the worker becomes installed.
  base::RunLoop().RunUntilIdle();

  scoped_refptr<ServiceWorkerVersion> second_version =
      registration->waiting_version();
  ASSERT_TRUE(second_version);

  RunUnregisterJob(options.scope);

  EXPECT_TRUE(registration->is_uninstalling());

  EXPECT_EQ(registration, RunRegisterJob(script3, options));
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilStatusChange(second_version.get(),
                                ServiceWorkerVersion::REDUNDANT);
  scoped_refptr<ServiceWorkerVersion> third_version =
      registration->waiting_version();
  ASSERT_TRUE(third_version);

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, second_version->status());

  first_version->RemoveControllee(container_host->client_uuid());
  RequestTermination(&initial_client->host());

  // Wait for activated.
  observer.RunUntilActivated(third_version.get(), runner);

  // Verify the new version is activated.
  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(third_version, registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, third_version->status());
}

class CheckPauseAfterDownloadEmbeddedWorkerInstanceClient
    : public FakeEmbeddedWorkerInstanceClient {
 public:
  explicit CheckPauseAfterDownloadEmbeddedWorkerInstanceClient(
      EmbeddedWorkerTestHelper* helper)
      : FakeEmbeddedWorkerInstanceClient(helper) {}

  int num_of_startworker() const { return num_of_startworker_; }
  void set_next_pause_after_download(bool expectation) {
    next_pause_after_download_ = expectation;
  }

 protected:
  void StartWorker(blink::mojom::EmbeddedWorkerStartParamsPtr params) override {
    ASSERT_TRUE(next_pause_after_download_.has_value());
    EXPECT_EQ(next_pause_after_download_.value(), params->pause_after_download);
    num_of_startworker_++;
    FakeEmbeddedWorkerInstanceClient::StartWorker(std::move(params));
  }

 private:
  base::Optional<bool> next_pause_after_download_;
  int num_of_startworker_ = 0;
  DISALLOW_COPY_AND_ASSIGN(CheckPauseAfterDownloadEmbeddedWorkerInstanceClient);
};

// Test that activation doesn't complete if it's triggered by removing a
// controllee and starting the worker failed due to shutdown.
TEST_F(ServiceWorkerUpdateJobTest, ActivateCancelsOnShutdown) {
  GURL script("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");

  auto* initial_client =
      helper_->AddNewPendingInstanceClient<FakeEmbeddedWorkerInstanceClient>(
          helper_.get());
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script, options);
  // Wait until the worker becomes active.
  base::RunLoop().RunUntilIdle();
  auto runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee.
  ServiceWorkerContainerHost* container_host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->AddControllee(container_host);

  // Update. The new version should be waiting.
  // Change script body.
  update_helper_->fake_network_.SetResponse(script, kHeaders, kNewBody,
                                            /*network_accessed=*/true, net::OK);

  registration->AddListener(update_helper_);
  first_version->StartUpdate();
  base::RunLoop().RunUntilIdle();
  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();
  ASSERT_TRUE(new_version);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, new_version->status());

  // Stop the worker so that it must start again when activation is attempted.
  // (This is not strictly necessary to exercise the codepath, but it makes it
  // easy to cause a failure with set_force_start_worker_failure after
  // shutdown is simulated. Otherwise our test helper often fails on
  // DCHECK(context)).
  new_version->StopWorker(base::DoNothing());

  // Remove the controllee. The new version should be activating, and delayed
  // until the runner runs again.
  first_version->RemoveControllee(container_host->client_uuid());
  base::RunLoop().RunUntilIdle();

  // Activating the new version won't happen until
  // RequestTermination() is called.
  EXPECT_EQ(first_version.get(), registration->active_version());
  RequestTermination(&initial_client->host());

  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilStatusChange(new_version.get(),
                                ServiceWorkerVersion::ACTIVATING);
  EXPECT_EQ(new_version.get(), registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, new_version->status());

  // Shutdown.
  update_helper_->context()->wrapper()->Shutdown();
  auto* embedded_worker_instance_client =
      update_helper_->AddNewPendingInstanceClient<
          UpdateJobTestHelper::UpdateJobEmbeddedWorkerInstanceClient>(
          update_helper_);
  embedded_worker_instance_client->set_force_start_worker_failure(true);

  // Allow the activation to continue. It will fail, and the worker
  // should not be promoted to ACTIVATED because failure occur
  // during shutdown.
  runner->RunPendingTasks();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(new_version.get(), registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, new_version->status());
  registration->RemoveListener(update_helper_);
  // Dispatch Mojo messages for those Mojo interfaces bound on |runner| to
  // avoid possible memory leak.
  runner->RunUntilIdle();
}

class ServiceWorkerUpdateJobTestWithCrossOriginIsolation
    : public ServiceWorkerUpdateJobTest {
 public:
  ServiceWorkerUpdateJobTestWithCrossOriginIsolation() {
    feature_list_.InitAndEnableFeature(
        ::network::features::kCrossOriginIsolation);
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

// Update job should handle the COEP header appropriately.
TEST_F(ServiceWorkerUpdateJobTestWithCrossOriginIsolation,
       Update_CrossOriginEmbedderPolicy) {
  const char kHeadersWithRequireCorp[] = R"(HTTP/1.1 200 OK
Content-Type: application/javascript
Cross-Origin-Embedder-Policy: require-corp

)";
  const char kHeadersWithNone[] = R"(HTTP/1.1 200 OK
Content-Type: application/javascript
Cross-Origin-Embedder-Policy: none

)";

  const base::Time kToday = base::Time::Now();
  const base::Time kYesterday =
      kToday - base::TimeDelta::FromDays(1) - base::TimeDelta::FromHours(1);

  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper_->SetupInitialRegistration(NewVersionOrigin());
  ASSERT_TRUE(registration.get());
  EXPECT_EQ(network::mojom::CrossOriginEmbedderPolicy::kNone,
            registration->active_version()->cross_origin_embedder_policy());

  registration->AddListener(update_helper_);

  // Run an update where the response header is updated but the script did not
  // change. No update is found but the last update check time is updated.
  update_helper_->fake_network_.SetResponse(NewVersionOrigin().Resolve(kScript),
                                            kHeadersWithRequireCorp, kBody,
                                            /*network_accessed=*/true, net::OK);

  {
    base::HistogramTester histogram_tester;
    registration->set_last_update_check(kYesterday);
    registration->active_version()->StartUpdate();
    base::RunLoop().RunUntilIdle();
    EXPECT_LT(kYesterday, registration->last_update_check());
    EXPECT_FALSE(update_helper_->update_found_);

    // Update check succeeds but no update is found.
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                       blink::ServiceWorkerStatusCode::kOk, 1);
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                       false, 1);
  }

  // Run an update where the COEP value and the script changed.
  update_helper_->fake_network_.SetResponse(NewVersionOrigin().Resolve(kScript),
                                            kHeadersWithRequireCorp, kNewBody,
                                            /*network_accessed=*/true, net::OK);
  {
    base::HistogramTester histogram_tester;
    registration->set_last_update_check(kYesterday);
    registration->active_version()->StartUpdate();
    base::RunLoop().RunUntilIdle();
    EXPECT_LT(kYesterday, registration->last_update_check());
    EXPECT_TRUE(update_helper_->update_found_);
    // Update check succeeds and update is found.
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                       blink::ServiceWorkerStatusCode::kOk, 1);
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                       true, 1);
    ASSERT_NE(nullptr, registration->waiting_version());
    EXPECT_EQ(network::mojom::CrossOriginEmbedderPolicy::kRequireCorp,
              registration->waiting_version()->cross_origin_embedder_policy());
  }

  // Run an update again where the COEP value and the body has been updated. The
  // COEP value should be updated appropriately.
  update_helper_->fake_network_.SetResponse(NewVersionOrigin().Resolve(kScript),
                                            kHeadersWithNone, kBody,
                                            /*network_accessed=*/true, net::OK);
  {
    base::HistogramTester histogram_tester;
    registration->set_last_update_check(kYesterday);
    registration->active_version()->StartUpdate();
    base::RunLoop().RunUntilIdle();
    EXPECT_LT(kYesterday, registration->last_update_check());
    EXPECT_TRUE(update_helper_->update_found_);
    // Update check succeeds and update is found.
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.Result",
                                       blink::ServiceWorkerStatusCode::kOk, 1);
    histogram_tester.ExpectBucketCount("ServiceWorker.UpdateCheck.UpdateFound",
                                       true, 1);
    ASSERT_NE(nullptr, registration->waiting_version());
    EXPECT_EQ(network::mojom::CrossOriginEmbedderPolicy::kNone,
              registration->waiting_version()->cross_origin_embedder_policy());
  }
}

class WaitForeverInstallWorker : public FakeServiceWorker {
 public:
  WaitForeverInstallWorker(EmbeddedWorkerTestHelper* helper)
      : FakeServiceWorker(helper) {}
  ~WaitForeverInstallWorker() override = default;

  void DispatchInstallEvent(
      blink::mojom::ServiceWorker::DispatchInstallEventCallback callback)
      override {
    callback_ = std::move(callback);
  }

 private:
  blink::mojom::ServiceWorker::DispatchInstallEventCallback callback_;
};

// Test that the job queue doesn't get stuck by bad workers.
TEST_F(ServiceWorkerJobTest, TimeoutBadJobs) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");

  // Make a job that gets stuck due to a worker stalled in starting.
  base::RunLoop loop1;
  scoped_refptr<ServiceWorkerRegistration> registration1;
  helper_->AddPendingInstanceClient(
      std::make_unique<DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get()));
  job_coordinator()->Register(
      GURL("https://www.example.com/service_worker1.js"), options,
      blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kErrorTimeout,
                       &registration1, loop1.QuitClosure()));

  task_environment_.FastForwardBy(base::TimeDelta::FromMinutes(1));

  // Make a job that gets stuck due to a worker that doesn't finish the install
  // event. The callback is called with kOk, but the job will be stuck until
  // the install event times out.
  base::RunLoop loop2;
  helper_->AddPendingServiceWorker(
      std::make_unique<WaitForeverInstallWorker>(helper_.get()));
  scoped_refptr<ServiceWorkerRegistration> registration2;
  job_coordinator()->Register(
      GURL("https://www.example.com/service_worker2.js"), options,
      blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kOk, &registration2,
                       loop2.QuitClosure()));

  task_environment_.FastForwardBy(base::TimeDelta::FromMinutes(1));

  // Make a normal job.
  base::RunLoop loop3;
  scoped_refptr<ServiceWorkerRegistration> registration3;
  job_coordinator()->Register(
      GURL("https://www.example.com/service_worker3.js"), options,
      blink::mojom::FetchClientSettingsObject::New(),
      SaveRegistration(blink::ServiceWorkerStatusCode::kOk, &registration3,
                       loop3.QuitClosure()));

  task_environment_.FastForwardBy(base::TimeDelta::FromMinutes(1));

  // Timeout the first job.
  task_environment_.FastForwardBy(base::TimeDelta::FromMinutes(2));
  loop1.Run();

  // Let the second job run until the install event is dispatched, then
  // time out the event.
  loop2.Run();
  scoped_refptr<ServiceWorkerVersion> version =
      registration2->installing_version();
  ASSERT_TRUE(version);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLING, version->status());
  task_environment_.FastForwardBy(base::TimeDelta::FromMinutes(5));
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());

  // Let the third job finish successfully. It might have already been
  // progressing so we don't know what state its worker is in, but it will
  // eventually reach ACTIVATED.
  loop3.Run();
  version = registration3->GetNewestVersion();
  ASSERT_TRUE(version);
  TestServiceWorkerObserver observer(helper_->context_wrapper());
  observer.RunUntilStatusChange(version.get(), ServiceWorkerVersion::ACTIVATED);
}

}  // namespace content
