// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sharing/sharing_device_source_sync.h"

#include <memory>

#include "base/callback.h"
#include "base/guid.h"
#include "base/run_loop.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "chrome/browser/sharing/fake_device_info.h"
#include "chrome/browser/sharing/features.h"
#include "chrome/browser/sharing/sharing_sync_preference.h"
#include "chrome/browser/sharing/sharing_utils.h"
#include "components/send_tab_to_self/features.h"
#include "components/send_tab_to_self/target_device_info.h"
#include "components/sync/driver/test_sync_service.h"
#include "components/sync_device_info/device_info.h"
#include "components/sync_device_info/fake_device_info_sync_service.h"
#include "components/sync_device_info/fake_device_info_tracker.h"
#include "components/sync_device_info/fake_local_device_info_provider.h"
#include "components/sync_device_info/local_device_info_util.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

const char kVapidFCMToken[] = "test_fcm_token";
const char kSenderIdFCMToken[] = "sharing_fcm_token";
const char kDevicep256dh[] = "test_p256_dh";
const char kSenderIdP256dh[] = "sharing_p256dh";
const char kDeviceAuthSecret[] = "test_auth_secret";
const char kSenderIdAuthSecret[] = "sharing_auth_secret";

std::unique_ptr<syncer::DeviceInfo> CreateDeviceInfo(
    const std::string& client_name,
    const base::SysInfo::HardwareInfo& hardware_info,
    syncer::DeviceInfo::SharingTargetInfo vapid_target_info,
    syncer::DeviceInfo::SharingTargetInfo sender_id_target_info,
    std::set<sync_pb::SharingSpecificFields::EnabledFeatures>
        enabled_features) {
  syncer::DeviceInfo::SharingInfo sharing_info(std::move(vapid_target_info),
                                               std::move(sender_id_target_info),
                                               std::move(enabled_features));

  return CreateFakeDeviceInfo(
      base::GenerateGUID(), client_name, std::move(sharing_info),
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, hardware_info);
}

std::unique_ptr<syncer::DeviceInfo> CreateDeviceInfo(
    const std::string& client_name,
    const base::SysInfo::HardwareInfo& hardware_info,
    sync_pb::SharingSpecificFields::EnabledFeatures enabled_feature) {
  return CreateDeviceInfo(
      client_name, hardware_info,
      {kVapidFCMToken, kDevicep256dh, kDeviceAuthSecret},
      {kSenderIdFCMToken, kSenderIdP256dh, kSenderIdAuthSecret},
      {enabled_feature});
}

class SharingDeviceSourceSyncTest : public testing::Test {
 public:
  SharingDeviceSourceSyncTest()
      : sharing_sync_preference_(&prefs_, &fake_device_info_sync_service_) {
    SharingSyncPreference::RegisterProfilePrefs(prefs_.registry());
  }

  std::unique_ptr<SharingDeviceSourceSync> CreateDeviceSource(
      bool wait_until_ready) {
    auto device_source = std::make_unique<SharingDeviceSourceSync>(
        &test_sync_service_, &fake_local_device_info_provider_,
        &fake_device_info_tracker_, &sharing_sync_preference_);
    if (!wait_until_ready)
      return device_source;

    if (!fake_device_info_tracker_.IsSyncing())
      fake_device_info_tracker_.Add(local_device_info_);
    fake_local_device_info_provider_.SetReady(true);

    // Wait until local personalizable device
    base::RunLoop run_loop;
    device_source->AddReadyCallback(run_loop.QuitClosure());
    run_loop.Run();

    return device_source;
  }

 protected:
  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::test::ScopedFeatureList scoped_feature_list_;

  syncer::TestSyncService test_sync_service_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
  syncer::FakeDeviceInfoSyncService fake_device_info_sync_service_;
  SharingSyncPreference sharing_sync_preference_;
  syncer::FakeLocalDeviceInfoProvider fake_local_device_info_provider_;
  syncer::FakeDeviceInfoTracker fake_device_info_tracker_;
  const syncer::DeviceInfo* local_device_info_ =
      fake_local_device_info_provider_.GetLocalDeviceInfo();
};

}  // namespace

TEST_F(SharingDeviceSourceSyncTest, RunsReadyCallback) {
  fake_local_device_info_provider_.SetReady(false);
  EXPECT_FALSE(fake_device_info_tracker_.IsSyncing());
  EXPECT_FALSE(fake_local_device_info_provider_.GetLocalDeviceInfo());

  auto device_source = CreateDeviceSource(/*wait_until_ready=*/false);

  base::RunLoop run_loop;
  bool did_run_callback = false;
  device_source->AddReadyCallback(
      base::BindLambdaForTesting([&did_run_callback, &run_loop]() {
        did_run_callback = true;
        run_loop.Quit();
      }));
  EXPECT_FALSE(did_run_callback);

  // Make DeviceInfoTracker ready.
  fake_device_info_tracker_.Add(local_device_info_);
  EXPECT_FALSE(did_run_callback);

  // Set LocalDeviceInfoProvider ready.
  fake_local_device_info_provider_.SetReady(true);
  EXPECT_FALSE(did_run_callback);

  // Wait until local device name is ready.
  run_loop.Run();
  EXPECT_TRUE(did_run_callback);
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceByGuid_Ready) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  EXPECT_TRUE(device_source->GetDeviceByGuid(local_device_info_->guid()));
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceByGuid_NotReady) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/false);
  fake_device_info_tracker_.Add(local_device_info_);
  // Even if local device is not ready we should be able to query devices.
  EXPECT_TRUE(device_source->GetDeviceByGuid(local_device_info_->guid()));
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceByGuid_UnknownGuid) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  EXPECT_FALSE(device_source->GetDeviceByGuid("unknown"));
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceByGuid_SyncDisabled) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  test_sync_service_.SetTransportState(
      syncer::SyncService::TransportState::DISABLED);
  EXPECT_FALSE(device_source->GetDeviceByGuid(local_device_info_->guid()));
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_Ready) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  auto device_info = CreateDeviceInfo(
      "client_name", {}, sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  ASSERT_EQ(1u, devices.size());
  EXPECT_EQ(device_info->guid(), devices[0]->guid());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_NotReady) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/false);
  auto device_info = CreateDeviceInfo(
      "client_name", {}, sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info.get());
  // Local device needs to be ready for deduplication.
  EXPECT_TRUE(
      device_source
          ->GetDeviceCandidates(sync_pb::SharingSpecificFields::CLICK_TO_CALL)
          .empty());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_Deduplicated) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);

  // Add two devices with the same |client_name| without hardware info.
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_1 = CreateDeviceInfo(
      "client_name_1", {}, sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_1.get());
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_2 = CreateDeviceInfo(
      "client_name_1", {}, sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_2.get());

  // Add two devices with the same hardware info.
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_3 =
      CreateDeviceInfo("model 1", {"manufacturer 1", "model 1"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_3.get());
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_4 =
      CreateDeviceInfo("model 1", {"manufacturer 1", "model 1"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_4.get());

  // Add a device with the same info as the local device.
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_5 = CreateDeviceInfo(
      local_device_info_->client_name(), local_device_info_->hardware_info(),
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_5.get());

  // Add a device with the local personalizable device name as client_name to
  // simulate old versions without hardware info.
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_6 =
      CreateDeviceInfo(syncer::GetPersonalizableDeviceNameBlocking(), {},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_6.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  ASSERT_EQ(2u, devices.size());
  EXPECT_EQ(device_info_4->guid(), devices[0]->guid());
  EXPECT_EQ(device_info_2->guid(), devices[1]->guid());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_DeviceNaming) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_1 = CreateDeviceInfo(
      "client_name", {}, sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_1.get());

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_2 =
      CreateDeviceInfo("model 1", {"manufacturer 1", "model 1"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_2.get());

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_3 =
      CreateDeviceInfo("model 2", {"manufacturer 1", "model 2"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_3.get());

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_4 =
      CreateDeviceInfo("model 1", {"manufacturer 2", "model 1"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_4.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  ASSERT_EQ(4u, devices.size());
  EXPECT_EQ(
      send_tab_to_self::GetSharingDeviceNames(device_info_4.get()).short_name,
      devices[0]->client_name());
  EXPECT_EQ(
      send_tab_to_self::GetSharingDeviceNames(device_info_3.get()).full_name,
      devices[1]->client_name());
  EXPECT_EQ(
      send_tab_to_self::GetSharingDeviceNames(device_info_2.get()).full_name,
      devices[2]->client_name());
  EXPECT_EQ(
      send_tab_to_self::GetSharingDeviceNames(device_info_1.get()).short_name,
      devices[3]->client_name());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_Expired) {
  // Create device in advance so we can forward time before calling
  // GetDeviceCandidates.
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  auto device_info =
      CreateDeviceInfo("model 1", {"manufacturer 2", "model 1"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info.get());

  // Forward time until device expires.
  task_environment_.FastForwardBy(
      base::TimeDelta::FromHours(kSharingDeviceExpirationHours.Get()) +
      base::TimeDelta::FromMilliseconds(1));

  std::vector<std::unique_ptr<syncer::DeviceInfo>> candidates =
      device_source->GetDeviceCandidates(
          sync_pb::SharingSpecificFields::CLICK_TO_CALL);

  EXPECT_TRUE(candidates.empty());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_MissingRequirements) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  // Create device in with Click to call feature.
  auto device_info =
      CreateDeviceInfo("model 1", {"manufacturer 2", "model 1"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info.get());

  // Requires shared clipboard feature.
  std::vector<std::unique_ptr<syncer::DeviceInfo>> candidates =
      device_source->GetDeviceCandidates(
          sync_pb::SharingSpecificFields::SHARED_CLIPBOARD);

  EXPECT_TRUE(candidates.empty());
}

TEST_F(SharingDeviceSourceSyncTest,
       GetDeviceCandidates_AlternativeRequirement) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  auto device_info = CreateDeviceInfo(
      "client_name", {}, sync_pb::SharingSpecificFields::CLICK_TO_CALL_VAPID);
  fake_device_info_tracker_.Add(device_info.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  ASSERT_EQ(1u, devices.size());
  EXPECT_EQ(device_info->guid(), devices[0]->guid());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_RenameAfterFiltering) {
  scoped_feature_list_.InitAndEnableFeature(
      send_tab_to_self::kSharingRenameDevices);
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);

  // This device will be filtered out because its older than |min_updated_time|.
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_1 =
      CreateDeviceInfo("model 3", {"manufacturer 2", "model 3"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_1.get());

  // This device will be displayed with its short name.
  task_environment_.FastForwardBy(
      base::TimeDelta::FromHours(kSharingDeviceExpirationHours.Get()));
  auto device_info_2 =
      CreateDeviceInfo("model 1", {"manufacturer 1", "model 1"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_2.get());

  // This device will be filtered out since click to call is not enabled.
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_3 =
      CreateDeviceInfo("model 1", {"manufacturer 1", "model 1"},
                       sync_pb::SharingSpecificFields::SHARED_CLIPBOARD);
  fake_device_info_tracker_.Add(device_info_3.get());

  // This device will be displayed with its short name.
  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(10));
  auto device_info_4 =
      CreateDeviceInfo("model 2", {"manufacturer 2", "model 2"},
                       sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  fake_device_info_tracker_.Add(device_info_4.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  ASSERT_EQ(2u, devices.size());
  EXPECT_EQ(device_info_4->guid(), devices[0]->guid());
  EXPECT_EQ(
      send_tab_to_self::GetSharingDeviceNames(device_info_4.get()).short_name,
      devices[0]->client_name());
  EXPECT_EQ(device_info_2->guid(), devices[1]->guid());
  EXPECT_EQ(
      send_tab_to_self::GetSharingDeviceNames(device_info_2.get()).short_name,
      devices[1]->client_name());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_NoChannel) {
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  auto device_info =
      CreateDeviceInfo("client_name", /*hardware_info=*/{},
                       /*vapid_target_info=*/{}, /*sender_id_target_info=*/{},
                       {sync_pb::SharingSpecificFields::CLICK_TO_CALL});
  fake_device_info_tracker_.Add(device_info.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  EXPECT_TRUE(devices.empty());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_FCMChannel) {
  scoped_feature_list_.InitAndDisableFeature(kSharingSendViaSync);
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  auto device_info =
      CreateDeviceInfo("client_name", /*hardware_info=*/{},
                       {kVapidFCMToken, kDevicep256dh, kDeviceAuthSecret},
                       /*sender_id_target_info=*/{},
                       {sync_pb::SharingSpecificFields::CLICK_TO_CALL});
  fake_device_info_tracker_.Add(device_info.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  ASSERT_EQ(1u, devices.size());
  EXPECT_EQ(device_info->guid(), devices[0]->guid());
}

TEST_F(SharingDeviceSourceSyncTest, GetDeviceCandidates_SenderIDChannel) {
  scoped_feature_list_.InitWithFeatures(
      /*enabled_features=*/{kSharingSendViaSync, kSharingUseDeviceInfo},
      /*disabled_features=*/{});
  test_sync_service_.SetActiveDataTypes(
      {syncer::DEVICE_INFO, syncer::SHARING_MESSAGE});
  auto device_source = CreateDeviceSource(/*wait_until_ready=*/true);
  auto device_info = CreateDeviceInfo(
      "client_name", /*hardware_info=*/{},
      /*vapid_target_info=*/{},
      {kSenderIdFCMToken, kSenderIdP256dh, kSenderIdAuthSecret},
      {sync_pb::SharingSpecificFields::CLICK_TO_CALL});
  fake_device_info_tracker_.Add(device_info.get());

  auto devices = device_source->GetDeviceCandidates(
      sync_pb::SharingSpecificFields::CLICK_TO_CALL);
  ASSERT_EQ(1u, devices.size());
  EXPECT_EQ(device_info->guid(), devices[0]->guid());
}
