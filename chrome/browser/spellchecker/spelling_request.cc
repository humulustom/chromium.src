// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/spellchecker/spelling_request.h"

#include "base/barrier_closure.h"
#include "base/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "chrome/browser/spellchecker/spellcheck_custom_dictionary.h"
#include "chrome/browser/spellchecker/spellcheck_factory.h"
#include "components/spellcheck/browser/spellcheck_platform.h"
#include "components/spellcheck/common/spellcheck_features.h"
#include "components/spellcheck/common/spellcheck_result.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"

namespace {

bool CompareLocation(const SpellCheckResult& r1, const SpellCheckResult& r2) {
  return r1.location < r2.location;
}

}  // namespace

SpellingRequest::SpellingRequest(SpellingServiceClient* client,
                                 const base::string16& text,
                                 int render_process_id,
                                 int document_tag,
                                 RequestTextCheckCallback callback,
                                 DestructionCallback destruction_callback)
    : remote_success_(false),
      text_(text),
      callback_(std::move(callback)),
      destruction_callback_(std::move(destruction_callback)) {
  DCHECK(!text_.empty());
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  completion_barrier_ =
      BarrierClosure(2, base::BindOnce(&SpellingRequest::OnCheckCompleted,
                                       weak_factory_.GetWeakPtr()));
  RequestRemoteCheck(client, render_process_id);
  RequestLocalCheck(document_tag);
}

SpellingRequest::~SpellingRequest() = default;

// static
void SpellingRequest::CombineResults(
    std::vector<SpellCheckResult>* remote_results,
    const std::vector<SpellCheckResult>& local_results) {
  std::vector<SpellCheckResult>::const_iterator local_iter(
      local_results.begin());
  std::vector<SpellCheckResult>::iterator remote_iter;

  for (remote_iter = remote_results->begin();
       remote_iter != remote_results->end(); ++remote_iter) {
    // Discard all local results occurring before remote result.
    while (local_iter != local_results.end() &&
           local_iter->location < remote_iter->location) {
      local_iter++;
    }

    remote_iter->spelling_service_used = true;

    // Unless local and remote result coincide, result is GRAMMAR.
    remote_iter->decoration = SpellCheckResult::GRAMMAR;
    if (local_iter != local_results.end() &&
        local_iter->location == remote_iter->location &&
        local_iter->length == remote_iter->length) {
      remote_iter->decoration = SpellCheckResult::SPELLING;
    }
  }
}

void SpellingRequest::RequestRemoteCheck(SpellingServiceClient* client,
                                         int render_process_id) {
  auto* host = content::RenderProcessHost::FromID(render_process_id);
  if (!host)
    return;

  // |this| may be gone at callback invocation if the owner has been removed.
  client->RequestTextCheck(
      host->GetBrowserContext(), SpellingServiceClient::SPELLCHECK, text_,
      base::BindOnce(&SpellingRequest::OnRemoteCheckCompleted,
                     weak_factory_.GetWeakPtr()));
}

void SpellingRequest::RequestLocalCheck(int document_tag) {
  // |this| may be gone at callback invocation if the owner has been removed.
  spellcheck_platform::RequestTextCheck(
      document_tag, text_,
      base::BindOnce(&SpellingRequest::OnLocalCheckCompletedOnAnyThread,
                     weak_factory_.GetWeakPtr()));
}

void SpellingRequest::OnCheckCompleted() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  std::vector<SpellCheckResult>* check_results = &local_results_;
  if (remote_success_) {
    std::sort(remote_results_.begin(), remote_results_.end(), CompareLocation);
    std::sort(local_results_.begin(), local_results_.end(), CompareLocation);
    CombineResults(&remote_results_, local_results_);
    check_results = &remote_results_;
  }

  std::move(callback_).Run(*check_results);

  std::move(destruction_callback_).Run(this);

  // |destruction_callback_| removes |this|. No more operations allowed.
}

void SpellingRequest::OnRemoteCheckCompleted(
    bool success,
    const base::string16& text,
    const std::vector<SpellCheckResult>& results) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  remote_success_ = success;
  remote_results_ = results;
  completion_barrier_.Run();
}

// static
void SpellingRequest::OnLocalCheckCompletedOnAnyThread(
    base::WeakPtr<SpellingRequest> request,
    const std::vector<SpellCheckResult>& results) {
  // Local checking can happen on any thread - don't DCHECK thread.
  base::PostTask(FROM_HERE, {content::BrowserThread::UI},
                 base::BindOnce(&SpellingRequest::OnLocalCheckCompleted,
                                request, results));
}

void SpellingRequest::OnLocalCheckCompleted(
    const std::vector<SpellCheckResult>& results) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  local_results_ = results;
  completion_barrier_.Run();
}
