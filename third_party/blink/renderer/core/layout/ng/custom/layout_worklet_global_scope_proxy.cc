// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/custom/layout_worklet_global_scope_proxy.h"

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/script/script_type.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/script_source_code.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_client.h"
#include "third_party/blink/renderer/core/loader/worker_fetch_context.h"
#include "third_party/blink/renderer/core/origin_trials/origin_trial_context.h"
#include "third_party/blink/renderer/core/script/script.h"
#include "third_party/blink/renderer/core/workers/global_scope_creation_params.h"
#include "third_party/blink/renderer/core/workers/worklet_module_responses_map.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"

namespace blink {

LayoutWorkletGlobalScopeProxy* LayoutWorkletGlobalScopeProxy::From(
    WorkletGlobalScopeProxy* proxy) {
  return static_cast<LayoutWorkletGlobalScopeProxy*>(proxy);
}

LayoutWorkletGlobalScopeProxy::LayoutWorkletGlobalScopeProxy(
    LocalFrame* frame,
    WorkletModuleResponsesMap* module_responses_map,
    PendingLayoutRegistry* pending_layout_registry,
    size_t global_scope_number) {
  DCHECK(IsMainThread());
  Document* document = frame->GetDocument();
  reporting_proxy_ =
      std::make_unique<MainThreadWorkletReportingProxy>(document);

  String global_scope_name =
      StringView("LayoutWorklet #") + String::Number(global_scope_number);

  auto creation_params = std::make_unique<GlobalScopeCreationParams>(false, std::string(),
      document->Url(), mojom::ScriptType::kModule,
      OffMainThreadWorkerScriptFetchOption::kEnabled, global_scope_name,
      document->UserAgent(), frame->Client()->CreateWorkerFetchContext(),
      document->GetContentSecurityPolicy()->Headers(),
      document->GetReferrerPolicy(), document->GetSecurityOrigin(),
      document->IsSecureContext(), document->GetHttpsState(),
      nullptr /* worker_clients */,
      frame->Client()->CreateWorkerContentSettingsClient(),
      document->GetSecurityContext().AddressSpace(),
      OriginTrialContext::GetTokens(document).get(),
      base::UnguessableToken::Create(), nullptr /* worker_settings */,
      kV8CacheOptionsDefault, module_responses_map,
      mojo::NullRemote() /* browser_interface_broker */,
      BeginFrameProviderParams(), nullptr /* parent_feature_policy */,
      base::UnguessableToken() /* agent_cluster_id */);
  global_scope_ = LayoutWorkletGlobalScope::Create(
      frame, std::move(creation_params), *reporting_proxy_,
      pending_layout_registry);
}

void LayoutWorkletGlobalScopeProxy::FetchAndInvokeScript(
    const KURL& module_url_record,
    network::mojom::CredentialsMode credentials_mode,
    const FetchClientSettingsObjectSnapshot& outside_settings_object,
    WorkerResourceTimingNotifier& outside_resource_timing_notifier,
    scoped_refptr<base::SingleThreadTaskRunner> outside_settings_task_runner,
    WorkletPendingTasks* pending_tasks) {
  DCHECK(IsMainThread());
  global_scope_->FetchAndInvokeScript(
      module_url_record, credentials_mode, outside_settings_object,
      outside_resource_timing_notifier, std::move(outside_settings_task_runner),
      pending_tasks);
}

void LayoutWorkletGlobalScopeProxy::WorkletObjectDestroyed() {
  DCHECK(IsMainThread());
  // Do nothing.
}

void LayoutWorkletGlobalScopeProxy::TerminateWorkletGlobalScope() {
  DCHECK(IsMainThread());
  global_scope_->Dispose();
  // Nullify these fields to cut a potential reference cycle.
  global_scope_ = nullptr;
  reporting_proxy_.reset();
}

CSSLayoutDefinition* LayoutWorkletGlobalScopeProxy::FindDefinition(
    const AtomicString& name) {
  DCHECK(IsMainThread());
  return global_scope_->FindDefinition(name);
}

void LayoutWorkletGlobalScopeProxy::Trace(blink::Visitor* visitor) {
  visitor->Trace(global_scope_);
}

}  // namespace blink
