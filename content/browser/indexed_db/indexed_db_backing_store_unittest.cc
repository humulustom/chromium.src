// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/indexed_db/indexed_db_backing_store.h"

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include "base/barrier_closure.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/guid.h"
#include "base/logging.h"
#include "base/sequenced_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/test/bind_test_util.h"
#include "base/time/default_clock.h"
#include "components/services/storage/indexed_db/scopes/disjoint_range_lock_manager.h"
#include "components/services/storage/indexed_db/scopes/varint_coding.h"
#include "components/services/storage/indexed_db/transactional_leveldb/leveldb_write_batch.h"
#include "components/services/storage/indexed_db/transactional_leveldb/transactional_leveldb_database.h"
#include "components/services/storage/public/mojom/indexed_db_control.mojom-test-utils.h"
#include "content/browser/indexed_db/indexed_db_class_factory.h"
#include "content/browser/indexed_db/indexed_db_context_impl.h"
#include "content/browser/indexed_db/indexed_db_factory_impl.h"
#include "content/browser/indexed_db/indexed_db_leveldb_coding.h"
#include "content/browser/indexed_db/indexed_db_leveldb_operations.h"
#include "content/browser/indexed_db/indexed_db_metadata_coding.h"
#include "content/browser/indexed_db/indexed_db_origin_state.h"
#include "content/browser/indexed_db/indexed_db_value.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_utils.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "storage/browser/quota/special_storage_policy.h"
#include "storage/browser/test/fake_blob.h"
#include "storage/browser/test/mock_quota_manager_proxy.h"
#include "storage/browser/test/mock_special_storage_policy.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/indexeddb/web_idb_types.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom.h"

using base::ASCIIToUTF16;
using blink::IndexedDBDatabaseMetadata;
using blink::IndexedDBIndexMetadata;
using blink::IndexedDBKey;
using blink::IndexedDBKeyPath;
using blink::IndexedDBKeyRange;
using blink::IndexedDBObjectStoreMetadata;
using url::Origin;

namespace content {
namespace indexed_db_backing_store_unittest {

// Write |content| to |file|. Returns true on success.
bool WriteFile(const base::FilePath& file, base::StringPiece content) {
  int write_size = base::WriteFile(file, content.data(), content.length());
  return write_size >= 0 && write_size == static_cast<int>(content.length());
}

class TestableIndexedDBBackingStore : public IndexedDBBackingStore {
 public:
  TestableIndexedDBBackingStore(
      IndexedDBBackingStore::Mode backing_store_mode,
      TransactionalLevelDBFactory* leveldb_factory,
      const url::Origin& origin,
      const base::FilePath& blob_path,
      std::unique_ptr<TransactionalLevelDBDatabase> db,
      storage::mojom::BlobStorageContext* blob_storage_context,
      BlobFilesCleanedCallback blob_files_cleaned,
      ReportOutstandingBlobsCallback report_outstanding_blobs,
      scoped_refptr<base::SequencedTaskRunner> idb_task_runner,
      scoped_refptr<base::SequencedTaskRunner> io_task_runner)
      : IndexedDBBackingStore(backing_store_mode,
                              leveldb_factory,
                              origin,
                              blob_path,
                              std::move(db),
                              blob_storage_context,
                              std::move(blob_files_cleaned),
                              std::move(report_outstanding_blobs),
                              std::move(idb_task_runner),
                              std::move(io_task_runner)) {}
  ~TestableIndexedDBBackingStore() override = default;

  const std::vector<base::FilePath>& removals() const { return removals_; }
  void ClearRemovals() { removals_.clear(); }

  void StartJournalCleaningTimer() override {
    IndexedDBBackingStore::StartJournalCleaningTimer();
  }

 protected:
  bool RemoveBlobFile(int64_t database_id, int64_t blob_number) const override {
    removals_.push_back(GetBlobFileName(database_id, blob_number));
    return IndexedDBBackingStore::RemoveBlobFile(database_id, blob_number);
  }

 private:
  // This is modified in an overridden virtual function that is properly const
  // in the real implementation, therefore must be mutable here.
  mutable std::vector<base::FilePath> removals_;

  DISALLOW_COPY_AND_ASSIGN(TestableIndexedDBBackingStore);
};

// Factory subclass to allow the test to use the
// TestableIndexedDBBackingStore subclass.
class TestIDBFactory : public IndexedDBFactoryImpl {
 public:
  explicit TestIDBFactory(
      IndexedDBContextImpl* idb_context,
      storage::mojom::BlobStorageContext* blob_storage_context)
      : IndexedDBFactoryImpl(idb_context,
                             IndexedDBClassFactory::Get(),
                             base::DefaultClock::GetInstance(),
                             blob_storage_context) {}
  ~TestIDBFactory() override = default;

 protected:
  std::unique_ptr<IndexedDBBackingStore> CreateBackingStore(
      IndexedDBBackingStore::Mode backing_store_mode,
      TransactionalLevelDBFactory* leveldb_factory,
      const url::Origin& origin,
      const base::FilePath& blob_path,
      std::unique_ptr<TransactionalLevelDBDatabase> db,
      storage::mojom::BlobStorageContext* blob_storage_context,
      IndexedDBBackingStore::BlobFilesCleanedCallback blob_files_cleaned,
      IndexedDBBackingStore::ReportOutstandingBlobsCallback
          report_outstanding_blobs,
      scoped_refptr<base::SequencedTaskRunner> idb_task_runner,
      scoped_refptr<base::SequencedTaskRunner> io_task_runner) override {
    return std::make_unique<TestableIndexedDBBackingStore>(
        backing_store_mode, leveldb_factory, origin, blob_path, std::move(db),
        blob_storage_context, std::move(blob_files_cleaned),
        std::move(report_outstanding_blobs), std::move(idb_task_runner),
        std::move(io_task_runner));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestIDBFactory);
};

struct BlobWrite {
  BlobWrite() = default;
  BlobWrite(BlobWrite&& other) {
    blob = std::move(other.blob);
    path = std::move(other.path);
  }
  BlobWrite(mojo::PendingRemote<::blink::mojom::Blob> blob, base::FilePath path)
      : blob(std::move(blob)), path(path) {}
  ~BlobWrite() = default;

  int64_t GetBlobNumber() const {
    int64_t result;
    EXPECT_TRUE(base::StringToInt64(path.BaseName().AsUTF8Unsafe(), &result));
    return result;
  }

  mojo::Remote<::blink::mojom::Blob> blob;
  base::FilePath path;
};

class MockBlobStorageContext : public ::storage::mojom::BlobStorageContext {
 public:
  ~MockBlobStorageContext() override = default;

  void RegisterFromDataItem(mojo::PendingReceiver<::blink::mojom::Blob> blob,
                            const std::string& uuid,
                            storage::mojom::BlobDataItemPtr item) override {
    NOTREACHED();
  }
  void RegisterFromMemory(mojo::PendingReceiver<::blink::mojom::Blob> blob,
                          const std::string& uuid,
                          ::mojo_base::BigBuffer data) override {
    NOTREACHED();
  }
  void WriteBlobToFile(mojo::PendingRemote<::blink::mojom::Blob> blob,
                       const base::FilePath& path,
                       bool flush_on_write,
                       base::Optional<base::Time> last_modified,
                       WriteBlobToFileCallback callback) override {
    writes_.emplace_back(std::move(blob), path);
    base::SequencedTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback),
                       storage::mojom::WriteBlobToFileResult::kSuccess));
  }

  const std::vector<BlobWrite>& writes() { return writes_; }
  void ClearWrites() { writes_.clear(); }

 private:
  std::vector<BlobWrite> writes_;
};

class IndexedDBBackingStoreTest : public testing::Test {
 public:
  IndexedDBBackingStoreTest()
      : special_storage_policy_(
            base::MakeRefCounted<MockSpecialStoragePolicy>()),
        quota_manager_proxy_(
            base::MakeRefCounted<MockQuotaManagerProxy>(nullptr, nullptr)) {}

  void SetUp() override {
    special_storage_policy_->SetAllUnlimited(true);
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());

    blob_context_ = std::make_unique<MockBlobStorageContext>();

    idb_context_ = base::MakeRefCounted<IndexedDBContextImpl>(
        temp_dir_.GetPath(), special_storage_policy_, quota_manager_proxy_,
        base::DefaultClock::GetInstance(),
        mojo::PendingRemote<storage::mojom::BlobStorageContext>(),
        base::SequencedTaskRunnerHandle::Get(),
        base::SequencedTaskRunnerHandle::Get());

    CreateFactoryAndBackingStore();

    // useful keys and values during tests
    value1_ = IndexedDBValue("value1", {});
    value2_ = IndexedDBValue("value2", {});

    key1_ = IndexedDBKey(99, blink::mojom::IDBKeyType::Number);
    key2_ = IndexedDBKey(ASCIIToUTF16("key2"));
  }

  void CreateFactoryAndBackingStore() {
    const Origin origin = Origin::Create(GURL("http://localhost:81"));
    idb_factory_ = std::make_unique<TestIDBFactory>(idb_context_.get(),
                                                    blob_context_.get());

    leveldb::Status s;
    std::tie(origin_state_handle_, s, std::ignore, data_loss_info_,
             std::ignore) =
        idb_factory_->GetOrOpenOriginFactory(origin, idb_context_->data_path(),
                                             /*create_if_missing=*/true);
    if (!origin_state_handle_.IsHeld()) {
      backing_store_ = nullptr;
      return;
    }
    backing_store_ = static_cast<TestableIndexedDBBackingStore*>(
        origin_state_handle_.origin_state()->backing_store());
    lock_manager_ = origin_state_handle_.origin_state()->lock_manager();
  }

  std::vector<ScopeLock> CreateDummyLock() {
    base::RunLoop loop;
    ScopesLocksHolder locks_receiver;
    bool success = lock_manager_->AcquireLocks(
        {{0, {"01", "11"}, ScopesLockManager::LockType::kShared}},
        locks_receiver.AsWeakPtr(),
        base::BindLambdaForTesting([&loop]() { loop.Quit(); }));
    EXPECT_TRUE(success);
    if (success)
      loop.Run();
    return std::move(locks_receiver.locks);
  }

  void DestroyFactoryAndBackingStore() {
    origin_state_handle_.Release();
    idb_factory_.reset();
    backing_store_ = nullptr;
  }

  void TearDown() override {
    DestroyFactoryAndBackingStore();
    quota_manager_proxy_->SimulateQuotaManagerDestroyed();

    if (idb_context_ && !idb_context_->IsInMemoryContext()) {
      IndexedDBFactoryImpl* factory = idb_context_->GetIDBFactory();

      // Loop through all open origins, and force close them, and request the
      // deletion of the leveldb state. Once the states are no longer around,
      // delete all of the databases on disk.
      auto open_factory_origins = factory->GetOpenOrigins();

      for (auto origin : open_factory_origins) {
        base::RunLoop loop;
        IndexedDBOriginState* per_origin_factory =
            factory->GetOriginFactory(origin);
        per_origin_factory->backing_store()
            ->db()
            ->leveldb_state()
            ->RequestDestruction(loop.QuitClosure(),
                                 base::SequencedTaskRunnerHandle::Get());
        idb_context_->ForceCloseSync(
            origin,
            storage::mojom::ForceCloseReason::FORCE_CLOSE_DELETE_ORIGIN);
        loop.Run();
      }
      // All leveldb databases are closed, and they can be deleted.
      for (auto origin : idb_context_->GetAllOrigins()) {
        bool success = false;
        storage::mojom::IndexedDBControlAsyncWaiter waiter(idb_context_.get());
        waiter.DeleteForOrigin(origin, &success);
        EXPECT_TRUE(success);
      }
    }
    if (temp_dir_.IsValid())
      ASSERT_TRUE(temp_dir_.Delete());

    // Wait until the context has fully destroyed.
    scoped_refptr<base::SequencedTaskRunner> task_runner =
        idb_context_->IDBTaskRunner();
    idb_context_.reset();
    {
      base::RunLoop loop;
      task_runner->PostTask(FROM_HERE, loop.QuitClosure());
      loop.Run();
    }
  }

  TestableIndexedDBBackingStore* backing_store() { return backing_store_; }

  // Cycle the idb runner to help clean up tasks, which allows for a clean
  // shutdown of the leveldb database. This ensures that all file handles are
  // released and the folder can be deleted on windows (which doesn't allow
  // folders to be deleted when inside files are in use/exist).
  void CycleIDBTaskRunner() {
    base::RunLoop cycle_loop;
    idb_context_->IDBTaskRunner()->PostTask(FROM_HERE,
                                            cycle_loop.QuitClosure());
    cycle_loop.Run();
  }

 protected:
  BrowserTaskEnvironment task_environment_;

  base::ScopedTempDir temp_dir_;
  std::unique_ptr<MockBlobStorageContext> blob_context_;
  scoped_refptr<MockSpecialStoragePolicy> special_storage_policy_;
  scoped_refptr<MockQuotaManagerProxy> quota_manager_proxy_;
  scoped_refptr<IndexedDBContextImpl> idb_context_;
  std::unique_ptr<TestIDBFactory> idb_factory_;
  DisjointRangeLockManager* lock_manager_;

  IndexedDBOriginStateHandle origin_state_handle_;
  TestableIndexedDBBackingStore* backing_store_ = nullptr;
  IndexedDBDataLossInfo data_loss_info_;

  // Sample keys and values that are consistent.
  IndexedDBKey key1_;
  IndexedDBKey key2_;
  IndexedDBValue value1_;
  IndexedDBValue value2_;

 private:
  DISALLOW_COPY_AND_ASSIGN(IndexedDBBackingStoreTest);
};

const static std::string kBlobFileData1 = "asdfgasdf";
const static std::string kBlobFileData2 = "aaaaaa";

class IndexedDBBackingStoreTestWithBlobs : public IndexedDBBackingStoreTest {
 public:
  IndexedDBBackingStoreTestWithBlobs() = default;

  void SetUp() override {
    IndexedDBBackingStoreTest::SetUp();

    const int64_t kTime1 = 13255919133000000ll;
    const int64_t kTime2 = 13287455133000000ll;
    // useful keys and values during tests
    external_objects_.push_back(
        CreateBlobInfo(base::UTF8ToUTF16("blob type"), 1));
    external_objects_.push_back(CreateBlobInfo(
        base::UTF8ToUTF16("file name"), base::UTF8ToUTF16("file type"),
        base::Time::FromDeltaSinceWindowsEpoch(
            base::TimeDelta::FromMicroseconds(kTime1)),
        kBlobFileData1.size()));
    external_objects_.push_back(CreateBlobInfo(
        base::UTF8ToUTF16("file name"), base::UTF8ToUTF16("file type"),
        base::Time::FromDeltaSinceWindowsEpoch(
            base::TimeDelta::FromMicroseconds(kTime2)),
        kBlobFileData2.size()));
    value3_ = IndexedDBValue("value3", external_objects_);
    key3_ = IndexedDBKey(ASCIIToUTF16("key3"));
  }

  IndexedDBExternalObject CreateBlobInfo(const base::string16& file_name,
                                         const base::string16& type,
                                         base::Time last_modified,
                                         int64_t size) {
    auto uuid = base::GenerateGUID();
    mojo::PendingRemote<blink::mojom::Blob> remote;
    base::CreateSequencedTaskRunner({base::ThreadPool()})
        ->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](std::string uuid,
                   mojo::PendingReceiver<blink::mojom::Blob> pending_receiver) {
                  mojo::MakeSelfOwnedReceiver(
                      std::make_unique<storage::FakeBlob>(uuid),
                      std::move(pending_receiver));
                },
                uuid, remote.InitWithNewPipeAndPassReceiver()));
    IndexedDBExternalObject info(std::move(remote), uuid, file_name, type,
                                 last_modified, size);
    return info;
  }

  IndexedDBExternalObject CreateBlobInfo(const base::string16& type,
                                         int64_t size) {
    auto uuid = base::GenerateGUID();
    mojo::PendingRemote<blink::mojom::Blob> remote;
    base::CreateSequencedTaskRunner({base::ThreadPool()})
        ->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](std::string uuid,
                   mojo::PendingReceiver<blink::mojom::Blob> pending_receiver) {
                  mojo::MakeSelfOwnedReceiver(
                      std::make_unique<storage::FakeBlob>(uuid),
                      std::move(pending_receiver));
                },
                uuid, remote.InitWithNewPipeAndPassReceiver()));
    IndexedDBExternalObject info(std::move(remote), uuid, type, size);
    return info;
  }

  // This just checks the data that survive getting stored and recalled, e.g.
  // the file path and UUID will change and thus aren't verified.
  bool CheckBlobInfoMatches(
      const std::vector<IndexedDBExternalObject>& reads) const {
    DCHECK(idb_context_->IDBTaskRunner()->RunsTasksInCurrentSequence());

    if (external_objects_.size() != reads.size()) {
      EXPECT_EQ(external_objects_.size(), reads.size());
      return false;
    }
    for (size_t i = 0; i < external_objects_.size(); ++i) {
      const IndexedDBExternalObject& a = external_objects_[i];
      const IndexedDBExternalObject& b = reads[i];
      if (a.is_file() != b.is_file()) {
        EXPECT_EQ(a.is_file(), b.is_file());
        return false;
      }
      if (a.type() != b.type()) {
        EXPECT_EQ(a.is_file(), b.is_file());
        return false;
      }
      if (a.size() != b.size()) {
        EXPECT_EQ(a.size(), b.size());
        return false;
      }
      if (a.is_file()) {
        if (a.file_name() != b.file_name()) {
          EXPECT_EQ(a.file_name(), b.file_name());
          return false;
        }
        if (a.last_modified() != b.last_modified()) {
          EXPECT_EQ(a.last_modified(), b.last_modified());
          return false;
        }
      }
    }
    return true;
  }

  bool CheckBlobReadsMatchWrites(
      const std::vector<IndexedDBExternalObject>& reads) const {
    DCHECK(idb_context_->IDBTaskRunner()->RunsTasksInCurrentSequence());

    if (blob_context_->writes().size() != reads.size())
      return false;
    std::set<base::FilePath> ids;
    for (const auto& write : blob_context_->writes())
      ids.insert(write.path);
    if (ids.size() != blob_context_->writes().size())
      return false;
    for (const auto& read : reads) {
      if (ids.count(read.indexed_db_file_path()) != 1)
        return false;
    }
    return true;
  }

  bool CheckBlobWrites() {
    DCHECK(idb_context_->IDBTaskRunner()->RunsTasksInCurrentSequence());

    if (blob_context_->writes().size() != external_objects_.size())
      return false;
    for (size_t i = 0; i < blob_context_->writes().size(); ++i) {
      const BlobWrite& desc = blob_context_->writes()[i];
      const IndexedDBExternalObject& info = external_objects_[i];
      base::RunLoop uuid_loop;
      std::string uuid_out;
      DCHECK(desc.blob.is_bound());
      DCHECK(desc.blob.is_connected());
      desc.blob->GetInternalUUID(
          base::BindLambdaForTesting([&](const std::string& uuid) {
            uuid_out = uuid;
            uuid_loop.Quit();
          }));
      uuid_loop.Run();
      if (uuid_out != info.uuid())
        return false;
    }
    return true;
  }

  bool CheckBlobRemovals() const {
    DCHECK(idb_context_->IDBTaskRunner()->RunsTasksInCurrentSequence());

    if (backing_store_->removals().size() != blob_context_->writes().size())
      return false;
    for (size_t i = 0; i < blob_context_->writes().size(); ++i) {
      if (blob_context_->writes()[i].path != backing_store_->removals()[i]) {
        return false;
      }
    }
    return true;
  }

  std::vector<IndexedDBExternalObject>& external_objects() {
    return external_objects_;
  }

  // Sample keys and values that are consistent. Public so that posted lambdas
  // passed |this| can access them.
  IndexedDBKey key3_;
  IndexedDBValue value3_;

 private:
  // Blob details referenced by |value3_|. The various CheckBlob*() methods
  // can be used to verify the state as a test progresses.
  std::vector<IndexedDBExternalObject> external_objects_;

  std::vector<std::string> blob_remote_uuids_;

  DISALLOW_COPY_AND_ASSIGN(IndexedDBBackingStoreTestWithBlobs);
};

BlobWriteCallback CreateBlobWriteCallback(
    bool* succeeded,
    base::OnceClosure on_done = base::OnceClosure()) {
  *succeeded = false;
  return base::BindOnce(
      [](bool* succeeded, base::OnceClosure on_done, BlobWriteResult result) {
        switch (result) {
          case BlobWriteResult::kFailure:
            NOTREACHED();
            break;
          case BlobWriteResult::kRunPhaseTwoAsync:
          case BlobWriteResult::kRunPhaseTwoAndReturnResult:
            *succeeded = true;
            break;
        }
        if (!on_done.is_null())
          std::move(on_done).Run();
        return leveldb::Status::OK();
      },
      succeeded, std::move(on_done));
}

TEST_F(IndexedDBBackingStoreTest, PutGetConsistency) {
  base::RunLoop loop;
  idb_context_->IDBTaskRunner()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&]() {
        const IndexedDBKey key = key1_;
        IndexedDBValue value = value1_;
        {
          IndexedDBBackingStore::Transaction transaction1(
              backing_store()->AsWeakPtr(),
              blink::mojom::IDBTransactionDurability::Relaxed,
              blink::mojom::IDBTransactionMode::ReadWrite);
          transaction1.Begin(CreateDummyLock());
          IndexedDBBackingStore::RecordIdentifier record;
          leveldb::Status s = backing_store()->PutRecord(&transaction1, 1, 1,
                                                         key, &value, &record);
          EXPECT_TRUE(s.ok());
          bool succeeded = false;
          EXPECT_TRUE(
              transaction1.CommitPhaseOne(CreateBlobWriteCallback(&succeeded))
                  .ok());
          EXPECT_TRUE(succeeded);
          EXPECT_TRUE(transaction1.CommitPhaseTwo().ok());
        }

        {
          IndexedDBBackingStore::Transaction transaction2(
              backing_store()->AsWeakPtr(),
              blink::mojom::IDBTransactionDurability::Relaxed,
              blink::mojom::IDBTransactionMode::ReadWrite);
          transaction2.Begin(CreateDummyLock());
          IndexedDBValue result_value;
          EXPECT_TRUE(backing_store()
                          ->GetRecord(&transaction2, 1, 1, key, &result_value)
                          .ok());
          bool succeeded = false;
          EXPECT_TRUE(
              transaction2.CommitPhaseOne(CreateBlobWriteCallback(&succeeded))
                  .ok());
          EXPECT_TRUE(succeeded);
          EXPECT_TRUE(transaction2.CommitPhaseTwo().ok());
          EXPECT_EQ(value.bits, result_value.bits);
        }
        loop.Quit();
      }));
  loop.Run();

  CycleIDBTaskRunner();
}

TEST_F(IndexedDBBackingStoreTestWithBlobs, PutGetConsistencyWithBlobs) {
  // Initiate transaction1 - writing blobs.
  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction1 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction1->Begin(CreateDummyLock());
  IndexedDBBackingStore::RecordIdentifier record;
  EXPECT_TRUE(
      backing_store()
          ->PutRecord(transaction1.get(), 1, 1, key3_, &value3_, &record)
          .ok());
  bool succeeded = false;
  base::RunLoop phase_one_wait;
  EXPECT_TRUE(transaction1
                  ->CommitPhaseOne(CreateBlobWriteCallback(
                      &succeeded, phase_one_wait.QuitClosure()))
                  .ok());
  EXPECT_FALSE(succeeded);
  RunAllTasksUntilIdle();
  phase_one_wait.Run();

  // Finish up transaction1, verifying blob writes.

  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(CheckBlobWrites());
  EXPECT_TRUE(transaction1->CommitPhaseTwo().ok());

  // Initiate transaction2, reading blobs.
  IndexedDBBackingStore::Transaction transaction2(
      backing_store()->AsWeakPtr(),
      blink::mojom::IDBTransactionDurability::Relaxed,
      blink::mojom::IDBTransactionMode::ReadWrite);
  transaction2.Begin(CreateDummyLock());
  IndexedDBValue result_value;
  EXPECT_TRUE(backing_store()
                  ->GetRecord(&transaction2, 1, 1, key3_, &result_value)
                  .ok());

  // Finish up transaction2, verifying blob reads.
  succeeded = false;
  EXPECT_TRUE(
      transaction2.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(transaction2.CommitPhaseTwo().ok());
  EXPECT_EQ(value3_.bits, result_value.bits);

  RunAllTasksUntilIdle();
  EXPECT_TRUE(CheckBlobInfoMatches(result_value.external_objects));
  EXPECT_TRUE(CheckBlobReadsMatchWrites(result_value.external_objects));

  // Initiate transaction3, deleting blobs.
  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction3 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction3->Begin(CreateDummyLock());
  EXPECT_TRUE(
      backing_store()
          ->DeleteRange(transaction3.get(), 1, 1, IndexedDBKeyRange(key3_))
          .ok());
  succeeded = false;
  EXPECT_TRUE(
      transaction3->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  EXPECT_TRUE(succeeded);
  RunAllTasksUntilIdle();

  EXPECT_TRUE(succeeded);

  // Finish up transaction 3, verifying blob deletes.
  EXPECT_TRUE(transaction3->CommitPhaseTwo().ok());
  EXPECT_TRUE(CheckBlobRemovals());

  // Clean up on the IDB sequence.
  transaction1.reset();
  transaction3.reset();
  RunAllTasksUntilIdle();
}

TEST_F(IndexedDBBackingStoreTestWithBlobs, DeleteRange) {
  const std::vector<IndexedDBKey> keys = {
      IndexedDBKey(ASCIIToUTF16("key0")), IndexedDBKey(ASCIIToUTF16("key1")),
      IndexedDBKey(ASCIIToUTF16("key2")), IndexedDBKey(ASCIIToUTF16("key3"))};
  const IndexedDBKeyRange ranges[] = {
      IndexedDBKeyRange(keys[1], keys[2], false, false),
      IndexedDBKeyRange(keys[1], keys[2], false, false),
      IndexedDBKeyRange(keys[0], keys[2], true, false),
      IndexedDBKeyRange(keys[1], keys[3], false, true),
      IndexedDBKeyRange(keys[0], keys[3], true, true)};

  for (size_t i = 0; i < base::size(ranges); ++i) {
    const int64_t database_id = 1;
    const int64_t object_store_id = i + 1;
    const IndexedDBKeyRange& range = ranges[i];

    std::vector<IndexedDBExternalObject> external_objects;
    for (size_t j = 0; j < 4; ++j) {
      std::string type = "type " + base::NumberToString(j);
      external_objects.push_back(CreateBlobInfo(base::UTF8ToUTF16(type), 1));
    }

    // Reset from previous iteration.
    blob_context_->ClearWrites();
    backing_store()->ClearRemovals();

    std::vector<IndexedDBValue> values = {
        IndexedDBValue("value0", {external_objects[0]}),
        IndexedDBValue("value1", {external_objects[1]}),
        IndexedDBValue("value2", {external_objects[2]}),
        IndexedDBValue("value3", {external_objects[3]}),
    };
    ASSERT_GE(keys.size(), values.size());

    // Initiate transaction1 - write records.
    std::unique_ptr<IndexedDBBackingStore::Transaction> transaction1 =
        std::make_unique<IndexedDBBackingStore::Transaction>(
            backing_store()->AsWeakPtr(),
            blink::mojom::IDBTransactionDurability::Relaxed,
            blink::mojom::IDBTransactionMode::ReadWrite);
    transaction1->Begin(CreateDummyLock());
    IndexedDBBackingStore::RecordIdentifier record;
    for (size_t i = 0; i < values.size(); ++i) {
      EXPECT_TRUE(backing_store()
                      ->PutRecord(transaction1.get(), database_id,
                                  object_store_id, keys[i], &values[i], &record)
                      .ok());
    }

    // Start committing transaction1.
    bool succeeded = false;
    EXPECT_TRUE(
        transaction1->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    RunAllTasksUntilIdle();

    // Finish committing transaction1.

    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction1->CommitPhaseTwo().ok());

    // Initiate transaction 2 - delete range.
    std::unique_ptr<IndexedDBBackingStore::Transaction> transaction2 =
        std::make_unique<IndexedDBBackingStore::Transaction>(
            backing_store()->AsWeakPtr(),
            blink::mojom::IDBTransactionDurability::Relaxed,
            blink::mojom::IDBTransactionMode::ReadWrite);
    transaction2->Begin(CreateDummyLock());
    IndexedDBValue result_value;
    EXPECT_TRUE(backing_store()
                    ->DeleteRange(transaction2.get(), database_id,
                                  object_store_id, range)
                    .ok());

    // Start committing transaction2.
    succeeded = false;
    EXPECT_TRUE(
        transaction2->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    RunAllTasksUntilIdle();

    // Finish committing transaction2.

    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction2->CommitPhaseTwo().ok());

    // Verify blob removals.
    ASSERT_EQ(2UL, backing_store()->removals().size());
    EXPECT_EQ(blob_context_->writes()[1].path, backing_store()->removals()[0]);
    EXPECT_EQ(blob_context_->writes()[2].path, backing_store()->removals()[1]);

    // Clean up on the IDB sequence.
    transaction1.reset();
    transaction2.reset();
    RunAllTasksUntilIdle();
  }
}

TEST_F(IndexedDBBackingStoreTestWithBlobs, DeleteRangeEmptyRange) {
  const std::vector<IndexedDBKey> keys = {
      IndexedDBKey(ASCIIToUTF16("key0")), IndexedDBKey(ASCIIToUTF16("key1")),
      IndexedDBKey(ASCIIToUTF16("key2")), IndexedDBKey(ASCIIToUTF16("key3")),
      IndexedDBKey(ASCIIToUTF16("key4"))};
  const IndexedDBKeyRange ranges[] = {
      IndexedDBKeyRange(keys[3], keys[4], true, false),
      IndexedDBKeyRange(keys[2], keys[1], false, false),
      IndexedDBKeyRange(keys[2], keys[1], true, true)};

  for (size_t i = 0; i < base::size(ranges); ++i) {
    const int64_t database_id = 1;
    const int64_t object_store_id = i + 1;
    const IndexedDBKeyRange& range = ranges[i];

    std::vector<IndexedDBExternalObject> external_objects;
    for (size_t j = 0; j < 4; ++j) {
      std::string type = "type " + base::NumberToString(j);
      external_objects.push_back(CreateBlobInfo(base::UTF8ToUTF16(type), 1));
    }

    // Reset from previous iteration.
    blob_context_->ClearWrites();
    backing_store()->ClearRemovals();

    std::vector<IndexedDBValue> values = {
        IndexedDBValue("value0", {external_objects[0]}),
        IndexedDBValue("value1", {external_objects[1]}),
        IndexedDBValue("value2", {external_objects[2]}),
        IndexedDBValue("value3", {external_objects[3]}),
    };
    ASSERT_GE(keys.size(), values.size());

    // Initiate transaction1 - write records.
    std::unique_ptr<IndexedDBBackingStore::Transaction> transaction1 =
        std::make_unique<IndexedDBBackingStore::Transaction>(
            backing_store()->AsWeakPtr(),
            blink::mojom::IDBTransactionDurability::Relaxed,
            blink::mojom::IDBTransactionMode::ReadWrite);
    transaction1->Begin(CreateDummyLock());

    IndexedDBBackingStore::RecordIdentifier record;
    for (size_t i = 0; i < values.size(); ++i) {
      EXPECT_TRUE(backing_store()
                      ->PutRecord(transaction1.get(), database_id,
                                  object_store_id, keys[i], &values[i], &record)
                      .ok());
    }
    // Start committing transaction1.
    bool succeeded = false;
    EXPECT_TRUE(
        transaction1->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    RunAllTasksUntilIdle();

    // Finish committing transaction1.
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction1->CommitPhaseTwo().ok());

    // Initiate transaction 2 - delete range.
    std::unique_ptr<IndexedDBBackingStore::Transaction> transaction2 =
        std::make_unique<IndexedDBBackingStore::Transaction>(
            backing_store()->AsWeakPtr(),
            blink::mojom::IDBTransactionDurability::Relaxed,
            blink::mojom::IDBTransactionMode::ReadWrite);
    transaction2->Begin(CreateDummyLock());
    IndexedDBValue result_value;
    EXPECT_TRUE(backing_store()
                    ->DeleteRange(transaction2.get(), database_id,
                                  object_store_id, range)
                    .ok());

    // Start committing transaction2.
    succeeded = false;
    EXPECT_TRUE(
        transaction2->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    RunAllTasksUntilIdle();

    // Finish committing transaction2.
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction2->CommitPhaseTwo().ok());

    // Verify blob removals.
    EXPECT_EQ(0UL, backing_store()->removals().size());

    // Clean on the IDB sequence.
    transaction1.reset();
    transaction2.reset();
    RunAllTasksUntilIdle();
  }
}

TEST_F(IndexedDBBackingStoreTestWithBlobs, BlobJournalInterleavedTransactions) {
  // Initiate transaction1.
  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction1 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction1->Begin(CreateDummyLock());
  IndexedDBBackingStore::RecordIdentifier record1;
  EXPECT_TRUE(
      backing_store()
          ->PutRecord(transaction1.get(), 1, 1, key3_, &value3_, &record1)
          .ok());
  bool succeeded = false;
  EXPECT_TRUE(
      transaction1->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  RunAllTasksUntilIdle();

  // Verify transaction1 phase one completed.

  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(CheckBlobWrites());
  EXPECT_EQ(0U, backing_store()->removals().size());

  // Initiate transaction2.
  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction2 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction2->Begin(CreateDummyLock());
  IndexedDBBackingStore::RecordIdentifier record2;
  EXPECT_TRUE(
      backing_store()
          ->PutRecord(transaction2.get(), 1, 1, key1_, &value1_, &record2)
          .ok());
  succeeded = false;
  EXPECT_TRUE(
      transaction2->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  RunAllTasksUntilIdle();

  // Verify transaction2 phase one completed.
  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(CheckBlobWrites());
  EXPECT_EQ(0U, backing_store()->removals().size());

  // Finalize both transactions.
  EXPECT_TRUE(transaction1->CommitPhaseTwo().ok());
  EXPECT_EQ(0U, backing_store()->removals().size());

  EXPECT_TRUE(transaction2->CommitPhaseTwo().ok());
  EXPECT_EQ(0U, backing_store()->removals().size());

  // Clean up on the IDB sequence.
  transaction1.reset();
  transaction2.reset();
  RunAllTasksUntilIdle();
}

TEST_F(IndexedDBBackingStoreTestWithBlobs, ActiveBlobJournal) {
  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction1 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction1->Begin(CreateDummyLock());
  IndexedDBBackingStore::RecordIdentifier record;
  EXPECT_TRUE(
      backing_store()
          ->PutRecord(transaction1.get(), 1, 1, key3_, &value3_, &record)
          .ok());
  bool succeeded = false;
  EXPECT_TRUE(
      transaction1->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());

  RunAllTasksUntilIdle();

  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(CheckBlobWrites());
  EXPECT_TRUE(transaction1->CommitPhaseTwo().ok());

  IndexedDBBackingStore::Transaction transaction2(
      backing_store()->AsWeakPtr(),
      blink::mojom::IDBTransactionDurability::Relaxed,
      blink::mojom::IDBTransactionMode::ReadWrite);
  transaction2.Begin(CreateDummyLock());
  IndexedDBValue read_result_value;
  EXPECT_TRUE(backing_store()
                  ->GetRecord(&transaction2, 1, 1, key3_, &read_result_value)
                  .ok());
  succeeded = false;

  EXPECT_TRUE(
      transaction2.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());

  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(transaction2.CommitPhaseTwo().ok());
  EXPECT_EQ(value3_.bits, read_result_value.bits);
  EXPECT_TRUE(CheckBlobInfoMatches(read_result_value.external_objects));
  EXPECT_TRUE(CheckBlobReadsMatchWrites(read_result_value.external_objects));
  for (size_t i = 0; i < read_result_value.external_objects.size(); ++i) {
    read_result_value.external_objects[i].mark_used_callback().Run();
  }

  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction3 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction3->Begin(CreateDummyLock());
  EXPECT_TRUE(
      backing_store()
          ->DeleteRange(transaction3.get(), 1, 1, IndexedDBKeyRange(key3_))
          .ok());
  succeeded = false;
  EXPECT_TRUE(
      transaction3->CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  RunAllTasksUntilIdle();

  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(transaction3->CommitPhaseTwo().ok());
  EXPECT_EQ(0U, backing_store()->removals().size());
  for (size_t i = 0; i < read_result_value.external_objects.size(); ++i) {
    read_result_value.external_objects[i].release_callback().Run();
  }
  RunAllTasksUntilIdle();

  EXPECT_TRUE(backing_store()->IsBlobCleanupPending());
#if DCHECK_IS_ON()
  EXPECT_EQ(3,
            backing_store()->NumAggregatedJournalCleaningRequestsForTesting());
#endif
  for (int i = 3; i < IndexedDBBackingStore::kMaxJournalCleanRequests; ++i) {
    backing_store()->StartJournalCleaningTimer();
  }
  EXPECT_NE(0U, backing_store()->removals().size());
  EXPECT_TRUE(CheckBlobRemovals());
#if DCHECK_IS_ON()
  EXPECT_EQ(3, backing_store()->NumBlobFilesDeletedForTesting());
#endif
  EXPECT_FALSE(backing_store()->IsBlobCleanupPending());

  // Clean on the IDB sequence.
  transaction1.reset();
  transaction3.reset();
  RunAllTasksUntilIdle();
}

// Make sure that using very high ( more than 32 bit ) values for database_id
// and object_store_id still work.
TEST_F(IndexedDBBackingStoreTest, HighIds) {
  IndexedDBKey key1 = key1_;
  IndexedDBKey key2 = key2_;
  IndexedDBValue value1 = value1_;

  const int64_t high_database_id = 1ULL << 35;
  const int64_t high_object_store_id = 1ULL << 39;
  // index_ids are capped at 32 bits for storage purposes.
  const int64_t high_index_id = 1ULL << 29;

  const int64_t invalid_high_index_id = 1ULL << 37;

  const IndexedDBKey& index_key = key2;
  std::string index_key_raw;
  EncodeIDBKey(index_key, &index_key_raw);
  {
    IndexedDBBackingStore::Transaction transaction1(
        backing_store()->AsWeakPtr(),
        blink::mojom::IDBTransactionDurability::Relaxed,
        blink::mojom::IDBTransactionMode::ReadWrite);
    transaction1.Begin(CreateDummyLock());
    IndexedDBBackingStore::RecordIdentifier record;
    leveldb::Status s = backing_store()->PutRecord(
        &transaction1, high_database_id, high_object_store_id, key1, &value1,
        &record);
    EXPECT_TRUE(s.ok());

    s = backing_store()->PutIndexDataForRecord(
        &transaction1, high_database_id, high_object_store_id,
        invalid_high_index_id, index_key, record);
    EXPECT_FALSE(s.ok());

    s = backing_store()->PutIndexDataForRecord(
        &transaction1, high_database_id, high_object_store_id, high_index_id,
        index_key, record);
    EXPECT_TRUE(s.ok());

    bool succeeded = false;
    EXPECT_TRUE(
        transaction1.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction1.CommitPhaseTwo().ok());
  }

  {
    IndexedDBBackingStore::Transaction transaction2(
        backing_store()->AsWeakPtr(),
        blink::mojom::IDBTransactionDurability::Relaxed,
        blink::mojom::IDBTransactionMode::ReadWrite);
    transaction2.Begin(CreateDummyLock());
    IndexedDBValue result_value;
    leveldb::Status s =
        backing_store()->GetRecord(&transaction2, high_database_id,
                                   high_object_store_id, key1, &result_value);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(value1.bits, result_value.bits);

    std::unique_ptr<IndexedDBKey> new_primary_key;
    s = backing_store()->GetPrimaryKeyViaIndex(
        &transaction2, high_database_id, high_object_store_id,
        invalid_high_index_id, index_key, &new_primary_key);
    EXPECT_FALSE(s.ok());

    s = backing_store()->GetPrimaryKeyViaIndex(
        &transaction2, high_database_id, high_object_store_id, high_index_id,
        index_key, &new_primary_key);
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(new_primary_key->Equals(key1));

    bool succeeded = false;
    EXPECT_TRUE(
        transaction2.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction2.CommitPhaseTwo().ok());
  }

  CycleIDBTaskRunner();
}

// Make sure that other invalid ids do not crash.
TEST_F(IndexedDBBackingStoreTest, InvalidIds) {
  base::RunLoop loop;
  idb_context_->IDBTaskRunner()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&]() {
        const IndexedDBKey key = key1_;
        IndexedDBValue value = value1_;

        // valid ids for use when testing invalid ids
        const int64_t database_id = 1;
        const int64_t object_store_id = 1;
        const int64_t index_id = kMinimumIndexId;
        // index_ids must be > kMinimumIndexId
        const int64_t invalid_low_index_id = 19;
        IndexedDBValue result_value;

        IndexedDBBackingStore::Transaction transaction1(
            backing_store()->AsWeakPtr(),
            blink::mojom::IDBTransactionDurability::Relaxed,
            blink::mojom::IDBTransactionMode::ReadWrite);
        transaction1.Begin(CreateDummyLock());

        IndexedDBBackingStore::RecordIdentifier record;
        leveldb::Status s = backing_store()->PutRecord(
            &transaction1, database_id, KeyPrefix::kInvalidId, key, &value,
            &record);
        EXPECT_FALSE(s.ok());
        s = backing_store()->PutRecord(&transaction1, database_id, 0, key,
                                       &value, &record);
        EXPECT_FALSE(s.ok());
        s = backing_store()->PutRecord(&transaction1, KeyPrefix::kInvalidId,
                                       object_store_id, key, &value, &record);
        EXPECT_FALSE(s.ok());
        s = backing_store()->PutRecord(&transaction1, 0, object_store_id, key,
                                       &value, &record);
        EXPECT_FALSE(s.ok());

        s = backing_store()->GetRecord(&transaction1, database_id,
                                       KeyPrefix::kInvalidId, key,
                                       &result_value);
        EXPECT_FALSE(s.ok());
        s = backing_store()->GetRecord(&transaction1, database_id, 0, key,
                                       &result_value);
        EXPECT_FALSE(s.ok());
        s = backing_store()->GetRecord(&transaction1, KeyPrefix::kInvalidId,
                                       object_store_id, key, &result_value);
        EXPECT_FALSE(s.ok());
        s = backing_store()->GetRecord(&transaction1, 0, object_store_id, key,
                                       &result_value);
        EXPECT_FALSE(s.ok());

        std::unique_ptr<IndexedDBKey> new_primary_key;
        s = backing_store()->GetPrimaryKeyViaIndex(
            &transaction1, database_id, object_store_id, KeyPrefix::kInvalidId,
            key, &new_primary_key);
        EXPECT_FALSE(s.ok());
        s = backing_store()->GetPrimaryKeyViaIndex(
            &transaction1, database_id, object_store_id, invalid_low_index_id,
            key, &new_primary_key);
        EXPECT_FALSE(s.ok());
        s = backing_store()->GetPrimaryKeyViaIndex(&transaction1, database_id,
                                                   object_store_id, 0, key,
                                                   &new_primary_key);
        EXPECT_FALSE(s.ok());

        s = backing_store()->GetPrimaryKeyViaIndex(
            &transaction1, KeyPrefix::kInvalidId, object_store_id, index_id,
            key, &new_primary_key);
        EXPECT_FALSE(s.ok());
        s = backing_store()->GetPrimaryKeyViaIndex(
            &transaction1, database_id, KeyPrefix::kInvalidId, index_id, key,
            &new_primary_key);
        EXPECT_FALSE(s.ok());
        loop.Quit();
      }));
  loop.Run();
}

TEST_F(IndexedDBBackingStoreTest, CreateDatabase) {
  base::RunLoop loop;
  idb_context_->IDBTaskRunner()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&]() {
        const base::string16 database_name(ASCIIToUTF16("db1"));
        int64_t database_id;
        const int64_t version = 9;

        const int64_t object_store_id = 99;
        const base::string16 object_store_name(ASCIIToUTF16("object_store1"));
        const bool auto_increment = true;
        const IndexedDBKeyPath object_store_key_path(
            ASCIIToUTF16("object_store_key"));

        const int64_t index_id = 999;
        const base::string16 index_name(ASCIIToUTF16("index1"));
        const bool unique = true;
        const bool multi_entry = true;
        const IndexedDBKeyPath index_key_path(ASCIIToUTF16("index_key"));

        IndexedDBMetadataCoding metadata_coding;

        {
          IndexedDBDatabaseMetadata database;
          leveldb::Status s = metadata_coding.CreateDatabase(
              backing_store()->db(), backing_store()->origin_identifier(),
              database_name, version, &database);
          EXPECT_TRUE(s.ok());
          EXPECT_GT(database.id, 0);
          database_id = database.id;

          IndexedDBBackingStore::Transaction transaction(
              backing_store()->AsWeakPtr(),
              blink::mojom::IDBTransactionDurability::Relaxed,
              blink::mojom::IDBTransactionMode::ReadWrite);
          transaction.Begin(CreateDummyLock());

          IndexedDBObjectStoreMetadata object_store;
          s = metadata_coding.CreateObjectStore(
              transaction.transaction(), database.id, object_store_id,
              object_store_name, object_store_key_path, auto_increment,
              &object_store);
          EXPECT_TRUE(s.ok());

          IndexedDBIndexMetadata index;
          s = metadata_coding.CreateIndex(
              transaction.transaction(), database.id, object_store.id, index_id,
              index_name, index_key_path, unique, multi_entry, &index);
          EXPECT_TRUE(s.ok());

          bool succeeded = false;
          EXPECT_TRUE(
              transaction.CommitPhaseOne(CreateBlobWriteCallback(&succeeded))
                  .ok());
          EXPECT_TRUE(succeeded);
          EXPECT_TRUE(transaction.CommitPhaseTwo().ok());
        }

        {
          IndexedDBDatabaseMetadata database;
          bool found;
          leveldb::Status s = metadata_coding.ReadMetadataForDatabaseName(
              backing_store()->db(), backing_store()->origin_identifier(),
              database_name, &database, &found);
          EXPECT_TRUE(s.ok());
          EXPECT_TRUE(found);

          // database.name is not filled in by the implementation.
          EXPECT_EQ(version, database.version);
          EXPECT_EQ(database_id, database.id);

          EXPECT_EQ(1UL, database.object_stores.size());
          IndexedDBObjectStoreMetadata object_store =
              database.object_stores[object_store_id];
          EXPECT_EQ(object_store_name, object_store.name);
          EXPECT_EQ(object_store_key_path, object_store.key_path);
          EXPECT_EQ(auto_increment, object_store.auto_increment);

          EXPECT_EQ(1UL, object_store.indexes.size());
          IndexedDBIndexMetadata index = object_store.indexes[index_id];
          EXPECT_EQ(index_name, index.name);
          EXPECT_EQ(index_key_path, index.key_path);
          EXPECT_EQ(unique, index.unique);
          EXPECT_EQ(multi_entry, index.multi_entry);
        }
        loop.Quit();
      }));
  loop.Run();

  {
    // Cycle the idb runner to help clean up tasks for the Windows tests.
    base::RunLoop cycle_loop;
    idb_context_->IDBTaskRunner()->PostTask(FROM_HERE,
                                            cycle_loop.QuitClosure());
    cycle_loop.Run();
  }
}

TEST_F(IndexedDBBackingStoreTest, GetDatabaseNames) {
  const base::string16 db1_name(ASCIIToUTF16("db1"));
  const int64_t db1_version = 1LL;

  // Database records with DEFAULT_VERSION represent
  // stale data, and should not be enumerated.
  const base::string16 db2_name(ASCIIToUTF16("db2"));
  const int64_t db2_version = IndexedDBDatabaseMetadata::DEFAULT_VERSION;
  IndexedDBMetadataCoding metadata_coding;

  IndexedDBDatabaseMetadata db1;
  leveldb::Status s = metadata_coding.CreateDatabase(
      backing_store()->db(), backing_store()->origin_identifier(), db1_name,
      db1_version, &db1);
  EXPECT_TRUE(s.ok());
  EXPECT_GT(db1.id, 0LL);

  IndexedDBDatabaseMetadata db2;
  s = metadata_coding.CreateDatabase(backing_store()->db(),
                                     backing_store()->origin_identifier(),
                                     db2_name, db2_version, &db2);
  EXPECT_TRUE(s.ok());
  EXPECT_GT(db2.id, db1.id);

  std::vector<base::string16> names;
  s = metadata_coding.ReadDatabaseNames(
      backing_store()->db(), backing_store()->origin_identifier(), &names);
  EXPECT_TRUE(s.ok());
  ASSERT_EQ(1U, names.size());
  EXPECT_EQ(db1_name, names[0]);
}

TEST_F(IndexedDBBackingStoreTest, ReadCorruptionInfo) {
  // No |path_base|.
  EXPECT_TRUE(
      indexed_db::ReadCorruptionInfo(base::FilePath(), Origin()).empty());

  const base::FilePath path_base = temp_dir_.GetPath();
  const Origin origin = Origin::Create(GURL("http://www.google.com/"));
  ASSERT_FALSE(path_base.empty());
  ASSERT_TRUE(PathIsWritable(path_base));

  // File not found.
  EXPECT_TRUE(indexed_db::ReadCorruptionInfo(path_base, origin).empty());

  const base::FilePath info_path =
      path_base.AppendASCII("http_www.google.com_0.indexeddb.leveldb")
          .AppendASCII("corruption_info.json");
  ASSERT_TRUE(CreateDirectory(info_path.DirName()));

  // Empty file.
  std::string dummy_data;
  ASSERT_TRUE(WriteFile(info_path, dummy_data));
  EXPECT_TRUE(indexed_db::ReadCorruptionInfo(path_base, origin).empty());
  EXPECT_FALSE(PathExists(info_path));

  // File size > 4 KB.
  dummy_data.resize(5000, 'c');
  ASSERT_TRUE(WriteFile(info_path, dummy_data));
  EXPECT_TRUE(indexed_db::ReadCorruptionInfo(path_base, origin).empty());
  EXPECT_FALSE(PathExists(info_path));

  // Random string.
  ASSERT_TRUE(WriteFile(info_path, "foo bar"));
  EXPECT_TRUE(indexed_db::ReadCorruptionInfo(path_base, origin).empty());
  EXPECT_FALSE(PathExists(info_path));

  // Not a dictionary.
  ASSERT_TRUE(WriteFile(info_path, "[]"));
  EXPECT_TRUE(indexed_db::ReadCorruptionInfo(path_base, origin).empty());
  EXPECT_FALSE(PathExists(info_path));

  // Empty dictionary.
  ASSERT_TRUE(WriteFile(info_path, "{}"));
  EXPECT_TRUE(indexed_db::ReadCorruptionInfo(path_base, origin).empty());
  EXPECT_FALSE(PathExists(info_path));

  // Dictionary, no message key.
  ASSERT_TRUE(WriteFile(info_path, "{\"foo\":\"bar\"}"));
  EXPECT_TRUE(indexed_db::ReadCorruptionInfo(path_base, origin).empty());
  EXPECT_FALSE(PathExists(info_path));

  // Dictionary, message key.
  ASSERT_TRUE(WriteFile(info_path, "{\"message\":\"bar\"}"));
  std::string message = indexed_db::ReadCorruptionInfo(path_base, origin);
  EXPECT_FALSE(message.empty());
  EXPECT_FALSE(PathExists(info_path));
  EXPECT_EQ("bar", message);

  // Dictionary, message key and more.
  ASSERT_TRUE(WriteFile(info_path, "{\"message\":\"foo\",\"bar\":5}"));
  message = indexed_db::ReadCorruptionInfo(path_base, origin);
  EXPECT_FALSE(message.empty());
  EXPECT_FALSE(PathExists(info_path));
  EXPECT_EQ("foo", message);
}

// There was a wrong migration from schema 2 to 3, which always delete IDB
// blobs and doesn't actually write the new schema version. This tests the
// upgrade path where the database doesn't have blob entries, so it' safe to
// keep the database.
// https://crbug.com/756447, https://crbug.com/829125, https://crbug.com/829141
TEST_F(IndexedDBBackingStoreTest, SchemaUpgradeWithoutBlobsSurvives) {
  int64_t database_id;
  const int64_t object_store_id = 99;

  // The database metadata needs to be written so we can verify the blob entry
  // keys are not detected.
  const base::string16 database_name(ASCIIToUTF16("db1"));
  const int64_t version = 9;

  const base::string16 object_store_name(ASCIIToUTF16("object_store1"));
  const bool auto_increment = true;
  const IndexedDBKeyPath object_store_key_path(
      ASCIIToUTF16("object_store_key"));

  IndexedDBMetadataCoding metadata_coding;

  {
    IndexedDBDatabaseMetadata database;
    leveldb::Status s = metadata_coding.CreateDatabase(
        backing_store()->db(), backing_store()->origin_identifier(),
        database_name, version, &database);
    EXPECT_TRUE(s.ok());
    EXPECT_GT(database.id, 0);
    database_id = database.id;

    IndexedDBBackingStore::Transaction transaction(
        backing_store()->AsWeakPtr(),
        blink::mojom::IDBTransactionDurability::Relaxed,
        blink::mojom::IDBTransactionMode::ReadWrite);
    transaction.Begin(CreateDummyLock());

    IndexedDBObjectStoreMetadata object_store;
    s = metadata_coding.CreateObjectStore(
        transaction.transaction(), database.id, object_store_id,
        object_store_name, object_store_key_path, auto_increment,
        &object_store);
    EXPECT_TRUE(s.ok());

    bool succeeded = false;
    EXPECT_TRUE(
        transaction.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction.CommitPhaseTwo().ok());
  }
  RunAllTasksUntilIdle();

  // Save a value.
  IndexedDBBackingStore::Transaction transaction1(
      backing_store()->AsWeakPtr(),
      blink::mojom::IDBTransactionDurability::Relaxed,
      blink::mojom::IDBTransactionMode::ReadWrite);
  transaction1.Begin(CreateDummyLock());
  IndexedDBBackingStore::RecordIdentifier record;
  leveldb::Status s = backing_store()->PutRecord(
      &transaction1, database_id, object_store_id, key1_, &value1_, &record);
  EXPECT_TRUE(s.ok());
  bool succeeded = false;
  EXPECT_TRUE(
      transaction1.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(transaction1.CommitPhaseTwo().ok());

  // Set the schema to 2, which was before blob support.
  std::unique_ptr<LevelDBWriteBatch> write_batch = LevelDBWriteBatch::Create();
  const std::string schema_version_key = SchemaVersionKey::Encode();
  ignore_result(indexed_db::PutInt(write_batch.get(), schema_version_key, 2));
  ASSERT_TRUE(backing_store()->db()->Write(write_batch.get()).ok());
  RunAllTasksUntilIdle();

  DestroyFactoryAndBackingStore();
  CreateFactoryAndBackingStore();

  IndexedDBBackingStore::Transaction transaction2(
      backing_store()->AsWeakPtr(),
      blink::mojom::IDBTransactionDurability::Relaxed,
      blink::mojom::IDBTransactionMode::ReadWrite);
  transaction2.Begin(CreateDummyLock());
  IndexedDBValue result_value;
  EXPECT_TRUE(backing_store()
                  ->GetRecord(&transaction2, database_id, object_store_id,
                              key1_, &result_value)
                  .ok());
  succeeded = false;
  EXPECT_TRUE(
      transaction2.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(transaction2.CommitPhaseTwo().ok());
  EXPECT_EQ(value1_.bits, result_value.bits);

  // Test that we upgraded.
  int64_t found_int = 0;
  bool found = false;
  bool success = indexed_db::GetInt(backing_store()->db(), schema_version_key,
                                    &found_int, &found)
                     .ok();
  ASSERT_TRUE(success);

  EXPECT_TRUE(found);
  EXPECT_EQ(4, found_int);
  RunAllTasksUntilIdle();
}

// Our v2->v3 schema migration code forgot to bump the on-disk version number.
// This test covers migrating a v3 database mislabeled as v2 to a properly
// labeled v3 database. When the mislabeled database has blob entries, we must
// treat it as corrupt and delete it.
// https://crbug.com/756447, https://crbug.com/829125, https://crbug.com/829141
TEST_F(IndexedDBBackingStoreTestWithBlobs, SchemaUpgradeWithBlobsCorrupt) {
  int64_t database_id;
  const int64_t object_store_id = 99;

  // The database metadata needs to be written so the blob entry keys can
  // be detected.
  const base::string16 database_name(ASCIIToUTF16("db1"));
  const int64_t version = 9;

  const base::string16 object_store_name(ASCIIToUTF16("object_store1"));
  const bool auto_increment = true;
  const IndexedDBKeyPath object_store_key_path(
      ASCIIToUTF16("object_store_key"));

  IndexedDBMetadataCoding metadata_coding;

  {
    IndexedDBDatabaseMetadata database;
    leveldb::Status s = metadata_coding.CreateDatabase(
        backing_store()->db(), backing_store()->origin_identifier(),
        database_name, version, &database);
    EXPECT_TRUE(s.ok());
    EXPECT_GT(database.id, 0);
    database_id = database.id;

    IndexedDBBackingStore::Transaction transaction(
        backing_store()->AsWeakPtr(),
        blink::mojom::IDBTransactionDurability::Relaxed,
        blink::mojom::IDBTransactionMode::ReadWrite);
    transaction.Begin(CreateDummyLock());

    IndexedDBObjectStoreMetadata object_store;
    s = metadata_coding.CreateObjectStore(
        transaction.transaction(), database.id, object_store_id,
        object_store_name, object_store_key_path, auto_increment,
        &object_store);
    EXPECT_TRUE(s.ok());

    bool succeeded = false;
    EXPECT_TRUE(
        transaction.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction.CommitPhaseTwo().ok());
  }
  RunAllTasksUntilIdle();

  base::RunLoop write_blobs_loop;
  // Initiate transaction1 - writing blobs.
  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction1 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction1->Begin(CreateDummyLock());
  IndexedDBBackingStore::RecordIdentifier record;
  EXPECT_TRUE(backing_store()
                  ->PutRecord(transaction1.get(), database_id, object_store_id,
                              key3_, &value3_, &record)
                  .ok());
  bool succeeded = false;
  EXPECT_TRUE(transaction1
                  ->CommitPhaseOne(CreateBlobWriteCallback(
                      &succeeded, write_blobs_loop.QuitClosure()))
                  .ok());
  RunAllTasksUntilIdle();
  write_blobs_loop.Run();

  // Finish up transaction1, verifying blob writes.
  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(CheckBlobWrites());
  EXPECT_TRUE(transaction1->CommitPhaseTwo().ok());

  // Set the schema to 2, which was before blob support.
  std::unique_ptr<LevelDBWriteBatch> write_batch = LevelDBWriteBatch::Create();
  const std::string schema_version_key = SchemaVersionKey::Encode();
  ignore_result(indexed_db::PutInt(write_batch.get(), schema_version_key, 2));
  ASSERT_TRUE(backing_store()->db()->Write(write_batch.get()).ok());

  // Clean up on the IDB sequence.
  transaction1.reset();
  RunAllTasksUntilIdle();

  DestroyFactoryAndBackingStore();
  CreateFactoryAndBackingStore();

  // The factory returns a null backing store pointer when there is a corrupt
  // database.
  EXPECT_TRUE(data_loss_info_.status == blink::mojom::IDBDataLoss::Total);
}

namespace {

// v3 Blob Data is encoded as a series of:
//   { is_file [bool], blob_number [int64_t as varInt],
//     type [string-with-length, may be empty],
//     (for Blobs only) size [int64_t as varInt]
//     (for Files only) fileName [string-with-length]
//   }
// There is no length field; just read until you run out of data.
std::string EncodeV3BlobInfos(
    const std::vector<IndexedDBExternalObject>& blob_info) {
  std::string ret;
  for (const auto& info : blob_info) {
    EncodeBool(info.is_file(), &ret);
    EncodeVarInt(info.blob_number(), &ret);
    EncodeStringWithLength(info.type(), &ret);
    if (info.is_file())
      EncodeStringWithLength(info.file_name(), &ret);
    else
      EncodeVarInt(info.size(), &ret);
  }
  return ret;
}

}  // namespace

TEST_F(IndexedDBBackingStoreTestWithBlobs, SchemaUpgradeV3ToV4) {
  int64_t database_id;
  const int64_t object_store_id = 99;

  const base::string16 database_name(ASCIIToUTF16("db1"));
  const int64_t version = 9;

  const base::string16 object_store_name(ASCIIToUTF16("object_store1"));
  const bool auto_increment = true;
  const IndexedDBKeyPath object_store_key_path(
      ASCIIToUTF16("object_store_key"));

  IndexedDBMetadataCoding metadata_coding;

  {
    IndexedDBDatabaseMetadata database;
    leveldb::Status s = metadata_coding.CreateDatabase(
        backing_store()->db(), backing_store()->origin_identifier(),
        database_name, version, &database);
    EXPECT_TRUE(s.ok());
    EXPECT_GT(database.id, 0);
    database_id = database.id;

    IndexedDBBackingStore::Transaction transaction(
        backing_store()->AsWeakPtr(),
        blink::mojom::IDBTransactionDurability::Relaxed,
        blink::mojom::IDBTransactionMode::ReadWrite);
    transaction.Begin(CreateDummyLock());

    IndexedDBObjectStoreMetadata object_store;
    s = metadata_coding.CreateObjectStore(
        transaction.transaction(), database.id, object_store_id,
        object_store_name, object_store_key_path, auto_increment,
        &object_store);
    EXPECT_TRUE(s.ok());

    bool succeeded = false;
    EXPECT_TRUE(
        transaction.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(transaction.CommitPhaseTwo().ok());
  }
  RunAllTasksUntilIdle();

  // Initiate transaction1 - writing blobs.
  std::unique_ptr<IndexedDBBackingStore::Transaction> transaction1 =
      std::make_unique<IndexedDBBackingStore::Transaction>(
          backing_store()->AsWeakPtr(),
          blink::mojom::IDBTransactionDurability::Relaxed,
          blink::mojom::IDBTransactionMode::ReadWrite);
  transaction1->Begin(CreateDummyLock());
  IndexedDBBackingStore::RecordIdentifier record;
  EXPECT_TRUE(backing_store()
                  ->PutRecord(transaction1.get(), database_id, object_store_id,
                              key3_, &value3_, &record)
                  .ok());
  bool succeeded = false;
  base::RunLoop write_blobs_loop;
  EXPECT_TRUE(transaction1
                  ->CommitPhaseOne(CreateBlobWriteCallback(
                      &succeeded, write_blobs_loop.QuitClosure()))
                  .ok());
  write_blobs_loop.Run();
  RunAllTasksUntilIdle();

  // Finish up transaction1, verifying blob writes.
  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(CheckBlobWrites());
  ASSERT_TRUE(transaction1->CommitPhaseTwo().ok());
  transaction1.reset();

  RunAllTasksUntilIdle();

  // Change entries to be v3, and change the schema to be v3.
  std::unique_ptr<LevelDBWriteBatch> write_batch = LevelDBWriteBatch::Create();
  const std::string schema_version_key = SchemaVersionKey::Encode();
  ASSERT_TRUE(
      indexed_db::PutInt(write_batch.get(), schema_version_key, 3).ok());
  const std::string object_store_data_key =
      ObjectStoreDataKey::Encode(database_id, object_store_id, key3_);
  base::StringPiece leveldb_key_piece(object_store_data_key);
  BlobEntryKey blob_entry_key;
  ASSERT_TRUE(BlobEntryKey::FromObjectStoreDataKey(&leveldb_key_piece,
                                                   &blob_entry_key));
  ASSERT_EQ(blob_context_->writes().size(), 3u);
  auto& writes = blob_context_->writes();
  external_objects()[0].set_blob_number(writes[0].GetBlobNumber());
  external_objects()[1].set_blob_number(writes[1].GetBlobNumber());
  external_objects()[2].set_blob_number(writes[2].GetBlobNumber());
  std::string v3_blob_data = EncodeV3BlobInfos(external_objects());
  write_batch->Put(base::StringPiece(blob_entry_key.Encode()),
                   base::StringPiece(v3_blob_data));
  ASSERT_TRUE(backing_store()->db()->Write(write_batch.get()).ok());

  // The migration code uses the physical files on disk, so those need to be
  // written with the correct size & timestamp.
  base::FilePath file1_path = writes[1].path;
  base::FilePath file2_path = writes[2].path;
  ASSERT_TRUE(CreateDirectory(file1_path.DirName()));
  ASSERT_TRUE(CreateDirectory(file2_path.DirName()));
  base::File file1(file1_path,
                   base::File::FLAG_WRITE | base::File::FLAG_CREATE_ALWAYS);
  ASSERT_TRUE(file1.IsValid());
  ASSERT_TRUE(file1.WriteAtCurrentPosAndCheck(
      base::as_bytes(base::make_span(kBlobFileData1))));
  ASSERT_TRUE(file1.SetTimes(external_objects()[1].last_modified(),
                             external_objects()[1].last_modified()));
  file1.Close();

  base::File file2(file2_path,
                   base::File::FLAG_WRITE | base::File::FLAG_CREATE_ALWAYS);
  ASSERT_TRUE(file2.IsValid());
  ASSERT_TRUE(file2.WriteAtCurrentPosAndCheck(
      base::as_bytes(base::make_span(kBlobFileData2))));
  ASSERT_TRUE(file2.SetTimes(external_objects()[2].last_modified(),
                             external_objects()[2].last_modified()));
  file2.Close();

  DestroyFactoryAndBackingStore();
  CreateFactoryAndBackingStore();

  // There should be no corruption.
  ASSERT_TRUE(data_loss_info_.status == blink::mojom::IDBDataLoss::None);

  // Initiate transaction2, reading blobs.
  IndexedDBBackingStore::Transaction transaction2(
      backing_store()->AsWeakPtr(),
      blink::mojom::IDBTransactionDurability::Relaxed,
      blink::mojom::IDBTransactionMode::ReadWrite);
  transaction2.Begin(CreateDummyLock());
  IndexedDBValue result_value;
  EXPECT_TRUE(backing_store()
                  ->GetRecord(&transaction2, database_id, object_store_id,
                              key3_, &result_value)
                  .ok());

  // Finish up transaction2, verifying blob reads.
  succeeded = false;
  EXPECT_TRUE(
      transaction2.CommitPhaseOne(CreateBlobWriteCallback(&succeeded)).ok());
  EXPECT_TRUE(succeeded);
  EXPECT_TRUE(transaction2.CommitPhaseTwo().ok());
  EXPECT_EQ(value3_.bits, result_value.bits);
  EXPECT_TRUE(CheckBlobInfoMatches(result_value.external_objects));
}

}  // namespace indexed_db_backing_store_unittest
}  // namespace content
