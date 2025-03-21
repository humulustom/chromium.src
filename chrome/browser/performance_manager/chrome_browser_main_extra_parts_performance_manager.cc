// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/performance_manager/chrome_browser_main_extra_parts_performance_manager.h"

#include <utility>

#include "base/bind.h"
#include "base/feature_list.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/performance_manager/browser_child_process_watcher.h"
#include "chrome/browser/performance_manager/decorators/frozen_frame_aggregator.h"
#include "chrome/browser/performance_manager/decorators/helpers/page_live_state_decorator_helper.h"
#include "chrome/browser/performance_manager/decorators/page_aggregator.h"
#include "chrome/browser/performance_manager/decorators/process_metrics_decorator.h"
#include "chrome/browser/performance_manager/graph/policies/policy_features.h"
#include "chrome/browser/performance_manager/graph/policies/urgent_page_discarding_policy.h"
#include "chrome/browser/performance_manager/graph/policies/working_set_trimmer_policy.h"
#include "chrome/browser/performance_manager/observers/isolation_context_metrics.h"
#include "chrome/browser/performance_manager/observers/metrics_collector.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "components/performance_manager/embedder/performance_manager_lifetime.h"
#include "components/performance_manager/embedder/performance_manager_registry.h"
#include "components/performance_manager/performance_manager_lock_observer.h"
#include "components/performance_manager/public/graph/graph.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_features.h"

#if defined(OS_LINUX)
#include "base/allocator/buildflags.h"
#if BUILDFLAG(USE_TCMALLOC)
#include "chrome/browser/performance_manager/graph/policies/dynamic_tcmalloc_policy_linux.h"
#include "chrome/common/performance_manager/mojom/tcmalloc.mojom.h"
#endif  // BUILDFLAG(USE_TCMALLOC)
#endif  // defined(OS_LINUX)

#if !defined(OS_ANDROID)
#include "chrome/browser/tab_contents/form_interaction_tab_helper.h"
#endif  // !defined(OS_ANDROID)

namespace {
ChromeBrowserMainExtraPartsPerformanceManager* g_instance = nullptr;
}

ChromeBrowserMainExtraPartsPerformanceManager::
    ChromeBrowserMainExtraPartsPerformanceManager()
    : lock_observer_(std::make_unique<
                     performance_manager::PerformanceManagerLockObserver>()) {
  DCHECK(!g_instance);
  g_instance = this;
}

ChromeBrowserMainExtraPartsPerformanceManager::
    ~ChromeBrowserMainExtraPartsPerformanceManager() {
  DCHECK_EQ(this, g_instance);
  g_instance = nullptr;
}

// static
ChromeBrowserMainExtraPartsPerformanceManager*
ChromeBrowserMainExtraPartsPerformanceManager::GetInstance() {
  return g_instance;
}

// static
void ChromeBrowserMainExtraPartsPerformanceManager::CreatePoliciesAndDecorators(
    performance_manager::Graph* graph) {
  graph->PassToGraph(std::make_unique<performance_manager::PageAggregator>());
  graph->PassToGraph(
      std::make_unique<performance_manager::FrozenFrameAggregator>());
  graph->PassToGraph(
      std::make_unique<performance_manager::IsolationContextMetrics>());
  graph->PassToGraph(std::make_unique<performance_manager::MetricsCollector>());
  graph->PassToGraph(
      std::make_unique<performance_manager::ProcessMetricsDecorator>());

  if (performance_manager::policies::WorkingSetTrimmerPolicy::
          PlatformSupportsWorkingSetTrim()) {
    graph->PassToGraph(performance_manager::policies::WorkingSetTrimmerPolicy::
                           CreatePolicyForPlatform());
  }

#if defined(OS_LINUX)
#if BUILDFLAG(USE_TCMALLOC)
  if (base::FeatureList::IsEnabled(
          performance_manager::features::kDynamicTcmallocTuning)) {
    graph->PassToGraph(std::make_unique<
                       performance_manager::policies::DynamicTcmallocPolicy>());
  }
#endif  // BUILDFLAG(USE_TCMALLOC)
#endif  // defined(OS_LINUX)

#if !defined(OS_ANDROID)
  graph->PassToGraph(FormInteractionTabHelper::CreateGraphObserver());

  if (base::FeatureList::IsEnabled(
          performance_manager::features::
              kUrgentDiscardingFromPerformanceManager)) {
    graph->PassToGraph(
        std::make_unique<
            performance_manager::policies::UrgentPageDiscardingPolicy>());
  }
#endif  // !defined(OS_ANDROID)
}

content::LockObserver*
ChromeBrowserMainExtraPartsPerformanceManager::GetLockObserver() {
  return lock_observer_.get();
}

void ChromeBrowserMainExtraPartsPerformanceManager::PostCreateThreads() {
  performance_manager_ =
      performance_manager::CreatePerformanceManagerWithDefaultDecorators(
          base::BindOnce(&ChromeBrowserMainExtraPartsPerformanceManager::
                             CreatePoliciesAndDecorators));
  registry_ = performance_manager::PerformanceManagerRegistry::Create();
  browser_child_process_watcher_ =
      std::make_unique<performance_manager::BrowserChildProcessWatcher>();
  browser_child_process_watcher_->Initialize();

  // There are no existing loaded profiles.
  DCHECK(g_browser_process->profile_manager()->GetLoadedProfiles().empty());

  g_browser_process->profile_manager()->AddObserver(this);

  page_live_state_data_helper_ =
      std::make_unique<performance_manager::PageLiveStateDecoratorHelper>();
}

void ChromeBrowserMainExtraPartsPerformanceManager::PostMainMessageLoopRun() {
  // Release all graph nodes before destroying the performance manager.
  // First release the browser and GPU process nodes.
  browser_child_process_watcher_->TearDown();
  browser_child_process_watcher_.reset();

  g_browser_process->profile_manager()->RemoveObserver(this);
  observed_profiles_.RemoveAll();

  page_live_state_data_helper_.reset();

  // There may still be WebContents and RenderProcessHosts with attached user
  // data, retaining PageNodes, FrameNodes and ProcessNodes. Tear down the
  // registry to release these nodes. There is no convenient later call-out to
  // destroy the performance manager after all WebContents and
  // RenderProcessHosts have been destroyed.
  registry_->TearDown();
  registry_.reset();

  performance_manager::DestroyPerformanceManager(
      std::move(performance_manager_));
}

void ChromeBrowserMainExtraPartsPerformanceManager::OnProfileAdded(
    Profile* profile) {
  observed_profiles_.Add(profile);
  registry_->NotifyBrowserContextAdded(profile);
}

void ChromeBrowserMainExtraPartsPerformanceManager::
    OnOffTheRecordProfileCreated(Profile* off_the_record) {
  OnProfileAdded(off_the_record);
}

void ChromeBrowserMainExtraPartsPerformanceManager::OnProfileWillBeDestroyed(
    Profile* profile) {
  observed_profiles_.Remove(profile);
  registry_->NotifyBrowserContextRemoved(profile);
}
