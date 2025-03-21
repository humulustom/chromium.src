// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_updated_script_loader.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include "base/bind_helpers.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/service_worker_consts.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_disk_cache.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/browser/url_loader_factory_getter.h"
#include "content/public/test/browser_task_environment.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/load_flags.h"
#include "net/base/test_completion_callback.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/test/test_url_loader_client.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"

namespace content {
namespace service_worker_updated_script_loader_unittest {

const char kNormalScriptURL[] = "https://example.com/normal.js";

// MockHTTPServer is a utility to provide mocked responses for
// ServiceWorkerUpdatedScriptLoader.
class MockHTTPServer {
 public:
  struct Response {
    Response(const std::string& headers, const std::string& body)
        : headers(headers), body(body) {}

    const std::string headers;
    const std::string body;
    bool has_certificate_error = false;
  };

  void Set(const GURL& url, const Response& response) {
    responses_.erase(url);
    responses_.emplace(url, response);
  }

  const Response& Get(const GURL& url) {
    auto found = responses_.find(url);
    EXPECT_TRUE(found != responses_.end());
    return found->second;
  }

 private:
  std::map<GURL, Response> responses_;
};

// ServiceWorkerUpdatedScriptLoaderTest is for testing the handling of requests
// for installing service worker scripts via ServiceWorkerUpdatedScriptLoader.
class ServiceWorkerUpdatedScriptLoaderTest : public testing::Test {
 public:
  ServiceWorkerUpdatedScriptLoaderTest()
      : task_environment_(BrowserTaskEnvironment::IO_MAINLOOP),
        kScriptURL(kNormalScriptURL) {}
  ~ServiceWorkerUpdatedScriptLoaderTest() override = default;

  ServiceWorkerContextCore* context() { return helper_->context(); }

  void SetUp() override {
    helper_ = std::make_unique<EmbeddedWorkerTestHelper>(base::FilePath());
    context()->storage()->LazyInitializeForTest();
    SetUpRegistration(kScriptURL);

    // Create the old script resource in storage.
    WriteToDiskCacheSync(context()->storage(), kScriptURL, kOldResourceId,
                         kOldHeaders, kOldData, std::string());
  }

  // Sets up ServiceWorkerRegistration and ServiceWorkerVersion. This should be
  // called before DoRequest().
  void SetUpRegistration(const GURL& script_url) {
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = script_url.GetWithoutFilename();
    SetUpRegistrationWithOptions(script_url, options);
  }
  void SetUpRegistrationWithOptions(
      const GURL& script_url,
      blink::mojom::ServiceWorkerRegistrationOptions options) {
    registration_ = context()->registry()->CreateNewRegistration(options);
    SetUpVersion(script_url);
  }

  // After this is called, |version_| will be a new, uninstalled version. The
  // next time DoRequest() is called, |version_| will attempt to install,
  // possibly updating if registration has an installed worker.
  void SetUpVersion(const GURL& script_url) {
    version_ = context()->registry()->CreateNewVersion(
        registration_.get(), script_url, blink::mojom::ScriptType::kClassic);
    version_->SetStatus(ServiceWorkerVersion::NEW);
  }

  void DoRequest(
      const GURL& url,
      std::unique_ptr<network::TestURLLoaderClient>* out_client,
      std::unique_ptr<ServiceWorkerUpdatedScriptLoader>* out_loader) {
    DCHECK(registration_);
    DCHECK(version_);

    // Dummy values.
    uint32_t options = 0;

    network::ResourceRequest request;
    request.url = url;
    request.method = "GET";
    request.resource_type = static_cast<int>((url == version_->script_url())
                                                 ? ResourceType::kServiceWorker
                                                 : ResourceType::kScript);

    *out_client = std::make_unique<network::TestURLLoaderClient>();
    *out_loader = ServiceWorkerUpdatedScriptLoader::CreateAndStart(
        options, request, (*out_client)->CreateRemote(), version_);
  }

  int64_t LookupResourceId(const GURL& url) {
    return version_->script_cache_map()->LookupResourceId(url);
  }

  void SetUpComparedScriptInfo(
      size_t bytes_compared,
      const std::string& new_headers,
      const std::string& diff_data_block,
      ServiceWorkerUpdatedScriptLoader::LoaderState network_loader_state,
      ServiceWorkerUpdatedScriptLoader::WriterState body_writer_state) {
    ServiceWorkerUpdateCheckTestUtils::CreateAndSetComparedScriptInfoForVersion(
        kScriptURL, bytes_compared, new_headers, diff_data_block,
        kOldResourceId, kNewResourceId, helper_.get(), network_loader_state,
        body_writer_state,
        ServiceWorkerSingleScriptUpdateChecker::Result::kDifferent,
        version_.get(), &network_producer_);
  }

  void NotifyLoaderCompletion(net::Error error) {
    network::URLLoaderCompletionStatus status;
    status.error_code = error;
    loader_->OnComplete(status);
  }

  // Verify the received response.
  void CheckReceivedResponse(const std::string& expected_body) {
    EXPECT_TRUE(client_->has_received_response());
    EXPECT_TRUE(client_->response_body().is_valid());

    // The response should also be stored in the storage.
    EXPECT_TRUE(ServiceWorkerUpdateCheckTestUtils::VerifyStoredResponse(
        LookupResourceId(kScriptURL), context()->storage(), expected_body));

    std::string response;
    EXPECT_TRUE(mojo::BlockingCopyToString(client_->response_body_release(),
                                           &response));
    EXPECT_EQ(expected_body, response);
  }

 protected:
  BrowserTaskEnvironment task_environment_;
  std::unique_ptr<EmbeddedWorkerTestHelper> helper_;

  scoped_refptr<ServiceWorkerRegistration> registration_;
  scoped_refptr<ServiceWorkerVersion> version_;

  const GURL kScriptURL;
  std::unique_ptr<network::TestURLLoaderClient> client_;
  std::unique_ptr<ServiceWorkerUpdatedScriptLoader> loader_;
  const std::vector<std::pair<std::string, std::string>> kOldHeaders = {
      {"Content-Type", "text/javascript"},
      {"Content-Length", "14"}};
  const std::string kOldData = "old-block-data";
  const int64_t kOldResourceId = 1;
  const int64_t kNewResourceId = 2;
  mojo::ScopedDataPipeProducerHandle network_producer_;
};

// Tests the loader when the first script data block is different.
TEST_F(ServiceWorkerUpdatedScriptLoaderTest, FirstBlockDifferent) {
  const std::string kNewHeaders =
      "HTTP/1.0 200 OK\0Content-Type: text/javascript\0Content-Length: 24\0\0";
  const std::string kDiffBlock = "diff-block-";
  const std::string kNetworkBlock = "network-block";
  const std::string kNewData = kDiffBlock + kNetworkBlock;

  SetUpComparedScriptInfo(
      0, kNewHeaders, kDiffBlock,
      ServiceWorkerUpdatedScriptLoader::LoaderState::kLoadingBody,
      ServiceWorkerUpdatedScriptLoader::WriterState::kWriting);

  DoRequest(kScriptURL, &client_, &loader_);

  // Send network data.
  ASSERT_TRUE(mojo::BlockingCopyFromString(kNetworkBlock, network_producer_));
  network_producer_.reset();

  // Notify the completion of network loader.
  NotifyLoaderCompletion(net::OK);
  client_->RunUntilComplete();

  EXPECT_EQ(net::OK, client_->completion_status().error_code);

  // The client should have received the response.
  CheckReceivedResponse(kNewData);
}

// Tests the loader when the script data block in the middle is different.
TEST_F(ServiceWorkerUpdatedScriptLoaderTest, MiddleBlockDifferent) {
  const std::string kNewHeaders =
      "HTTP/1.0 200 OK\0Content-Type: text/javascript\0Content-Length: 34\0\0";
  const std::string kSameBlock = "old-block";
  const std::string kDiffBlock = "|diff-block|";
  const std::string kNetworkBlock = "network-block";
  const std::string kNewData = kSameBlock + kDiffBlock + kNetworkBlock;

  SetUpComparedScriptInfo(
      kSameBlock.length(), kNewHeaders, kDiffBlock,
      ServiceWorkerUpdatedScriptLoader::LoaderState::kLoadingBody,
      ServiceWorkerUpdatedScriptLoader::WriterState::kWriting);

  DoRequest(kScriptURL, &client_, &loader_);

  // Send network data.
  ASSERT_TRUE(mojo::BlockingCopyFromString(kNetworkBlock, network_producer_));
  network_producer_.reset();

  // Notify the completion of network loader.
  NotifyLoaderCompletion(net::OK);
  client_->RunUntilComplete();

  EXPECT_EQ(net::OK, client_->completion_status().error_code);

  // The client should have received the response.
  CheckReceivedResponse(kNewData);
}

// Tests the loader when the last script data block is different.
TEST_F(ServiceWorkerUpdatedScriptLoaderTest, LastBlockDifferent) {
  const std::string kNewHeaders =
      "HTTP/1.0 200 OK\0Content-Type: text/javascript\0Content-Length: 21\0\0";
  const std::string kSameBlock = "old-block";
  const std::string kDiffBlock = "|diff-block|";
  const std::string kNewData = kSameBlock + kDiffBlock;

  SetUpComparedScriptInfo(
      kSameBlock.length(), kNewHeaders, kDiffBlock,
      ServiceWorkerUpdatedScriptLoader::LoaderState::kLoadingBody,
      ServiceWorkerUpdatedScriptLoader::WriterState::kWriting);

  DoRequest(kScriptURL, &client_, &loader_);
  network_producer_.reset();

  // Notify the completion of network loader.
  NotifyLoaderCompletion(net::OK);
  client_->RunUntilComplete();

  EXPECT_EQ(net::OK, client_->completion_status().error_code);

  // The client should have received the response.
  CheckReceivedResponse(kNewData);
}

// Tests the loader when the last script data block is different and
// OnCompleted() has been called during update check.
TEST_F(ServiceWorkerUpdatedScriptLoaderTest, LastBlockDifferentCompleted) {
  const std::string kNewHeaders =
      "HTTP/1.0 200 OK\0Content-Type: text/javascript\0Content-Length: 21\0\0";
  const std::string kSameBlock = "old-block";
  const std::string kDiffBlock = "|diff-block|";
  const std::string kNewData = kSameBlock + kDiffBlock;

  SetUpComparedScriptInfo(
      kSameBlock.length(), kNewHeaders, kDiffBlock,
      ServiceWorkerUpdatedScriptLoader::LoaderState::kCompleted,
      ServiceWorkerUpdatedScriptLoader::WriterState::kWriting);

  DoRequest(kScriptURL, &client_, &loader_);
  network_producer_.reset();
  client_->RunUntilComplete();

  EXPECT_EQ(net::OK, client_->completion_status().error_code);

  // The client should have received the response.
  CheckReceivedResponse(kNewData);
}

// Tests the loader when the new script has more data appended.
TEST_F(ServiceWorkerUpdatedScriptLoaderTest, NewScriptLargerThanOld) {
  const std::string kNewHeaders =
      "HTTP/1.0 200 OK\0Content-Type: text/javascript\0Content-Length: 39\0\0";
  const std::string kSameBlock = kOldData;
  const std::string kDiffBlock = "|diff-block|";
  const std::string kNetworkBlock = "network-block";
  const std::string kNewData = kSameBlock + kDiffBlock + kNetworkBlock;

  SetUpComparedScriptInfo(
      kSameBlock.length(), kNewHeaders, kDiffBlock,
      ServiceWorkerUpdatedScriptLoader::LoaderState::kLoadingBody,
      ServiceWorkerUpdatedScriptLoader::WriterState::kWriting);

  DoRequest(kScriptURL, &client_, &loader_);

  // Send network data.
  ASSERT_TRUE(mojo::BlockingCopyFromString(kNetworkBlock, network_producer_));
  network_producer_.reset();

  // Notify the completion of network loader.
  NotifyLoaderCompletion(net::OK);
  client_->RunUntilComplete();

  EXPECT_EQ(net::OK, client_->completion_status().error_code);

  // The client should have received the response.
  CheckReceivedResponse(kNewData);
}

// Tests the loader when the script changed to have no body.
TEST_F(ServiceWorkerUpdatedScriptLoaderTest, NewScriptEmptyBody) {
  const std::string kNewHeaders =
      "HTTP/1.0 200 OK\0Content-Type: text/javascript\0Content-Length: 0\0\0";
  const std::string kNewData = "";

  SetUpComparedScriptInfo(
      0, kNewHeaders, kNewData,
      ServiceWorkerUpdatedScriptLoader::LoaderState::kCompleted,
      ServiceWorkerUpdatedScriptLoader::WriterState::kCompleted);

  DoRequest(kScriptURL, &client_, &loader_);

  network_producer_.reset();
  client_->RunUntilComplete();

  EXPECT_EQ(net::OK, client_->completion_status().error_code);

  CheckReceivedResponse(kNewData);
}

// Tests the loader could report error when the resumed network
// download completed with error.
TEST_F(ServiceWorkerUpdatedScriptLoaderTest, CompleteFailed) {
  const std::string kNewHeaders =
      "HTTP/1.0 200 OK\0Content-Type: text/javascript\0Content-Length: 34\0\0";
  const std::string kSameBlock = "old-block";
  const std::string kDiffBlock = "|diff-block|";
  const std::string kNetworkBlock = "network-block";
  const std::string kNewData = kSameBlock + kDiffBlock + kNetworkBlock;

  SetUpComparedScriptInfo(
      kSameBlock.length(), kNewHeaders, kDiffBlock,
      ServiceWorkerUpdatedScriptLoader::LoaderState::kLoadingBody,
      ServiceWorkerUpdatedScriptLoader::WriterState::kWriting);

  DoRequest(kScriptURL, &client_, &loader_);
  network_producer_.reset();

  // Notify the failed completion of network loader.
  NotifyLoaderCompletion(net::ERR_FAILED);
  client_->RunUntilComplete();

  EXPECT_EQ(net::ERR_FAILED, client_->completion_status().error_code);
  EXPECT_EQ(ServiceWorkerConsts::kInvalidServiceWorkerResourceId,
            LookupResourceId(kScriptURL));
}

}  // namespace service_worker_updated_script_loader_unittest
}  // namespace content
