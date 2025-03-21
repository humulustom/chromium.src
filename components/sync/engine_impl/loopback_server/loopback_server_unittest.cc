// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/guid.h"
#include "components/sync/engine_impl/loopback_server/loopback_connection_manager.h"
#include "components/sync/engine_impl/syncer_proto_util.h"
#include "components/sync/protocol/sync.pb.h"
#include "components/sync/protocol/sync_enums.pb.h"
#include "components/sync/test/engine/test_directory_setter_upper.h"
#include "testing/gtest/include/gtest/gtest.h"

using sync_pb::ClientToServerMessage;
using sync_pb::ClientToServerResponse;
using sync_pb::EntitySpecifics;
using sync_pb::SyncEnums;
using sync_pb::SyncEntity;

namespace syncer {

namespace {

const char kUrl1[] = "http://www.one.com";
const char kUrl2[] = "http://www.two.com";
const char kUrl3[] = "http://www.three.com";
const char kBookmarkBar[] = "bookmark_bar";

SyncEntity NewBookmarkEntity(const std::string& url,
                             const std::string& parent_id) {
  SyncEntity entity;
  entity.mutable_specifics()->mutable_bookmark()->set_url(url);
  entity.set_parent_id_string(parent_id);
  entity.set_id_string(base::GenerateGUID());
  return entity;
}

SyncEntity UpdatedBookmarkEntity(const std::string& url,
                                 const std::string& id,
                                 const std::string& parent_id,
                                 int version) {
  SyncEntity entity;
  entity.mutable_specifics()->mutable_bookmark()->set_url(url);
  entity.set_id_string(id);
  entity.set_parent_id_string(parent_id);
  entity.set_version(version);
  return entity;
}

SyncEntity DeletedBookmarkEntity(const std::string& id, int version) {
  SyncEntity entity;
  entity.mutable_specifics()->mutable_bookmark();
  entity.set_id_string(id);
  entity.set_deleted(true);
  entity.set_version(version);
  return entity;
}

std::map<std::string, SyncEntity> ResponseToMap(
    const ClientToServerResponse& response) {
  EXPECT_TRUE(response.has_get_updates());
  std::map<std::string, SyncEntity> results;
  for (const SyncEntity& entity : response.get_updates().entries()) {
    results[entity.id_string()] = entity;
  }
  return results;
}

}  // namespace

class LoopbackServerTest : public testing::Test {
 public:
  void SetUp() override {
    base::CreateTemporaryFile(&persistent_file_);
    lcm_ = std::make_unique<LoopbackConnectionManager>(persistent_file_);
  }

  static bool CallPostAndProcessHeaders(ServerConnectionManager* scm,
                                        SyncCycle* cycle,
                                        const ClientToServerMessage& msg,
                                        ClientToServerResponse* response) {
    return SyncerProtoUtil::PostAndProcessHeaders(scm, cycle, msg, response);
  }

 protected:
  ClientToServerResponse GetUpdatesForType(int field_number) {
    ClientToServerMessage request;
    SyncerProtoUtil::SetProtocolVersion(&request);
    request.set_share("required");
    request.set_message_contents(ClientToServerMessage::GET_UPDATES);
    request.mutable_get_updates()->add_from_progress_marker()->set_data_type_id(
        field_number);

    ClientToServerResponse response;
    EXPECT_TRUE(
        CallPostAndProcessHeaders(lcm_.get(), nullptr, request, &response));
    EXPECT_EQ(SyncEnums::SUCCESS, response.error_code());
    return response;
  }

  ClientToServerMessage SingleEntryCommit(
      const std::vector<SyncEntity>& entity_vector) {
    ClientToServerMessage request;
    SyncerProtoUtil::SetProtocolVersion(&request);
    request.set_share("required");
    request.set_message_contents(ClientToServerMessage::COMMIT);
    request.set_invalidator_client_id("client_id");
    auto* commit = request.mutable_commit();
    commit->set_cache_guid("cache_guid");
    for (const SyncEntity& entity : entity_vector) {
      *commit->add_entries() = entity;
    }
    return request;
  }

  std::string CommitVerifySuccess(const SyncEntity& entity) {
    ClientToServerMessage request = SingleEntryCommit({entity});
    ClientToServerResponse response;
    EXPECT_TRUE(
        CallPostAndProcessHeaders(lcm_.get(), nullptr, request, &response));
    EXPECT_EQ(SyncEnums::SUCCESS, response.error_code());
    EXPECT_TRUE(response.has_commit());
    return response.commit().entryresponse(0).id_string();
  }

  void CommitVerifyFailure(const SyncEntity& entity) {
    ClientToServerMessage request = SingleEntryCommit({entity});
    ClientToServerResponse response;
    EXPECT_FALSE(
        CallPostAndProcessHeaders(lcm_.get(), nullptr, request, &response));
    EXPECT_NE(SyncEnums::SUCCESS, response.error_code());
    EXPECT_FALSE(response.has_commit());
  }

  base::FilePath persistent_file_;
  std::unique_ptr<LoopbackConnectionManager> lcm_;
};

TEST_F(LoopbackServerTest, WrongBirthday) {
  ClientToServerMessage msg;
  SyncerProtoUtil::SetProtocolVersion(&msg);
  msg.set_share("required");
  msg.set_store_birthday("not_your_birthday");
  msg.set_message_contents(ClientToServerMessage::GET_UPDATES);
  msg.mutable_get_updates()->add_from_progress_marker()->set_data_type_id(
      EntitySpecifics::kBookmarkFieldNumber);
  ClientToServerResponse response;

  EXPECT_TRUE(CallPostAndProcessHeaders(lcm_.get(), nullptr, msg, &response));
  EXPECT_EQ(SyncEnums::NOT_MY_BIRTHDAY, response.error_code());
}

TEST_F(LoopbackServerTest, GetUpdateCommand) {
  ClientToServerResponse response =
      GetUpdatesForType(EntitySpecifics::kBookmarkFieldNumber);
  // Expect to see the four top-level folders in this update already.
  EXPECT_EQ(4, response.get_updates().entries_size());
}

TEST_F(LoopbackServerTest, GetUpdateCommandShouldFilterByDataType) {
  ClientToServerResponse response =
      GetUpdatesForType(EntitySpecifics::kPreferenceFieldNumber);
  // Expect bookmark nodes to be ignored.
  EXPECT_EQ(0, response.get_updates().entries_size());
  EXPECT_EQ(1, response.get_updates().new_progress_marker_size());
}

TEST_F(LoopbackServerTest, ClearServerDataCommand) {
  ClientToServerMessage msg;
  SyncerProtoUtil::SetProtocolVersion(&msg);
  msg.set_share("required");
  msg.set_message_contents(ClientToServerMessage::CLEAR_SERVER_DATA);
  ClientToServerResponse response;

  EXPECT_TRUE(CallPostAndProcessHeaders(lcm_.get(), nullptr, msg, &response));
  EXPECT_EQ(SyncEnums::SUCCESS, response.error_code());
  EXPECT_TRUE(response.has_clear_server_data());
}

TEST_F(LoopbackServerTest, CommitCommand) {
  CommitVerifySuccess(NewBookmarkEntity(kUrl1, kBookmarkBar));
}

TEST_F(LoopbackServerTest, CommitFailureNoTag) {
  // Non-bookmarks and non-commit only types must have a
  // client_defined_unique_tag, which we don't set.
  SyncEntity entity;
  entity.mutable_specifics()->mutable_preference();
  CommitVerifyFailure(entity);
}

TEST_F(LoopbackServerTest, CommitBookmarkTombstoneSuccess) {
  std::string id1 = CommitVerifySuccess(NewBookmarkEntity(kUrl1, kBookmarkBar));
  std::string id2 = CommitVerifySuccess(NewBookmarkEntity(kUrl2, id1));
  std::string id3 = CommitVerifySuccess(NewBookmarkEntity(kUrl3, kBookmarkBar));

  // Because 2 is a child of 1, deleting 1 will also delete 2.
  CommitVerifySuccess(DeletedBookmarkEntity(id1, 10));

  std::map<std::string, SyncEntity> bookmarks =
      ResponseToMap(GetUpdatesForType(EntitySpecifics::kBookmarkFieldNumber));
  EXPECT_TRUE(bookmarks[id1].deleted());
  EXPECT_TRUE(bookmarks[id2].deleted());
  EXPECT_FALSE(bookmarks[id3].deleted());
}

TEST_F(LoopbackServerTest, CommitBookmarkTombstoneFailure) {
  std::string id1 = CommitVerifySuccess(NewBookmarkEntity(kUrl1, kBookmarkBar));
  std::string id2 = CommitVerifySuccess(NewBookmarkEntity(kUrl2, "9" + id1));

  // This write is going to fail, the id is supposed to encode the model type as
  // as prefix, by adding 9 we're creating a fake model type.
  SyncEntity entity = DeletedBookmarkEntity("9" + id1, 1);
  CommitVerifyFailure(entity);

  std::map<std::string, SyncEntity> bookmarks =
      ResponseToMap(GetUpdatesForType(EntitySpecifics::kBookmarkFieldNumber));
  EXPECT_FALSE(bookmarks[id1].deleted());
  // This is the point of this test, making sure the child doesn't get deleted.
  EXPECT_FALSE(bookmarks[id2].deleted());
}

TEST_F(LoopbackServerTest, LoadSavedState) {
  std::string id = CommitVerifySuccess(NewBookmarkEntity(kUrl1, kBookmarkBar));

  LoopbackConnectionManager second_user(persistent_file_);

  ClientToServerMessage get_updates_msg;
  SyncerProtoUtil::SetProtocolVersion(&get_updates_msg);
  get_updates_msg.set_share("required");
  get_updates_msg.set_message_contents(ClientToServerMessage::GET_UPDATES);
  get_updates_msg.mutable_get_updates()
      ->add_from_progress_marker()
      ->set_data_type_id(EntitySpecifics::kBookmarkFieldNumber);

  ClientToServerResponse response;
  EXPECT_TRUE(CallPostAndProcessHeaders(&second_user, nullptr, get_updates_msg,
                                        &response));
  EXPECT_EQ(SyncEnums::SUCCESS, response.error_code());
  ASSERT_TRUE(response.has_get_updates());
  // Expect to see the four top-level folders and the newly added bookmark!
  EXPECT_EQ(5, response.get_updates().entries_size());
  EXPECT_EQ(1U, ResponseToMap(response).count(id));

  ClientToServerResponse response2;
  EXPECT_TRUE(CallPostAndProcessHeaders(lcm_.get(), nullptr, get_updates_msg,
                                        &response2));
  EXPECT_EQ(SyncEnums::SUCCESS, response2.error_code());
  ASSERT_TRUE(response2.has_get_updates());
  ASSERT_TRUE(response2.has_store_birthday());
  EXPECT_EQ(response2.store_birthday(), response.store_birthday());
}

TEST_F(LoopbackServerTest, CommitCommandUpdate) {
  std::string id = CommitVerifySuccess(NewBookmarkEntity(kUrl1, kBookmarkBar));
  EXPECT_EQ(1U, ResponseToMap(
                    GetUpdatesForType(EntitySpecifics::kBookmarkFieldNumber))
                    .count(id));
  CommitVerifySuccess(UpdatedBookmarkEntity(kUrl2, id, "other_bookmarks", 1));

  ClientToServerResponse response =
      GetUpdatesForType(EntitySpecifics::kBookmarkFieldNumber);
  ASSERT_TRUE(response.has_get_updates());
  // Expect to see no sixth bookmark!
  EXPECT_EQ(5, response.get_updates().entries_size());
  EXPECT_EQ(kUrl2, ResponseToMap(response)[id].specifics().bookmark().url());
}

}  // namespace syncer
