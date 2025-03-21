// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/display_lock/display_lock_context.h"

#include <string>

#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "third_party/blink/renderer/core/accessibility/ax_object_cache.h"
#include "third_party/blink/renderer/core/css/style_change_reason.h"
#include "third_party/blink/renderer/core/css/style_recalc.h"
#include "third_party/blink/renderer/core/display_lock/display_lock_utilities.h"
#include "third_party/blink/renderer/core/display_lock/render_subtree_activation_event.h"
#include "third_party/blink/renderer/core/display_lock/strict_yielding_display_lock_budget.h"
#include "third_party/blink/renderer/core/display_lock/unyielding_display_lock_budget.h"
#include "third_party/blink/renderer/core/display_lock/yielding_display_lock_budget.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/html_element_type_helpers.h"
#include "third_party/blink/renderer/core/inspector/inspector_trace_events.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/pre_paint_tree_walk.h"
#include "third_party/blink/renderer/platform/bindings/microtask.h"
#include "third_party/blink/renderer/platform/heap/heap.h"

namespace blink {

namespace {
namespace rejection_names {
const char* kExecutionContextDestroyed = "Execution context destroyed.";
const char* kContainmentNotSatisfied =
    "Containment requirement is not satisfied.";
const char* kUnsupportedDisplay =
    "Element has unsupported display type (display: contents).";
const char* kElementIsDisconnected = "Element is disconnected.";
const char* kElementIsNested = "Element is nested under a locked element.";
}  // namespace rejection_names

void RecordActivationReason(DisplayLockActivationReason reason) {
  int ordered_reason = -1;

  // IMPORTANT: This number needs to be bumped up when adding
  // new reasons.
  static const int number_of_reasons = 9;

  switch (reason) {
    case DisplayLockActivationReason::kAccessibility:
      ordered_reason = 0;
      break;
    case DisplayLockActivationReason::kFindInPage:
      ordered_reason = 1;
      break;
    case DisplayLockActivationReason::kFragmentNavigation:
      ordered_reason = 2;
      break;
    case DisplayLockActivationReason::kScriptFocus:
      ordered_reason = 3;
      break;
    case DisplayLockActivationReason::kScrollIntoView:
      ordered_reason = 4;
      break;
    case DisplayLockActivationReason::kSelection:
      ordered_reason = 5;
      break;
    case DisplayLockActivationReason::kSimulatedClick:
      ordered_reason = 6;
      break;
    case DisplayLockActivationReason::kUserFocus:
      ordered_reason = 7;
      break;
    case DisplayLockActivationReason::kViewportIntersection:
      ordered_reason = 8;
      break;
    case DisplayLockActivationReason::kViewport:
    case DisplayLockActivationReason::kAny:
      NOTREACHED();
      break;
  }
  UMA_HISTOGRAM_ENUMERATION("Blink.Render.DisplayLockActivationReason",
                            ordered_reason, number_of_reasons);
}

// Helper function that returns an immediately rejected promise.
ScriptPromise GetRejectedPromise(ScriptState* script_state,
                                 const char* rejection_reason) {
  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  auto promise = resolver->Promise();
  resolver->Reject(MakeGarbageCollected<DOMException>(
      DOMExceptionCode::kNotAllowedError, rejection_reason));
  return promise;
}

// Helper function that returns an immediately resolved promise.
ScriptPromise GetResolvedPromise(ScriptState* script_state) {
  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  auto promise = resolver->Promise();
  resolver->Resolve();
  return promise;
}

}  // namespace

DisplayLockContext::DisplayLockContext(Element* element,
                                       ExecutionContext* context)
    : ContextLifecycleObserver(context),
      element_(element),
      document_(&element_->GetDocument()),
      state_(this) {}

DisplayLockContext::~DisplayLockContext() {
  DCHECK_EQ(state_, kUnlocked);
}

void DisplayLockContext::Trace(blink::Visitor* visitor) {
  visitor->Trace(update_resolver_);
  visitor->Trace(element_);
  visitor->Trace(document_);
  visitor->Trace(whitespace_reattach_set_);
  ContextLifecycleObserver::Trace(visitor);
}

void DisplayLockContext::Dispose() {
  // Note that if we have any resolvers at dispose time, then it's too late to
  // reject the promise, since we are not allowed to create new strong
  // references to objects already set for destruction (and rejecting would do
  // this since the rejection has to be deferred). We need to detach instead.
  // TODO(vmpstr): See if there is another earlier time we can detect that we're
  // going to be disposed.
  FinishUpdateResolver(kDetach);
  state_ = kUnlocked;
}

void DisplayLockContext::ContextDestroyed(ExecutionContext*) {
  FinishUpdateResolver(kReject, rejection_names::kExecutionContextDestroyed);
  state_ = kUnlocked;
}

void DisplayLockContext::UpdateActivationObservationIfNeeded() {
  if (!document_) {
    is_observed_ = false;
    return;
  }

  // We require observation if we are viewport-activatable, and one of the
  // following is true:
  // 1. We're locked, which means that we need to know when to unlock the
  //    element
  // 2. We're activated (in the CSS version), which means that we need to know
  //    when we stop intersecting the viewport so that we can re-lock.
  bool should_observe =
      (IsLocked() || (!IsAttributeVersion(this) && IsActivated())) &&
      IsActivatable(DisplayLockActivationReason::kViewportIntersection) &&
      ConnectedToView();
  if (should_observe && !is_observed_) {
    document_->RegisterDisplayLockActivationObservation(element_);
  } else if (!should_observe && is_observed_) {
    document_->UnregisterDisplayLockActivationObservation(element_);
  }
  is_observed_ = should_observe;
}

void DisplayLockContext::SetActivatable(uint16_t activatable_mask) {
  if (IsLocked()) {
    // If we're locked, the activatable mask might change the activation
    // blocking lock count. If we're not locked, the activation blocking lock
    // count will be updated when we changed the state.
    // Note that we record this only if we're blocking all activation. That is,
    // the lock is considered activatable if any bit is set.
    state_.UpdateActivationBlockingCount(activatable_mask_, activatable_mask);
  }
  activatable_mask_ = activatable_mask;
  UpdateActivationObservationIfNeeded();
}

void DisplayLockContext::StartAcquire() {
  DCHECK(!IsLocked());
  update_budget_.reset();
  state_ = kLocked;

  // We're no longer activated, so if the signal didn't run yet, we should
  // cancel it.
  weak_factory_.InvalidateWeakPtrs();

  // If we're already connected then we need to ensure that we update our style
  // to check for containment later, layout size based on the options, and
  // also clear the painted output.
  if (!ConnectedToView())
    return;

  // There are several ways we can call StartAcquire. Most of them require us to
  // dirty style so that we can add proper containment onto the element.
  // However, if we're doing a StartAcquire from within style recalc, then we
  // don't need to do anything as we should have already added containment.
  // Moreover, dirtying self style from within style recalc is not allowed,
  // since either it has no effect and is cleaned before any work is done, or it
  // causes DHCECKs in AssertLayoutTreeUpdated().
  if (!document_->InStyleRecalc()) {
    element_->SetNeedsStyleRecalc(
        kLocalStyleChange,
        StyleChangeReasonForTracing::Create(style_change_reason::kDisplayLock));
  }

  // In either case, we schedule an animation. If we're already inside a
  // lifecycle update, this will be a no-op.
  ScheduleAnimation();

  // We need to notify the AX cache (if it exists) to update the  childrens
  // of |element_| in the AX cache.
  if (AXObjectCache* cache = element_->GetDocument().ExistingAXObjectCache())
    cache->ChildrenChanged(element_);

  auto* layout_object = element_->GetLayoutObject();
  if (!layout_object) {
    is_horizontal_writing_mode_ = true;
    return;
  }

  layout_object->SetNeedsLayoutAndPrefWidthsRecalc(
      layout_invalidation_reason::kDisplayLock);

  is_horizontal_writing_mode_ = layout_object->IsHorizontalWritingMode();
  // GraphicsLayer collection would normally skip layers if paint is blocked
  // by display-locking (see: CollectDrawableLayersForLayerListRecursively
  // in LocalFrameView). However, if we don't trigger this collection, then
  // we might use the cached result instead. In order to ensure we skip the
  // newly locked layers, we need to set |need_graphics_layer_collection_|
  // before marking the layer for repaint.
  if (!RuntimeEnabledFeatures::CompositeAfterPaintEnabled())
    needs_graphics_layer_collection_ = true;
  MarkPaintLayerNeedsRepaint();
}

ScriptPromise DisplayLockContext::UpdateRendering(ScriptState* script_state) {
  // Immediately resolve if we're unlocked or disconnected.
  if (state_ == kUnlocked || !ConnectedToView())
    return GetResolvedPromise(script_state);

  // If we have a resolver, then we're at least updating already, just return
  // the same promise.
  if (update_resolver_) {
    DCHECK(state_ == kUpdating || state_ == kCommitting) << state_;
    return update_resolver_->Promise();
  }

  if (DisplayLockUtilities::NearestLockedExclusiveAncestor(*element_)) {
    return GetRejectedPromise(script_state, rejection_names::kElementIsNested);
  }

  MakeResolver(script_state, &update_resolver_);
  StartUpdateIfNeeded();
  return update_resolver_->Promise();
}

bool DisplayLockContext::CleanupAndRejectCommitIfNotConnected() {
  // If we're not connected, then the process of committing is the same as just
  // unlocking the element. Early out if this conditions *doesn't* hold.
  if (ConnectedToView())
    return false;

  state_ = kUnlocked;
  update_budget_.reset();
  // Note that we reject the update, but resolve the commit.
  FinishUpdateResolver(kReject, rejection_names::kElementIsDisconnected);
  return true;
}

void DisplayLockContext::MakeResolver(ScriptState* script_state,
                                      Member<ScriptPromiseResolver>* resolver) {
  DCHECK(ConnectedToView());
  document_->View()->RegisterForLifecycleNotifications(this);
  *resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
}

bool DisplayLockContext::HasResolver() {
  return update_resolver_;
}

void DisplayLockContext::FinishUpdateResolver(ResolverState state,
                                              const char* rejection_reason) {
  FinishResolver(&update_resolver_, state, rejection_reason);
}

void DisplayLockContext::FinishResolver(Member<ScriptPromiseResolver>* resolver,
                                        ResolverState state,
                                        const char* rejection_reason) {
  if (!*resolver)
    return;
  switch (state) {
    case kResolve:
      // In order to avoid script doing work as a part of the lifecycle update,
      // we delay the resolution to be in a task.
      GetExecutionContext()
          ->GetTaskRunner(TaskType::kMiscPlatformAPI)
          ->PostTask(FROM_HERE, WTF::Bind(
                                    +[](ScriptPromiseResolver* resolver) {
                                      resolver->Resolve();
                                    },
                                    WrapPersistent(resolver->Get())));
      break;
    case kReject:
      DCHECK(rejection_reason);
      (*resolver)->Reject(MakeGarbageCollected<DOMException>(
          DOMExceptionCode::kNotAllowedError, rejection_reason));
      break;
    case kDetach:
      (*resolver)->Detach();
      break;
  }
  *resolver = nullptr;
  if (!HasResolver() && ConnectedToView())
    document_->View()->UnregisterFromLifecycleNotifications(this);
}

bool DisplayLockContext::ShouldPerformUpdatePhase(
    DisplayLockBudget::Phase phase) const {
  DCHECK(document_);
  if (state_ != kUpdating)
    return false;
  auto* view = document_->View();
  return view && view->InLifecycleUpdate() &&
         update_budget_->ShouldPerformPhase(phase,
                                            view->CurrentLifecycleData());
}

bool DisplayLockContext::ShouldStyle(DisplayLockLifecycleTarget target) const {
  return target == DisplayLockLifecycleTarget::kSelf || update_forced_ ||
         state_ > kUpdating ||
         ShouldPerformUpdatePhase(DisplayLockBudget::Phase::kStyle);
}

void DisplayLockContext::DidStyle(DisplayLockLifecycleTarget target) {
  if (state_ == kUnlocked) {
    // If we're committing without finishing the acquire() first, it's possible
    // for the state to be kUnlocked instead of kCommitting. We should still
    // mark child reattachment & whitespace reattachment in that case.
    MarkElementsForWhitespaceReattachment();
    if (element_->ChildNeedsReattachLayoutTree())
      element_->MarkAncestorsWithChildNeedsReattachLayoutTree();
    return;
  }

  if (target == DisplayLockLifecycleTarget::kSelf) {
    if (ForceUnlockIfNeeded())
      return;

    if (blocked_style_traversal_type_ == kStyleUpdateSelf)
      blocked_style_traversal_type_ = kStyleUpdateNotRequired;
    auto* layout_object = element_->GetLayoutObject();
    is_horizontal_writing_mode_ =
        !layout_object || layout_object->IsHorizontalWritingMode();
    return;
  }

  if (state_ != kCommitting && state_ != kUpdating && !update_forced_)
    return;

  if (element_->ChildNeedsReattachLayoutTree())
    element_->MarkAncestorsWithChildNeedsReattachLayoutTree();

  blocked_style_traversal_type_ = kStyleUpdateNotRequired;

  MarkElementsForWhitespaceReattachment();

  if (state_ == kUpdating)
    update_budget_->DidPerformPhase(DisplayLockBudget::Phase::kStyle);
}

bool DisplayLockContext::ShouldLayout(DisplayLockLifecycleTarget target) const {
  return target == DisplayLockLifecycleTarget::kSelf || update_forced_ ||
         state_ > kUpdating ||
         ShouldPerformUpdatePhase(DisplayLockBudget::Phase::kLayout);
}

void DisplayLockContext::DidLayout(DisplayLockLifecycleTarget target) {
  if (target == DisplayLockLifecycleTarget::kSelf)
    return;

  // Since we did layout on children already, we'll clear this.
  child_layout_was_blocked_ = false;
  if (state_ == kUpdating)
    update_budget_->DidPerformPhase(DisplayLockBudget::Phase::kLayout);
}

bool DisplayLockContext::ShouldPrePaint(
    DisplayLockLifecycleTarget target) const {
  return target == DisplayLockLifecycleTarget::kSelf || update_forced_ ||
         state_ > kUpdating ||
         ShouldPerformUpdatePhase(DisplayLockBudget::Phase::kPrePaint);
}

void DisplayLockContext::DidPrePaint(DisplayLockLifecycleTarget target) {
  if (target == DisplayLockLifecycleTarget::kSelf)
    return;

  if (state_ == kUpdating)
    update_budget_->DidPerformPhase(DisplayLockBudget::Phase::kPrePaint);

#if DCHECK_IS_ON()
  if (state_ == kUpdating || state_ == kCommitting) {
    // Since we should be under containment, we should have a layer. If we
    // don't, then paint might not happen and we'll never resolve.
    DCHECK(element_->GetLayoutObject()->HasLayer());
  }
#endif
}

bool DisplayLockContext::ShouldPaint(DisplayLockLifecycleTarget target) const {
  // Note that forced updates should never require us to paint, so we don't
  // check |update_forced_| here. In other words, although |update_forced_|
  // could be true here, we still should not paint. This also holds for
  // kUpdating state, since updates should not paint.
  return target == DisplayLockLifecycleTarget::kSelf || state_ == kCommitting ||
         state_ == kUnlocked;
}

void DisplayLockContext::DidPaint(DisplayLockLifecycleTarget) {
  // This is here for symmetry, but could be removed if necessary.
}

bool DisplayLockContext::IsActivatable(
    DisplayLockActivationReason reason) const {
  return activatable_mask_ & static_cast<uint16_t>(reason);
}

void DisplayLockContext::FireActivationEvent(Element* activated_element) {
  element_->DispatchEvent(
      *MakeGarbageCollected<RenderSubtreeActivationEvent>(*activated_element));
}

void DisplayLockContext::CommitForActivationWithSignal(
    Element* activated_element,
    DisplayLockActivationReason reason_for_metrics) {
  DCHECK(activated_element);
  DCHECK(element_);
  DCHECK(ConnectedToView());
  DCHECK(IsLocked());
  DCHECK(ShouldCommitForActivation(DisplayLockActivationReason::kAny));

  document_->EnqueueDisplayLockActivationTask(
      WTF::Bind(&DisplayLockContext::FireActivationEvent,
                weak_factory_.GetWeakPtr(), WrapPersistent(activated_element)));

  StartCommit();

  RecordActivationReason(reason_for_metrics);
  if (reason_for_metrics == DisplayLockActivationReason::kFindInPage)
    document_->MarkHasFindInPageRenderSubtreeActiveMatch();

  if (!IsAttributeVersion(this)) {
    css_is_activated_ = true;
    // Since size containment depends on the activatability state, we should
    // invalidate the style for this element, so that the style adjuster can
    // properly remove the containment.
    element_->SetNeedsStyleRecalc(
        kLocalStyleChange,
        StyleChangeReasonForTracing::Create(style_change_reason::kDisplayLock));
  }

  // Since setting the attribute might trigger a commit if we are still locked,
  // we set it after we start the commit.
  if (element_->FastHasAttribute(html_names::kRendersubtreeAttr))
    element_->setAttribute(html_names::kRendersubtreeAttr, "");
}

bool DisplayLockContext::IsActivated() const {
  DCHECK(!IsAttributeVersion(this));
  return css_is_activated_;
}

void DisplayLockContext::ClearActivated() {
  css_is_activated_ = false;
}

bool DisplayLockContext::ShouldCommitForActivation(
    DisplayLockActivationReason reason) const {
  return IsActivatable(reason) && IsLocked();
}

void DisplayLockContext::DidAttachLayoutTree() {
  if (state_ >= kUnlocked)
    return;

  if (auto* layout_object = element_->GetLayoutObject())
    is_horizontal_writing_mode_ = layout_object->IsHorizontalWritingMode();
}

DisplayLockContext::ScopedForcedUpdate
DisplayLockContext::GetScopedForcedUpdate() {
  if (state_ >= kCommitting)
    return ScopedForcedUpdate(nullptr);

  DCHECK(!update_forced_);
  update_forced_ = true;
  TRACE_EVENT_ASYNC_BEGIN0(
      TRACE_DISABLED_BY_DEFAULT("blink.debug.display_lock"), "LockForced",
      TRACE_ID_LOCAL(this));

  // Now that the update is forced, we should ensure that style layout, and
  // prepaint code can reach it via dirty bits. Note that paint isn't a part of
  // this, since |update_forced_| doesn't force paint to happen. See
  // ShouldPaint().
  MarkForStyleRecalcIfNeeded();
  MarkForLayoutIfNeeded();
  MarkAncestorsForPrePaintIfNeeded();
  return ScopedForcedUpdate(this);
}

void DisplayLockContext::NotifyForcedUpdateScopeEnded() {
  DCHECK(update_forced_);
  update_forced_ = false;
  TRACE_EVENT_ASYNC_END0(TRACE_DISABLED_BY_DEFAULT("blink.debug.display_lock"),
                         "LockForced", TRACE_ID_LOCAL(this));
}

void DisplayLockContext::StartCommit() {
  DCHECK(IsLocked());
  if (CleanupAndRejectCommitIfNotConnected())
    return;

  if (state_ != kUpdating)
    ScheduleAnimation();

  // We might already be unlocked due to above, but we should still mark
  // ancestor chains for updates below.
  if (state_ < kCommitting)
    state_ = kCommitting;

  update_budget_.reset();

  // We skip updating the style dirtiness if we're within style recalc. This is
  // instead handled by a call to AdjustStyleRecalcChangeForChildren().
  if (!document_->InStyleRecalc()) {
    // We're committing without a budget, so ensure we can reach style.
    MarkForStyleRecalcIfNeeded();
  }

  // We also need to notify the AX cache (if it exists) to update the childrens
  // of |element_| in the AX cache.
  if (AXObjectCache* cache = element_->GetDocument().ExistingAXObjectCache())
    cache->ChildrenChanged(element_);

  auto* layout_object = element_->GetLayoutObject();
  // We might commit without connecting, so there is no layout object yet.
  if (!layout_object)
    return;

  // Now that we know we have a layout object, we should ensure that we can
  // reach the rest of the phases as well.
  MarkForLayoutIfNeeded();
  MarkAncestorsForPrePaintIfNeeded();
  MarkPaintLayerNeedsRepaint();

  layout_object->SetNeedsLayoutAndPrefWidthsRecalc(
      layout_invalidation_reason::kDisplayLock);

  if (auto* view = layout_object->GetFrameView())
    view->SetNeedsForcedResizeObservations();
}

void DisplayLockContext::StartUpdateIfNeeded() {
  // We should not be calling this if we're unlocked.
  DCHECK_NE(state_, kUnlocked);
  // Any state other than kLocked means that we are already in the process of
  // updating/committing, so we can piggy back on that process without kicking
  // off any new updates.
  if (state_ != kLocked)
    return;

  // We don't need to mark anything dirty since the budget will take care of
  // that for us.
  update_budget_ = CreateNewBudget();
  state_ = kUpdating;
  ScheduleAnimation();
}

std::unique_ptr<DisplayLockBudget> DisplayLockContext::CreateNewBudget() {
  switch (BudgetType::kDefault) {
    case BudgetType::kDoNotYield:
      return base::WrapUnique(new UnyieldingDisplayLockBudget(this));
    case BudgetType::kStrictYieldBetweenLifecyclePhases:
      return base::WrapUnique(new StrictYieldingDisplayLockBudget(this));
    case BudgetType::kYieldBetweenLifecyclePhases:
      return base::WrapUnique(new YieldingDisplayLockBudget(this));
  }
  NOTREACHED();
  return nullptr;
}

void DisplayLockContext::AddToWhitespaceReattachSet(Element& element) {
  whitespace_reattach_set_.insert(&element);
}

void DisplayLockContext::MarkElementsForWhitespaceReattachment() {
  for (auto element : whitespace_reattach_set_) {
    if (!element || element->NeedsReattachLayoutTree() ||
        !element->GetLayoutObject())
      continue;

    if (Node* first_child = LayoutTreeBuilderTraversal::FirstChild(*element))
      first_child->MarkAncestorsWithChildNeedsReattachLayoutTree();
  }
  whitespace_reattach_set_.clear();
}

StyleRecalcChange DisplayLockContext::AdjustStyleRecalcChangeForChildren(
    StyleRecalcChange change) {
  // This code is similar to MarkForStyleRecalcIfNeeded, except that it acts on
  // |change| and not on |element_|. This is only called during style recalc.
  // Note that since we're already in self style recalc, this code is shorter
  // since it doesn't have to deal with dirtying self-style.
  DCHECK(document_->InStyleRecalc());
  DCHECK(!IsAttributeVersion(this));

  if (reattach_layout_tree_was_blocked_) {
    change = change.ForceReattachLayoutTree();
    reattach_layout_tree_was_blocked_ = false;
  }

  if (blocked_style_traversal_type_ == kStyleUpdateDescendants)
    change = change.ForceRecalcDescendants();
  else if (blocked_style_traversal_type_ == kStyleUpdateChildren)
    change = change.EnsureAtLeast(StyleRecalcChange::kRecalcChildren);
  blocked_style_traversal_type_ = kStyleUpdateNotRequired;
  return change;
}

bool DisplayLockContext::MarkForStyleRecalcIfNeeded() {
  if (reattach_layout_tree_was_blocked_) {
    // We previously blocked a layout tree reattachment on |element_|'s
    // descendants, so we should mark it for layout tree reattachment now.
    element_->SetForceReattachLayoutTree();
    reattach_layout_tree_was_blocked_ = false;
  }
  if (IsElementDirtyForStyleRecalc()) {
    if (blocked_style_traversal_type_ > kStyleUpdateNotRequired) {
      // We blocked a traversal going to the element previously.
      // Make sure we will traverse this element and maybe its subtree if we
      // previously blocked a style traversal that should've done that.
      element_->SetNeedsStyleRecalc(
          blocked_style_traversal_type_ == kStyleUpdateDescendants
              ? kSubtreeStyleChange
              : kLocalStyleChange,
          StyleChangeReasonForTracing::Create(
              style_change_reason::kDisplayLock));
      if (blocked_style_traversal_type_ == kStyleUpdateChildren)
        element_->SetChildNeedsStyleRecalc();
      blocked_style_traversal_type_ = kStyleUpdateNotRequired;
    } else if (element_->ChildNeedsReattachLayoutTree()) {
      // Mark |element_| as style dirty, as we can't mark for child reattachment
      // before style.
      element_->SetNeedsStyleRecalc(kLocalStyleChange,
                                    StyleChangeReasonForTracing::Create(
                                        style_change_reason::kDisplayLock));
    }
    // Propagate to the ancestors, since the dirty bit in a locked subtree is
    // stopped at the locked ancestor.
    // See comment in IsElementDirtyForStyleRecalc.
    element_->MarkAncestorsWithChildNeedsStyleRecalc();
    return true;
  }
  return false;
}

bool DisplayLockContext::MarkForLayoutIfNeeded() {
  if (IsElementDirtyForLayout()) {
    // Forces the marking of ancestors to happen, even if
    // |DisplayLockContext::ShouldLayout()| returns false.
    base::AutoReset<bool> scoped_force(&update_forced_, true);
    if (child_layout_was_blocked_) {
      // We've previously blocked a child traversal when doing self-layout for
      // the locked element, so we're marking it with child-needs-layout so that
      // it will traverse to the locked element and do the child traversal
      // again. We don't need to mark it for self-layout (by calling
      // |LayoutObject::SetNeedsLayout()|) because the locked element itself
      // doesn't need to relayout.
      element_->GetLayoutObject()->SetChildNeedsLayout();
      child_layout_was_blocked_ = false;
    } else {
      // Since the dirty layout propagation stops at the locked element, we need
      // to mark its ancestors as dirty here so that it will be traversed to on
      // the next layout.
      element_->GetLayoutObject()->MarkContainerChainForLayout();
    }
    return true;
  }
  return false;
}

bool DisplayLockContext::MarkAncestorsForPrePaintIfNeeded() {
  // TODO(vmpstr): We should add a compositing phase for proper bookkeeping.
  bool compositing_dirtied = MarkForCompositingUpdatesIfNeeded();

  if (IsElementDirtyForPrePaint()) {
    auto* layout_object = element_->GetLayoutObject();
    if (auto* parent = layout_object->Parent())
      parent->SetSubtreeShouldCheckForPaintInvalidation();

    // Note that if either we or our descendants are marked as needing this
    // update, then ensure to mark self as needing the update. This sets up the
    // correct flags for PrePaint to recompute the necessary values and
    // propagate the information into the subtree.
    if (needs_effective_allowed_touch_action_update_ ||
        layout_object->EffectiveAllowedTouchActionChanged() ||
        layout_object->DescendantEffectiveAllowedTouchActionChanged()) {
      // Note that although the object itself should have up to date value, in
      // order to force recalc of the whole subtree, we mark it as needing an
      // update.
      layout_object->MarkEffectiveAllowedTouchActionChanged();
    }
    return true;
  }
  return compositing_dirtied;
}

bool DisplayLockContext::MarkPaintLayerNeedsRepaint() {
  DCHECK(ConnectedToView());
  if (auto* layout_object = element_->GetLayoutObject()) {
    layout_object->PaintingLayer()->SetNeedsRepaint();
    if (!RuntimeEnabledFeatures::CompositeAfterPaintEnabled() &&
        needs_graphics_layer_collection_) {
      document_->View()->SetForeignLayerListNeedsUpdate();
      needs_graphics_layer_collection_ = false;
    }
    return true;
  }
  return false;
}

bool DisplayLockContext::MarkForCompositingUpdatesIfNeeded() {
  if (!ConnectedToView())
    return false;

  auto* layout_object = element_->GetLayoutObject();
  if (!layout_object)
    return false;

  auto* layout_box = DynamicTo<LayoutBoxModelObject>(layout_object);
  if (layout_box && layout_box->HasSelfPaintingLayer()) {
    if (layout_box->Layer()->ChildNeedsCompositingInputsUpdate() &&
        layout_box->Layer()->Parent()) {
      // Note that if the layer's child needs compositing inputs update, then
      // that layer itself also needs compositing inputs update. In order to
      // propagate the dirty bit, we need to mark this layer's _parent_ as a
      // needing an update.
      layout_box->Layer()->Parent()->SetNeedsCompositingInputsUpdate();
    }
    if (needs_compositing_requirements_update_)
      layout_box->Layer()->SetNeedsCompositingRequirementsUpdate();
    needs_compositing_requirements_update_ = false;
    return true;
  }
  return false;
}

bool DisplayLockContext::IsElementDirtyForStyleRecalc() const {
  // The |element_| checks could be true even if |blocked_style_traversal_type_|
  // is not required. The reason for this is that the
  // blocked_style_traversal_type_ is set during the style walk that this
  // display lock blocked. However, we could dirty element style and commit
  // before ever having gone through the style calc that would have been
  // blocked, meaning we never blocked style during a walk. Instead we might
  // have not propagated the dirty bits up the tree.
  return element_->NeedsStyleRecalc() || element_->ChildNeedsStyleRecalc() ||
         element_->ChildNeedsReattachLayoutTree() ||
         blocked_style_traversal_type_ > kStyleUpdateNotRequired;
}

bool DisplayLockContext::IsElementDirtyForLayout() const {
  if (auto* layout_object = element_->GetLayoutObject())
    return layout_object->NeedsLayout() || child_layout_was_blocked_;
  return false;
}

bool DisplayLockContext::IsElementDirtyForPrePaint() const {
  if (auto* layout_object = element_->GetLayoutObject()) {
    auto* layout_box = DynamicTo<LayoutBoxModelObject>(layout_object);
    return PrePaintTreeWalk::ObjectRequiresPrePaint(*layout_object) ||
           PrePaintTreeWalk::ObjectRequiresTreeBuilderContext(*layout_object) ||
           needs_prepaint_subtree_walk_ ||
           needs_effective_allowed_touch_action_update_ ||
           needs_compositing_requirements_update_ ||
           (layout_box && layout_box->HasSelfPaintingLayer() &&
            layout_box->Layer()->ChildNeedsCompositingInputsUpdate());
  }
  return false;
}

void DisplayLockContext::DidMoveToNewDocument(Document& old_document) {
  DCHECK(element_);
  document_ = &element_->GetDocument();

  if (is_observed_) {
    old_document.UnregisterDisplayLockActivationObservation(element_);
    document_->RegisterDisplayLockActivationObservation(element_);
  }

  // Since we're observing the lifecycle updates, ensure that we listen to the
  // right document's view.
  if (HasResolver()) {
    if (old_document.View())
      old_document.View()->UnregisterFromLifecycleNotifications(this);
    if (document_->View())
      document_->View()->RegisterForLifecycleNotifications(this);
  }

  if (IsLocked()) {
    old_document.RemoveLockedDisplayLock();
    document_->AddLockedDisplayLock();
    if (!IsActivatable(DisplayLockActivationReason::kAny)) {
      old_document.DecrementDisplayLockBlockingAllActivation();
      document_->IncrementDisplayLockBlockingAllActivation();
    }
  }
}

void DisplayLockContext::WillStartLifecycleUpdate(const LocalFrameView& view) {
  if (update_budget_)
    update_budget_->OnLifecycleChange(view.CurrentLifecycleData());
}

void DisplayLockContext::DidFinishLifecycleUpdate(const LocalFrameView& view) {
  if (state_ == kCommitting) {
    FinishUpdateResolver(kResolve);
    state_ = kUnlocked;
    return;
  }

  if (state_ != kUpdating)
    return;

  // If we became disconnected for any reason, then we should reject the
  // update promise and go back to the locked state.
  if (!ConnectedToView()) {
    FinishUpdateResolver(kReject, rejection_names::kElementIsDisconnected);
    update_budget_.reset();

    if (state_ == kCommitting) {
      state_ = kUnlocked;
    } else {
      state_ = kLocked;
    }
    return;
  }

  if (update_budget_->NeedsLifecycleUpdates()) {
    // Note that we post a task to schedule an animation, since rAF requests can
    // be ignored if they happen from within a lifecycle update.
    GetExecutionContext()
        ->GetTaskRunner(TaskType::kMiscPlatformAPI)
        ->PostTask(FROM_HERE, WTF::Bind(&DisplayLockContext::ScheduleAnimation,
                                        WrapWeakPersistent(this)));
    return;
  }

  FinishUpdateResolver(kResolve);
  update_budget_.reset();
  state_ = kLocked;
}

void DisplayLockContext::NotifyWillDisconnect() {
  if (!IsLocked() || !element_ || !element_->GetLayoutObject())
    return;
  // If we're locked while being disconnected, we need to layout the parent.
  // The reason for this is that we might skip the layout if we're empty while
  // locked, but it's important to update IsSelfCollapsingBlock property on
  // the parent so that it's up to date. This property is updated during
  // layout.
  if (auto* parent = element_->GetLayoutObject()->Parent())
    parent->SetNeedsLayout(layout_invalidation_reason::kDisplayLock);
}

void DisplayLockContext::ElementDisconnected() {
  UpdateActivationObservationIfNeeded();
}

void DisplayLockContext::ElementConnected() {
  UpdateActivationObservationIfNeeded();
}

void DisplayLockContext::ScheduleAnimation() {
  DCHECK(element_);
  // We could have posted a task to run ScheduleAnimation if we're updating.
  // However, before that task runs, we could have disconnected the element
  // already. If that's the case and we don't need to finalize update, then we
  // can skip scheduling animation. If we do need to finalize update (ie reset
  // update_budget_), then we should still schedule an animation just in case
  // one was not scheduled.
  if ((!ConnectedToView() && !update_budget_) || !document_ ||
      !document_->GetPage()) {
    return;
  }

  // Schedule an animation to perform the lifecycle phases.
  document_->GetPage()->Animator().ScheduleVisualUpdate(document_->GetFrame());
}

const char* DisplayLockContext::ShouldForceUnlock() const {
  DCHECK(element_);
  // This function is only called after style, layout tree, or lifecycle
  // updates, so the style should be up-to-date, except in the case of nested
  // locks, where the style recalc will never actually get to |element_|.
  // TODO(vmpstr): We need to figure out what to do here, since we don't know
  // what the style is and whether this element has proper containment. However,
  // forcing an update from the ancestor locks seems inefficient. For now, we
  // just optimistically assume that we have all of the right containment in
  // place. See crbug.com/926276 for more information.
  if (element_->NeedsStyleRecalc()) {
    DCHECK(DisplayLockUtilities::NearestLockedExclusiveAncestor(*element_));
    return nullptr;
  }

  if (element_->HasDisplayContentsStyle())
    return rejection_names::kUnsupportedDisplay;

  auto* style = element_->GetComputedStyle();
  // Note that if for whatever reason we don't have computed style, then
  // optimistically assume that we have containment.
  if (!style)
    return nullptr;
  if (!style->ContainsStyle() || !style->ContainsLayout())
    return rejection_names::kContainmentNotSatisfied;

  // We allow replaced elements to be locked. This check is similar to the check
  // in DefinitelyNewFormattingContext() in element.cc, but in this case we
  // allow object element to get locked.
  if (IsA<HTMLObjectElement>(*element_) || IsA<HTMLImageElement>(*element_) ||
      element_->IsFormControlElement() || element_->IsMediaElement() ||
      element_->IsFrameOwnerElement() || element_->IsSVGElement()) {
    return nullptr;
  }

  // From https://www.w3.org/TR/css-contain-1/#containment-layout
  // If the element does not generate a principal box (as is the case with
  // display: contents or display: none), or if the element is an internal
  // table element other than display: table-cell, if the element is an
  // internal ruby element, or if the element’s principal box is a
  // non-atomic inline-level box, layout containment has no effect.
  // (Note we're allowing display:none for display locked elements, and a bit
  // more restrictive on ruby - banning <ruby> elements entirely).
  auto* html_element = DynamicTo<HTMLElement>(element_.Get());
  if ((style->IsDisplayTableType() &&
       style->Display() != EDisplay::kTableCell) ||
      (!html_element || IsA<HTMLRubyElement>(html_element)) ||
      (style->IsDisplayInlineType() && !style->IsDisplayReplacedType())) {
    return rejection_names::kContainmentNotSatisfied;
  }
  return nullptr;
}

bool DisplayLockContext::ForceUnlockIfNeeded() {
  // We must have "contain: style layout", and disallow display:contents
  // for display locking. Note that we should always guarantee this after
  // every style or layout tree update. Otherwise, proceeding with layout may
  // cause unexpected behavior. By rejecting the promise, the behavior can be
  // detected by script.
  // TODO(rakina): If this is after acquire's promise is resolved and update()
  // commit() isn't in progress, the web author won't know that the element
  // got unlocked. Figure out how to notify the author.
  if (auto* reason = ShouldForceUnlock()) {
    FinishUpdateResolver(kReject, reason);
    state_ = kUnlocked;
    return true;
  }
  return false;
}

bool DisplayLockContext::ConnectedToView() const {
  return element_ && document_ && element_->isConnected() && document_->View();
}

// Scoped objects implementation
// -----------------------------------------------

DisplayLockContext::ScopedForcedUpdate::ScopedForcedUpdate(
    DisplayLockContext* context)
    : context_(context) {}

DisplayLockContext::ScopedForcedUpdate::ScopedForcedUpdate(
    ScopedForcedUpdate&& other)
    : context_(other.context_) {
  other.context_ = nullptr;
}

DisplayLockContext::ScopedForcedUpdate::~ScopedForcedUpdate() {
  if (context_)
    context_->NotifyForcedUpdateScopeEnded();
}

// StateChangeHelper implementation
// -----------------------------------------------
DisplayLockContext::StateChangeHelper::StateChangeHelper(
    DisplayLockContext* context)
    : context_(context) {}

DisplayLockContext::StateChangeHelper& DisplayLockContext::StateChangeHelper::
operator=(State new_state) {
  if (new_state == state_)
    return *this;

  bool was_activatable =
      context_->IsActivatable(DisplayLockActivationReason::kAny);
  bool was_locked = context_->IsLocked();

  state_ = new_state;

  if (!context_->document_)
    return *this;

  bool is_activatable =
      context_->IsActivatable(DisplayLockActivationReason::kAny);
  bool is_locked = context_->IsLocked();

  UpdateActivationBlockingCount(!was_locked || was_activatable,
                                !is_locked || is_activatable);

  // Adjust the total number of locked display locks.
  auto& document = *context_->document_;
  if (is_locked != was_locked) {
    if (was_locked)
      document.RemoveLockedDisplayLock();
    else
      document.AddLockedDisplayLock();
  }

  context_->UpdateActivationObservationIfNeeded();
  return *this;
}

void DisplayLockContext::StateChangeHelper::UpdateActivationBlockingCount(
    bool old_activatable,
    bool new_activatable) {
  auto& document = *context_->document_;
  // Adjust activation blocking lock counts.
  if (old_activatable != new_activatable) {
    if (old_activatable)
      document.IncrementDisplayLockBlockingAllActivation();
    else
      document.DecrementDisplayLockBlockingAllActivation();
  }
}

}  // namespace blink
