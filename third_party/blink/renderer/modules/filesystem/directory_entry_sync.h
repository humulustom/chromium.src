/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_FILESYSTEM_DIRECTORY_ENTRY_SYNC_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_FILESYSTEM_DIRECTORY_ENTRY_SYNC_H_

#include "third_party/blink/renderer/modules/filesystem/entry_sync.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class DirectoryReaderSync;
class ExceptionState;
class FileEntrySync;
class FileSystemFlags;

class DirectoryEntrySync final : public EntrySync {
  DEFINE_WRAPPERTYPEINFO();

 public:
  DirectoryEntrySync(DOMFileSystemBase*, const String& full_path);

  bool isDirectory() const override { return true; }

  DirectoryReaderSync* createReader();
  FileEntrySync* getFile(const String& path,
                         const FileSystemFlags*,
                         ExceptionState&);
  DirectoryEntrySync* getDirectory(const String& path,
                                   const FileSystemFlags*,
                                   ExceptionState&);
  void removeRecursively(ExceptionState&);

  void Trace(blink::Visitor*) override;
};

template <>
struct DowncastTraits<DirectoryEntrySync> {
  static bool AllowFrom(const EntrySync& entry) { return entry.isDirectory(); }
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_FILESYSTEM_DIRECTORY_ENTRY_SYNC_H_
