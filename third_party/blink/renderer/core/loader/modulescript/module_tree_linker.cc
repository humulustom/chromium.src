// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/loader/modulescript/module_tree_linker.h"

#include "third_party/blink/renderer/bindings/core/v8/module_record.h"
#include "third_party/blink/renderer/core/loader/modulescript/module_script_fetch_request.h"
#include "third_party/blink/renderer/core/loader/modulescript/module_tree_linker_registry.h"
#include "third_party/blink/renderer/core/script/layered_api.h"
#include "third_party/blink/renderer/core/script/module_script.h"
#include "third_party/blink/renderer/platform/bindings/v8_throw_exception.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_loading_log.h"
#include "third_party/blink/renderer/platform/weborigin/security_policy.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "v8/include/v8.h"

// <specdef label="IMSGF"
// href="https://html.spec.whatwg.org/C/#internal-module-script-graph-fetching-procedure">

// <specdef label="fetch-a-module-script-tree"
// href="https://html.spec.whatwg.org/C/#fetch-a-module-script-tree">

// <specdef
// label="fetch-a-module-worker-script-tree"
// href="https://html.spec.whatwg.org/C/#fetch-a-module-worker-script-tree">

// <specdef label="fetch-an-import()-module-script-graph"
// href="https://html.spec.whatwg.org/C/#fetch-an-import()-module-script-graph">

namespace blink {

void ModuleTreeLinker::Fetch(
    const KURL& url,
    ResourceFetcher* fetch_client_settings_object_fetcher,
    mojom::RequestContextType context_type,
    network::mojom::RequestDestination destination,
    const ScriptFetchOptions& options,
    Modulator* modulator,
    ModuleScriptCustomFetchType custom_fetch_type,
    ModuleTreeLinkerRegistry* registry,
    ModuleTreeClient* client) {
  ModuleTreeLinker* fetcher = MakeGarbageCollected<ModuleTreeLinker>(
      fetch_client_settings_object_fetcher, context_type, destination,
      modulator, custom_fetch_type, registry, client);
  registry->AddFetcher(fetcher);
  fetcher->FetchRoot(url, options);
  DCHECK(fetcher->IsFetching());
}

void ModuleTreeLinker::FetchDescendantsForInlineScript(
    ModuleScript* module_script,
    ResourceFetcher* fetch_client_settings_object_fetcher,
    mojom::RequestContextType context_type,
    network::mojom::RequestDestination destination,
    Modulator* modulator,
    ModuleScriptCustomFetchType custom_fetch_type,
    ModuleTreeLinkerRegistry* registry,
    ModuleTreeClient* client) {
  DCHECK(module_script);
  ModuleTreeLinker* fetcher = MakeGarbageCollected<ModuleTreeLinker>(
      fetch_client_settings_object_fetcher, context_type, destination,
      modulator, custom_fetch_type, registry, client);
  registry->AddFetcher(fetcher);
  fetcher->FetchRootInline(module_script);
  DCHECK(fetcher->IsFetching());
}

ModuleTreeLinker::ModuleTreeLinker(
    ResourceFetcher* fetch_client_settings_object_fetcher,
    mojom::RequestContextType context_type,
    network::mojom::RequestDestination destination,
    Modulator* modulator,
    ModuleScriptCustomFetchType custom_fetch_type,
    ModuleTreeLinkerRegistry* registry,
    ModuleTreeClient* client)
    : fetch_client_settings_object_fetcher_(
          fetch_client_settings_object_fetcher),
      context_type_(context_type),
      destination_(destination),
      modulator_(modulator),
      custom_fetch_type_(custom_fetch_type),
      registry_(registry),
      client_(client) {
  CHECK(modulator);
  CHECK(registry);
  CHECK(client);
}

void ModuleTreeLinker::Trace(blink::Visitor* visitor) {
  visitor->Trace(fetch_client_settings_object_fetcher_);
  visitor->Trace(modulator_);
  visitor->Trace(registry_);
  visitor->Trace(client_);
  visitor->Trace(result_);
  SingleModuleClient::Trace(visitor);
}

#if DCHECK_IS_ON()
const char* ModuleTreeLinker::StateToString(ModuleTreeLinker::State state) {
  switch (state) {
    case State::kInitial:
      return "Initial";
    case State::kFetchingSelf:
      return "FetchingSelf";
    case State::kFetchingDependencies:
      return "FetchingDependencies";
    case State::kInstantiating:
      return "Instantiating";
    case State::kFinished:
      return "Finished";
  }
  NOTREACHED();
  return "";
}
#endif

void ModuleTreeLinker::AdvanceState(State new_state) {
#if DCHECK_IS_ON()
  RESOURCE_LOADING_DVLOG(1)
      << *this << "::advanceState(" << StateToString(state_) << " -> "
      << StateToString(new_state) << ")";
#endif

  switch (state_) {
    case State::kInitial:
      CHECK_EQ(num_incomplete_fetches_, 0u);
      CHECK_EQ(new_state, State::kFetchingSelf);
      break;
    case State::kFetchingSelf:
      CHECK_EQ(num_incomplete_fetches_, 0u);
      CHECK(new_state == State::kFetchingDependencies ||
            new_state == State::kFinished);
      break;
    case State::kFetchingDependencies:
      CHECK(new_state == State::kInstantiating ||
            new_state == State::kFinished);
      break;
    case State::kInstantiating:
      CHECK_EQ(new_state, State::kFinished);
      break;
    case State::kFinished:
      NOTREACHED();
      break;
  }

  state_ = new_state;

  if (state_ == State::kFinished) {
#if DCHECK_IS_ON()
    if (result_) {
      RESOURCE_LOADING_DVLOG(1)
          << *this << " finished with final result " << *result_;
    } else {
      RESOURCE_LOADING_DVLOG(1) << *this << " finished with nullptr.";
    }
#endif

    registry_->ReleaseFinishedFetcher(this);

    // <spec label="IMSGF" step="6">When the appropriate algorithm
    // asynchronously completes with final result, asynchronously complete this
    // algorithm with final result.</spec>
    client_->NotifyModuleTreeLoadFinished(result_);
  }
}

// #fetch-a-module-script-tree, #fetch-an-import()-module-script-graph, and
// #fetch-a-module-worker-script-tree.
void ModuleTreeLinker::FetchRoot(const KURL& original_url,
                                 const ScriptFetchOptions& options) {
#if DCHECK_IS_ON()
  original_url_ = original_url;
  root_is_inline_ = false;
#endif

  // https://wicg.github.io/import-maps/#wait-for-import-maps
  modulator_->ClearIsAcquiringImportMaps();

  AdvanceState(State::kFetchingSelf);

  KURL url = original_url;
  // <spec
  // href="https://github.com/drufball/layered-apis/blob/master/spec.md#fetch-a-module-script-graph"
  // step="1">Set url to the layered API fetching URL given url and the current
  // settings object's API base URL.</spec>
  if (modulator_->BuiltInModuleInfraEnabled())
    url = blink::layered_api::ResolveFetchingURL(*modulator_, url);

#if DCHECK_IS_ON()
  url_ = url;
#endif

  // <spec label="fetch-a-module-script-tree" step="2">If result is null,
  // asynchronously complete this algorithm with null, and abort these
  // steps.</spec>
  //
  // <spec label="fetch-an-import()-module-script-graph" step="4">If result is
  // null, asynchronously complete this algorithm with null, and abort these
  // steps.</spec>
  //
  // <spec label="fetch-a-module-worker-script-tree" step="3">If result is null,
  // asynchronously complete this algorithm with null, and abort these
  // steps.</spec>
  if (!url.IsValid()) {
    result_ = nullptr;
    modulator_->TaskRunner()->PostTask(
        FROM_HERE, WTF::Bind(&ModuleTreeLinker::AdvanceState,
                             WrapPersistent(this), State::kFinished));
    return;
  }

  // <spec label="fetch-a-module-script-tree" step="3">Let visited set be « url
  // ».</spec>
  //
  // <spec label="fetch-an-import()-module-script-graph" step="5">Let visited
  // set be « url ».</spec>
  //
  // <spec label="fetch-a-module-worker-script-tree" step="4">Let visited set be
  // « url ».</spec>
  visited_set_.insert(url);

  // <spec label="fetch-a-module-script-tree" step="1">Fetch a single module
  // script given url, settings object, "script", options, settings object,
  // "client", and with the top-level module fetch flag set. ...</spec>
  //
  // <spec label="fetch-an-import()-module-script-graph" step="3">Fetch a single
  // module script given url, settings object, "script", options, settings
  // object, "client", and with the top-level module fetch flag set. ...</spec>
  //
  // <spec label="fetch-a-module-worker-script-tree" step="2">Fetch a single
  // module script given url, fetch client settings object, destination,
  // options, module map settings object, "client", and with the top-level
  // module fetch flag set. ...</spec>
  ModuleScriptFetchRequest request(url, context_type_, destination_, options,
                                   Referrer::ClientReferrerString(),
                                   TextPosition::MinimumPosition());
  ++num_incomplete_fetches_;
  modulator_->FetchSingle(request, fetch_client_settings_object_fetcher_.Get(),
                          ModuleGraphLevel::kTopLevelModuleFetch,
                          custom_fetch_type_, this);
}

// <specdef
// href="https://html.spec.whatwg.org/C/#fetch-an-inline-module-script-graph">
void ModuleTreeLinker::FetchRootInline(ModuleScript* module_script) {
  DCHECK(module_script);
#if DCHECK_IS_ON()
  original_url_ = module_script->BaseURL();
  url_ = original_url_;
  root_is_inline_ = true;
#endif

  // https://wicg.github.io/import-maps/#wait-for-import-maps
  //
  // TODO(hiroshige): This should be done before |module_script| is created.
  modulator_->ClearIsAcquiringImportMaps();

  AdvanceState(State::kFetchingSelf);

  // Store the |module_script| here which will be used as result of the
  // algorithm when success. Also, this ensures that the |module_script| is
  // traced via ModuleTreeLinker.
  result_ = module_script;
  AdvanceState(State::kFetchingDependencies);

  // <spec step="3">Let visited set be an empty set.</spec>
  //
  // |visited_set_| is initialized to an empty set in ctor.

  // <spec step="4">Fetch the descendants of and instantiate script, ...</spec>
  modulator_->TaskRunner()->PostTask(
      FROM_HERE,
      WTF::Bind(&ModuleTreeLinker::FetchDescendants, WrapPersistent(this),
                WrapPersistent(module_script)));
}

// Returning from #fetch-a-single-module-script, calling from
// #fetch-a-module-script-tree, #fetch-an-import()-module-script-graph, and
// #fetch-a-module-worker-script-tree, and IMSGF.
void ModuleTreeLinker::NotifyModuleLoadFinished(ModuleScript* module_script) {
  CHECK_GT(num_incomplete_fetches_, 0u);
  --num_incomplete_fetches_;

#if DCHECK_IS_ON()
  if (module_script) {
    RESOURCE_LOADING_DVLOG(1)
        << *this << "::NotifyModuleLoadFinished() with " << *module_script;
  } else {
    RESOURCE_LOADING_DVLOG(1)
        << *this << "::NotifyModuleLoadFinished() with nullptr.";
  }
#endif

  if (state_ == State::kFetchingSelf) {
    // non-IMSGF cases: |module_script| is the top-level module, and will be
    // instantiated and returned later.
    result_ = module_script;
    AdvanceState(State::kFetchingDependencies);
  }

  if (state_ != State::kFetchingDependencies) {
    // We may reach here if one of the descendant failed to load, and the other
    // descendants fetches were in flight.
    return;
  }

  // <spec label="fetch-a-module-script-tree" step="2">If result is null,
  // asynchronously complete this algorithm with null, and abort these
  // steps.</spec>
  //
  // <spec label="fetch-an-import()-module-script-graph" step="4">If result is
  // null, asynchronously complete this algorithm with null, and abort these
  // steps.</spec>
  //
  // <spec label="fetch-a-module-worker-script-tree" step="3">If result is null,
  // asynchronously complete this algorithm with null, and abort these
  // steps.</spec>
  //
  // <spec label="IMSGF" step="4">If result is null, asynchronously complete
  // this algorithm with null, and abort these steps.</spec>
  if (!module_script) {
    result_ = nullptr;
    AdvanceState(State::kFinished);
    return;
  }

  // <spec label="fetch-a-module-script-tree" step="4">Fetch the descendants of
  // and instantiate ...</spec>
  //
  // <spec label="fetch-an-import()-module-script-graph" step="6">Fetch the
  // descendants of and instantiate result ...</spec>
  //
  // <spec label="fetch-a-module-worker-script-tree" step="5">Fetch the
  // descendants of and instantiate result given fetch client settings object,
  // ...</spec>
  //
  // <spec label="IMSGF" step="5">Fetch the descendants of result given fetch
  // client settings object, destination, and visited set.</spec>
  FetchDescendants(module_script);
}

// <specdef
// href="https://html.spec.whatwg.org/C/#fetch-the-descendants-of-a-module-script">
void ModuleTreeLinker::FetchDescendants(const ModuleScript* module_script) {
  DCHECK(module_script);

  v8::Isolate* isolate = modulator_->GetScriptState()->GetIsolate();
  v8::HandleScope scope(isolate);
  // [nospec] Abort the steps if the browsing context is discarded.
  if (!modulator_->HasValidContext()) {
    result_ = nullptr;
    AdvanceState(State::kFinished);
    return;
  }

  // <spec step="2">Let record be module script's record.</spec>
  v8::Local<v8::Module> record = module_script->V8Module();

  // <spec step="1">If module script's record is null, then asynchronously
  // complete this algorithm with module script and abort these steps.</spec>
  if (record.IsEmpty()) {
    found_parse_error_ = true;
    // We don't early-exit here and wait until all module scripts to be
    // loaded, because we might be not sure which error to be reported.
    //
    // It is possible to determine whether the error to be reported can be
    // determined without waiting for loading module scripts, and thus to
    // early-exit here if possible. However, the complexity of such early-exit
    // implementation might be high, and optimizing error cases with the
    // implementation cost might be not worth doing.
    FinalizeFetchDescendantsForOneModuleScript();
    return;
  }

  // <spec step="3">... if record.[[RequestedModules]] is empty, asynchronously
  // complete this algorithm with module script.</spec>
  //
  // Note: We defer this bail-out until the end of the procedure. The rest of
  // the procedure will be no-op anyway if record.[[RequestedModules]] is empty.

  // <spec step="4">Let urls be a new empty list.</spec>
  Vector<KURL> urls;
  Vector<TextPosition> positions;

  // <spec step="5">For each string requested of
  // record.[[RequestedModules]],</spec>
  Vector<Modulator::ModuleRequest> module_requests =
      modulator_->ModuleRequestsFromModuleRecord(record);

  for (const auto& module_request : module_requests) {
    // <spec step="5.1">Let url be the result of resolving a module specifier
    // given module script's base URL and requested.</spec>
    KURL url = module_script->ResolveModuleSpecifier(module_request.specifier);

    // <spec step="5.2">Assert: url is never failure, because resolving a module
    // specifier must have been previously successful with these same two
    // arguments.</spec>
    CHECK(url.IsValid()) << "ModuleScript::ResolveModuleSpecifier() impl must "
                            "return a valid url.";

    // <spec step="5.3">If visited set does not contain url, then:</spec>
    if (!visited_set_.Contains(url)) {
      // <spec step="5.3.1">Append url to urls.</spec>
      urls.push_back(url);

      // <spec step="5.3.2">Append url to visited set.</spec>
      visited_set_.insert(url);

      positions.push_back(module_request.position);
    }
  }

  if (urls.IsEmpty()) {
    // <spec step="3">... if record.[[RequestedModules]] is empty,
    // asynchronously complete this algorithm with module script.</spec>
    //
    // Also, if record.[[RequestedModules]] is not empty but |urls| is
    // empty here, we complete this algorithm.
    FinalizeFetchDescendantsForOneModuleScript();
    return;
  }

  // <spec step="6">Let options be the descendant script fetch options for
  // module script's fetch options.</spec>
  //
  // <spec
  // href="https://html.spec.whatwg.org/C/#descendant-script-fetch-options"> For
  // any given script fetch options options, the descendant script fetch options
  // are a new script fetch options whose items all have the same values, except
  // for the integrity metadata, which is instead the empty string.</spec>
  //
  // TODO(domfarolino): It has not yet been decided how a root module script's
  // "importance" mode should trickle down to imports. There is discussion of
  // this at https://github.com/whatwg/html/issues/3670, but for now, descendant
  // scripts get "auto" importance (Also see https://crbug.com/821464).
  ScriptFetchOptions options(module_script->FetchOptions().Nonce(),
                             IntegrityMetadataSet(), String(),
                             module_script->FetchOptions().ParserState(),
                             module_script->FetchOptions().CredentialsMode(),
                             module_script->FetchOptions().GetReferrerPolicy(),
                             mojom::FetchImportanceMode::kImportanceAuto);

  // <spec step="8">For each url in urls, ...</spec>
  //
  // <spec step="8">... These invocations of the internal module script graph
  // fetching procedure should be performed in parallel to each other.
  // ...</spec>
  for (wtf_size_t i = 0; i < urls.size(); ++i) {
    // <spec step="8">... perform the internal module script graph fetching
    // procedure given url, fetch client settings object, destination, options,
    // module script's settings object, visited set, and module script's base
    // URL. ...</spec>
    ModuleScriptFetchRequest request(
        urls[i], context_type_, destination_, options,
        module_script->BaseURL().GetString(), positions[i]);

    // <spec label="IMSGF" step="1">Assert: visited set contains url.</spec>
    DCHECK(visited_set_.Contains(request.Url()));

    ++num_incomplete_fetches_;

    // <spec label="IMSGF" step="2">Fetch a single module script given url,
    // fetch client settings object, destination, options, module map settings
    // object, referrer, and with the top-level module fetch flag unset.
    // ...</spec>
    modulator_->FetchSingle(
        request, fetch_client_settings_object_fetcher_.Get(),
        ModuleGraphLevel::kDependentModuleFetch, custom_fetch_type_, this);
  }

  // Asynchronously continue processing after NotifyModuleLoadFinished() is
  // called num_incomplete_fetches_ times.
  CHECK_GT(num_incomplete_fetches_, 0u);
}

void ModuleTreeLinker::FinalizeFetchDescendantsForOneModuleScript() {
  // [FD] of a single module script is completed here:
  //
  // <spec step="8">... Otherwise, wait until all of the internal module script
  // graph fetching procedure invocations have asynchronously completed.
  // ...</spec>

  // And, if |num_incomplete_fetches_| is 0, all the invocations of
  // #fetch-the-descendants-of-a-module-script is completed here and we proceed
  // to #fetch-the-descendants-of-and-instantiate-a-module-script Step 3
  // implemented by Instantiate().
  if (num_incomplete_fetches_ == 0)
    Instantiate();
}

// <specdef
// href="https://html.spec.whatwg.org/C/#fetch-the-descendants-of-and-instantiate-a-module-script">
void ModuleTreeLinker::Instantiate() {
  // [nospec] Abort the steps if the browsing context is discarded.
  if (!modulator_->HasValidContext()) {
    result_ = nullptr;
    AdvanceState(State::kFinished);
    return;
  }

  // <spec step="3">If result is null, then asynchronously complete this
  // algorithm with result.</spec>
  if (!result_) {
    AdvanceState(State::kFinished);
    return;
  }

  // <spec step="5">If parse error is null, then:</spec>
  //
  // [Optimization] If |found_parse_error_| is false (i.e. no parse errors
  // were found during fetching), we are sure that |parse error| is null and
  // thus skip FindFirstParseError() call.
  if (!found_parse_error_) {
#if DCHECK_IS_ON()
    HeapHashSet<Member<const ModuleScript>> discovered_set;
    DCHECK(FindFirstParseError(result_, &discovered_set).IsEmpty());
#endif

    // <spec step="5.1">Let record be result's record.</spec>
    v8::Local<v8::Module> record = result_->V8Module();

    // <spec step="5.2">Perform record.Instantiate(). ...</spec>
    AdvanceState(State::kInstantiating);
    ScriptValue instantiation_error =
        modulator_->InstantiateModule(record, result_->SourceURL());

    // <spec step="5.2">... If this throws an exception, set result's error to
    // rethrow to that exception.</spec>
    if (!instantiation_error.IsEmpty())
      result_->SetErrorToRethrow(instantiation_error);
  } else {
    // <spec step="6">Otherwise, ...</spec>

    // <spec
    // href="https://html.spec.whatwg.org/C/#finding-the-first-parse-error"
    // step="2">If discoveredSet was not given, let it be an empty set.</spec>
    HeapHashSet<Member<const ModuleScript>> discovered_set;

    // <spec step="4">Let parse error be the result of finding the first parse
    // error given result.</spec>
    ScriptValue parse_error = FindFirstParseError(result_, &discovered_set);
    DCHECK(!parse_error.IsEmpty());

    // <spec step="6">... set result's error to rethrow to parse error.</spec>
    result_->SetErrorToRethrow(parse_error);
  }

  // <spec step="7">Asynchronously complete this algorithm with result.</spec>
  AdvanceState(State::kFinished);
}

// <specdef
// href="https://html.spec.whatwg.org/C/#finding-the-first-parse-error">
// This returns non-empty ScriptValue iff a parse error is found.
ScriptValue ModuleTreeLinker::FindFirstParseError(
    const ModuleScript* module_script,
    HeapHashSet<Member<const ModuleScript>>* discovered_set) const {
  // FindFirstParseError() is called only when there is no fetch errors, i.e.
  // all module scripts in the graph are non-null.
  DCHECK(module_script);

  // <spec step="1">Let moduleMap be moduleScript's settings object's module
  // map.</spec>
  //
  // This is accessed via |modulator_|.

  // [FFPE] Step 2 is done before calling this in Instantiate().

  // <spec step="3">Append moduleScript to discoveredSet.</spec>
  discovered_set->insert(module_script);

  // <spec step="4">If moduleScript's record is null, then return moduleScript's
  // parse error.</spec>
  v8::Local<v8::Module> record = module_script->V8Module();
  if (record.IsEmpty())
    return module_script->CreateParseError();

  // <spec step="5.1">Let childSpecifiers be the value of moduleScript's
  // record's [[RequestedModules]] internal slot.</spec>
  Vector<Modulator::ModuleRequest> child_specifiers =
      modulator_->ModuleRequestsFromModuleRecord(record);

  for (const auto& module_request : child_specifiers) {
    // <spec step="5.2">Let childURLs be the list obtained by calling resolve a
    // module specifier once for each item of childSpecifiers, given
    // moduleScript's base URL and that item. ...</spec>
    KURL child_url =
        module_script->ResolveModuleSpecifier(module_request.specifier);

    // <spec step="5.2">... (None of these will ever fail, as otherwise
    // moduleScript would have been marked as itself having a parse
    // error.)</spec>
    CHECK(child_url.IsValid())
        << "ModuleScript::ResolveModuleSpecifier() impl must "
           "return a valid url.";

    // <spec step="5.3">Let childModules be the list obtained by getting each
    // value in moduleMap whose key is given by an item of childURLs.</spec>
    //
    // <spec step="5.4">For each childModule of childModules:</spec>
    const ModuleScript* child_module =
        modulator_->GetFetchedModuleScript(child_url);

    // <spec step="5.4.1">Assert: childModule is a module script (i.e., it is
    // not "fetching" or null); ...</spec>
    CHECK(child_module);

    // <spec step="5.4.2">If discoveredSet already contains childModule,
    // continue.</spec>
    if (discovered_set->Contains(child_module))
      continue;

    // <spec step="5.4.3">Let childParseError be the result of finding the first
    // parse error given childModule and discoveredSet.</spec>
    ScriptValue child_parse_error =
        FindFirstParseError(child_module, discovered_set);

    // <spec step="5.4.4">If childParseError is not null, return
    // childParseError.</spec>
    if (!child_parse_error.IsEmpty())
      return child_parse_error;
  }

  // <spec step="6">Return null.</spec>
  return ScriptValue();
}

#if DCHECK_IS_ON()
std::ostream& operator<<(std::ostream& stream, const ModuleTreeLinker& linker) {
  stream << "ModuleTreeLinker[" << &linker
         << ", original_url=" << linker.original_url_.GetString()
         << ", url=" << linker.url_.GetString()
         << ", inline=" << linker.root_is_inline_ << "]";
  return stream;
}
#endif

}  // namespace blink
