// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_TEST_SANDBOX_FILE_SYSTEM_TEST_HELPER_H_
#define CONTENT_PUBLIC_TEST_SANDBOX_FILE_SYSTEM_TEST_HELPER_H_

#include <stdint.h>

#include <string>

#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "storage/browser/file_system/file_system_url.h"
#include "storage/browser/file_system/file_system_usage_cache.h"
#include "storage/browser/file_system/task_runner_bound_observer_list.h"
#include "storage/common/file_system/file_system_types.h"
#include "storage/common/file_system/file_system_util.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom.h"

namespace base {
class FilePath;
}

namespace storage {
class FileSystemContext;
class FileSystemFileUtil;
class FileSystemOperationContext;
class FileSystemOperationRunner;
class ObfuscatedFileUtilDelegate;
class QuotaManagerProxy;
}  // namespace storage

namespace url {
class Origin;
}

namespace content {

// Filesystem test helper class that encapsulates test environment for
// a given {origin, type} pair.  This helper only works for sandboxed
// file systems (Temporary or Persistent).
class SandboxFileSystemTestHelper {
 public:
  SandboxFileSystemTestHelper(const url::Origin& origin,
                              storage::FileSystemType type);
  SandboxFileSystemTestHelper();
  ~SandboxFileSystemTestHelper();

  void SetUp(const base::FilePath& base_dir);
  // If you want to use more than one SandboxFileSystemTestHelper in
  // a single base directory, they have to share a context, so that they don't
  // have multiple databases fighting over the lock to the origin directory
  // [deep down inside ObfuscatedFileUtil].
  void SetUp(storage::FileSystemContext* file_system_context);
  void SetUp(const base::FilePath& base_dir,
             storage::QuotaManagerProxy* quota_manager_proxy);
  void TearDown();

  base::FilePath GetOriginRootPath();
  base::FilePath GetLocalPath(const base::FilePath& path);
  base::FilePath GetLocalPathFromASCII(const std::string& path);

  // Returns empty path if filesystem type is neither temporary nor persistent.
  base::FilePath GetUsageCachePath() const;

  storage::FileSystemURL CreateURL(const base::FilePath& path) const;
  storage::FileSystemURL CreateURLFromUTF8(const std::string& utf8) const {
    return CreateURL(base::FilePath::FromUTF8Unsafe(utf8));
  }

  // This returns cached usage size returned by QuotaUtil.
  int64_t GetCachedOriginUsage() const;

  // This doesn't work with OFSFU.
  int64_t ComputeCurrentOriginUsage();

  int64_t ComputeCurrentDirectoryDatabaseUsage();

  storage::FileSystemOperationRunner* operation_runner();
  storage::FileSystemOperationContext* NewOperationContext();

  void AddFileChangeObserver(storage::FileChangeObserver* observer);
  void AddFileUpdateObserver(storage::FileUpdateObserver* observer);

  storage::FileSystemContext* file_system_context() const {
    return file_system_context_.get();
  }

  const url::Origin& origin() const { return origin_; }
  storage::FileSystemType type() const { return type_; }
  blink::mojom::StorageType storage_type() const {
    return storage::FileSystemTypeToQuotaStorageType(type_);
  }
  storage::FileSystemFileUtil* file_util() const { return file_util_; }
  storage::FileSystemUsageCache* usage_cache();

  storage::ObfuscatedFileUtilDelegate* file_util_delegate();

 private:
  void SetUpFileSystem();

  scoped_refptr<storage::FileSystemContext> file_system_context_;

  const url::Origin origin_;
  const storage::FileSystemType type_;
  storage::FileSystemFileUtil* file_util_;
};

}  // namespace content

#endif  // CONTENT_PUBLIC_TEST_SANDBOX_FILE_SYSTEM_TEST_HELPER_H_
