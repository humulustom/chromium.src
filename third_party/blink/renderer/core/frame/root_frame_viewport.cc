// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/root_frame_viewport.h"

#include "base/barrier_closure.h"
#include "cc/input/snap_selection_strategy.h"
#include "third_party/blink/public/mojom/scroll/scroll_into_view_params.mojom-blink.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/scroll_anchor.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/scroll/scroll_alignment.h"
#include "third_party/blink/renderer/core/scroll/scroll_animator_base.h"
#include "third_party/blink/renderer/core/scroll/scroll_into_view_params_type_converters.h"
#include "third_party/blink/renderer/core/scroll/smooth_scroll_sequencer.h"
#include "third_party/blink/renderer/platform/geometry/double_rect.h"
#include "third_party/blink/renderer/platform/geometry/float_rect.h"
#include "third_party/blink/renderer/platform/geometry/layout_rect.h"

namespace blink {
namespace {
// Computes the rect of valid scroll offsets reachable by user scrolls for the
// scrollable area.
FloatRect GetUserScrollableRect(const ScrollableArea& area) {
  FloatRect user_scrollable_rect;
  FloatSize scrollable_size =
      area.MaximumScrollOffset() - area.MinimumScrollOffset();
  if (area.UserInputScrollable(kHorizontalScrollbar)) {
    user_scrollable_rect.SetX(area.MinimumScrollOffset().Width());
    user_scrollable_rect.SetWidth(scrollable_size.Width());
  } else {
    user_scrollable_rect.SetX(area.GetScrollOffset().Width());
    user_scrollable_rect.SetWidth(0);
  }

  if (area.UserInputScrollable(kVerticalScrollbar)) {
    user_scrollable_rect.SetY(area.MinimumScrollOffset().Height());
    user_scrollable_rect.SetHeight(scrollable_size.Height());
  } else {
    user_scrollable_rect.SetY(area.GetScrollOffset().Height());
    user_scrollable_rect.SetHeight(0);
  }
  return user_scrollable_rect;
}

}  // namespace
RootFrameViewport::RootFrameViewport(ScrollableArea& visual_viewport,
                                     ScrollableArea& layout_viewport)
    : visual_viewport_(visual_viewport), should_restore_scroll_(false) {
  SetLayoutViewport(layout_viewport);
}

void RootFrameViewport::SetLayoutViewport(ScrollableArea& new_layout_viewport) {
  if (layout_viewport_.Get() == &new_layout_viewport)
    return;

  if (layout_viewport_ && layout_viewport_->GetScrollAnchor())
    layout_viewport_->GetScrollAnchor()->SetScroller(layout_viewport_.Get());

  layout_viewport_ = &new_layout_viewport;

  if (layout_viewport_->GetScrollAnchor())
    layout_viewport_->GetScrollAnchor()->SetScroller(this);
}

ScrollableArea& RootFrameViewport::LayoutViewport() const {
  DCHECK(layout_viewport_);
  return *layout_viewport_;
}

PhysicalRect RootFrameViewport::RootContentsToLayoutViewportContents(
    LocalFrameView& root_frame_view,
    const PhysicalRect& rect) const {
  PhysicalRect ret = rect;

  // If the root LocalFrameView is the layout viewport then coordinates in the
  // root LocalFrameView's content space are already in the layout viewport's
  // content space.
  if (root_frame_view.LayoutViewport() == &LayoutViewport())
    return ret;

  // Make the given rect relative to the top of the layout viewport's content
  // by adding the scroll position.
  // TODO(bokan): This will have to be revisited if we ever remove the
  // restriction that a root scroller must be exactly screen filling.
  ret.Move(
      PhysicalOffset::FromFloatSizeRound(LayoutViewport().GetScrollOffset()));

  return ret;
}

void RootFrameViewport::RestoreToAnchor(const ScrollOffset& target_offset) {
  // Clamp the scroll offset of each viewport now so that we force any invalid
  // offsets to become valid so we can compute the correct deltas.
  GetVisualViewport().SetScrollOffset(
      GetVisualViewport().GetScrollOffset(),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);
  LayoutViewport().SetScrollOffset(
      LayoutViewport().GetScrollOffset(),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  ScrollOffset delta = target_offset - GetScrollOffset();

  GetVisualViewport().SetScrollOffset(
      GetVisualViewport().GetScrollOffset() + delta,
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  delta = target_offset - GetScrollOffset();

  // Since the main thread LocalFrameView has integer scroll offsets, scroll it
  // to the next pixel and then we'll scroll the visual viewport again to
  // compensate for the sub-pixel offset. We need this "overscroll" to ensure
  // the pixel of which we want to be partially in appears fully inside the
  // LocalFrameView since the VisualViewport is bounded by the LocalFrameView.
  IntSize layout_delta = IntSize(
      delta.Width() < 0 ? floor(delta.Width()) : ceil(delta.Width()),
      delta.Height() < 0 ? floor(delta.Height()) : ceil(delta.Height()));

  LayoutViewport().SetScrollOffset(
      ScrollOffset(LayoutViewport().ScrollOffsetInt() + layout_delta),
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);

  delta = target_offset - GetScrollOffset();
  GetVisualViewport().SetScrollOffset(
      GetVisualViewport().GetScrollOffset() + delta,
      mojom::blink::ScrollIntoViewParams::Type::kProgrammatic);
}

void RootFrameViewport::DidUpdateVisualViewport() {
  if (ScrollAnchor* anchor = LayoutViewport().GetScrollAnchor())
    anchor->Clear();
}

LayoutBox* RootFrameViewport::GetLayoutBox() const {
  return LayoutViewport().GetLayoutBox();
}

FloatQuad RootFrameViewport::LocalToVisibleContentQuad(
    const FloatQuad& quad,
    const LayoutObject* local_object,
    unsigned flags) const {
  if (!layout_viewport_)
    return quad;
  FloatQuad viewport_quad =
      layout_viewport_->LocalToVisibleContentQuad(quad, local_object, flags);
  if (visual_viewport_) {
    viewport_quad = visual_viewport_->LocalToVisibleContentQuad(
        viewport_quad, local_object, flags);
  }
  return viewport_quad;
}

scoped_refptr<base::SingleThreadTaskRunner>
RootFrameViewport::GetTimerTaskRunner() const {
  return LayoutViewport().GetTimerTaskRunner();
}

int RootFrameViewport::HorizontalScrollbarHeight(
    OverlayScrollbarClipBehavior behavior) const {
  return LayoutViewport().HorizontalScrollbarHeight(behavior);
}

int RootFrameViewport::VerticalScrollbarWidth(
    OverlayScrollbarClipBehavior behavior) const {
  return LayoutViewport().VerticalScrollbarWidth(behavior);
}

void RootFrameViewport::UpdateScrollAnimator() {
  GetScrollAnimator().SetCurrentOffset(ScrollOffsetFromScrollAnimators());
}

ScrollOffset RootFrameViewport::ScrollOffsetFromScrollAnimators() const {
  return GetVisualViewport().GetScrollAnimator().CurrentOffset() +
         LayoutViewport().GetScrollAnimator().CurrentOffset();
}

IntRect RootFrameViewport::VisibleContentRect(
    IncludeScrollbarsInRect scrollbar_inclusion) const {
  return IntRect(
      IntPoint(ScrollOffsetInt()),
      GetVisualViewport().VisibleContentRect(scrollbar_inclusion).Size());
}

PhysicalRect RootFrameViewport::VisibleScrollSnapportRect(
    IncludeScrollbarsInRect scrollbar_inclusion) const {
  // The effective viewport is the intersection of the visual viewport with the
  // layout viewport.
  PhysicalRect frame_rect_in_content(
      PhysicalOffset::FromFloatSizeRound(LayoutViewport().GetScrollOffset()),
      PhysicalSize(
          LayoutViewport().VisibleContentRect(scrollbar_inclusion).Size()));
  PhysicalRect visual_rect_in_content(
      PhysicalOffset::FromFloatSizeRound(
          LayoutViewport().GetScrollOffset() +
          GetVisualViewport().GetScrollAnimator().CurrentOffset()),
      PhysicalSize(
          GetVisualViewport().VisibleContentRect(scrollbar_inclusion).Size()));

  PhysicalRect visible_scroll_snapport =
      Intersection(visual_rect_in_content, frame_rect_in_content);
  if (!LayoutViewport().GetLayoutBox())
    return visible_scroll_snapport;

  const ComputedStyle* style = LayoutViewport().GetLayoutBox()->Style();
  LayoutRectOutsets padding(
      MinimumValueForLength(style->ScrollPaddingTop(),
                            visible_scroll_snapport.Height()),
      MinimumValueForLength(style->ScrollPaddingRight(),
                            visible_scroll_snapport.Width()),
      MinimumValueForLength(style->ScrollPaddingBottom(),
                            visible_scroll_snapport.Height()),
      MinimumValueForLength(style->ScrollPaddingLeft(),
                            visible_scroll_snapport.Width()));
  visible_scroll_snapport.Contract(padding);

  return visible_scroll_snapport;
}

bool RootFrameViewport::ShouldUseIntegerScrollOffset() const {
  // Fractionals are floored in the ScrollAnimatorBase but it's important that
  // the ScrollAnimators of the visual and layout viewports get the precise
  // fractional number so never use integer scrolling for RootFrameViewport,
  // we'll let the truncation happen in the subviewports.
  return false;
}

bool RootFrameViewport::IsActive() const {
  return LayoutViewport().IsActive();
}

int RootFrameViewport::ScrollSize(ScrollbarOrientation orientation) const {
  IntSize scroll_dimensions =
      MaximumScrollOffsetInt() - MinimumScrollOffsetInt();
  return (orientation == kHorizontalScrollbar) ? scroll_dimensions.Width()
                                               : scroll_dimensions.Height();
}

bool RootFrameViewport::IsScrollCornerVisible() const {
  return LayoutViewport().IsScrollCornerVisible();
}

IntRect RootFrameViewport::ScrollCornerRect() const {
  return LayoutViewport().ScrollCornerRect();
}

void RootFrameViewport::ApplyPendingHistoryRestoreScrollOffset() {
  if (!pending_view_state_)
    return;

  bool should_restore_scale = pending_view_state_->page_scale_factor_;

  // For main frame restore scale and visual viewport position
  ScrollOffset visual_viewport_offset(
      pending_view_state_->visual_viewport_scroll_offset_);

  // If the visual viewport's offset is (-1, -1) it means the history item
  // is an old version of HistoryItem so distribute the scroll between
  // the main frame and the visual viewport as best as we can.
  if (visual_viewport_offset.Width() == -1 &&
      visual_viewport_offset.Height() == -1) {
    visual_viewport_offset = pending_view_state_->scroll_offset_ -
                             LayoutViewport().GetScrollOffset();
  }

  auto* visual_viewport = static_cast<VisualViewport*>(&GetVisualViewport());
  if (should_restore_scale && should_restore_scroll_) {
    visual_viewport->SetScaleAndLocation(
        pending_view_state_->page_scale_factor_,
        visual_viewport->IsPinchGestureActive(),
        FloatPoint(visual_viewport_offset));
  } else if (should_restore_scale) {
    visual_viewport->SetScale(pending_view_state_->page_scale_factor_);
  } else if (should_restore_scroll_) {
    visual_viewport->SetLocation(FloatPoint(visual_viewport_offset));
  }

  should_restore_scroll_ = false;

  pending_view_state_.reset();
}

void RootFrameViewport::SetScrollOffset(
    const ScrollOffset& offset,
    mojom::blink::ScrollIntoViewParams::Type scroll_type,
    mojom::blink::ScrollIntoViewParams::Behavior scroll_behavior,
    ScrollCallback on_finish) {
  UpdateScrollAnimator();

  if (scroll_behavior == mojom::blink::ScrollIntoViewParams::Behavior::kAuto)
    scroll_behavior = ScrollBehaviorStyle();

  if (scroll_type == mojom::blink::ScrollIntoViewParams::Type::kAnchoring) {
    DistributeScrollBetweenViewports(offset, scroll_type, scroll_behavior,
                                     kLayoutViewport, std::move(on_finish));
    return;
  }

  if (scroll_behavior ==
      mojom::blink::ScrollIntoViewParams::Behavior::kSmooth) {
    DistributeScrollBetweenViewports(offset, scroll_type, scroll_behavior,
                                     kVisualViewport, std::move(on_finish));
    return;
  }

  ScrollOffset clamped_offset = ClampScrollOffset(offset);
  ScrollableArea::SetScrollOffset(clamped_offset, scroll_type, scroll_behavior,
                                  std::move(on_finish));
}

mojom::blink::ScrollIntoViewParams::Behavior
RootFrameViewport::ScrollBehaviorStyle() const {
  return LayoutViewport().ScrollBehaviorStyle();
}

WebColorScheme RootFrameViewport::UsedColorScheme() const {
  return LayoutViewport().UsedColorScheme();
}

ScrollOffset RootFrameViewport::ClampToUserScrollableOffset(
    const ScrollOffset& offset) const {
  ScrollOffset scroll_offset = offset;
  FloatRect user_scrollable = GetUserScrollableRect(LayoutViewport()) +
                              GetUserScrollableRect(GetVisualViewport());
  scroll_offset.SetWidth(clampTo(scroll_offset.Width(), user_scrollable.X(),
                                 user_scrollable.MaxX()));
  scroll_offset.SetHeight(clampTo(scroll_offset.Height(), user_scrollable.Y(),
                                  user_scrollable.MaxY()));
  return scroll_offset;
}

PhysicalRect RootFrameViewport::ScrollIntoView(
    const PhysicalRect& rect_in_absolute,
    const mojom::blink::ScrollIntoViewParamsPtr& params) {
  PhysicalRect scroll_snapport_rect = VisibleScrollSnapportRect();

  PhysicalRect rect_in_document = rect_in_absolute;
  rect_in_document.Move(
      PhysicalOffset::FromFloatSizeFloor(LayoutViewport().GetScrollOffset()));

  ScrollOffset new_scroll_offset =
      ClampScrollOffset(ScrollAlignment::GetScrollOffsetToExpose(
          scroll_snapport_rect, rect_in_document,
          params->align_x.To<ScrollAlignment>(),
          params->align_y.To<ScrollAlignment>(), GetScrollOffset()));
  if (params->type == mojom::blink::ScrollIntoViewParams::Type::kUser)
    new_scroll_offset = ClampToUserScrollableOffset(new_scroll_offset);

  FloatPoint end_point = ScrollOffsetToPosition(new_scroll_offset);
  std::unique_ptr<cc::SnapSelectionStrategy> strategy =
      cc::SnapSelectionStrategy::CreateForEndPosition(
          gfx::ScrollOffset(end_point), true, true);
  if (GetLayoutBox()) {
    end_point = GetSnapPositionAndSetTarget(*strategy).value_or(end_point);
    new_scroll_offset = ScrollPositionToOffset(end_point);
  }

  if (new_scroll_offset != GetScrollOffset()) {
    if (params->is_for_scroll_sequence) {
      DCHECK(params->type ==
                 mojom::blink::ScrollIntoViewParams::Type::kProgrammatic ||
             params->type == mojom::blink::ScrollIntoViewParams::Type::kUser);
      mojom::blink::ScrollIntoViewParams::Behavior behavior =
          DetermineScrollBehavior(params->behavior,
                                  GetLayoutBox()->StyleRef().ScrollBehavior());
      GetSmoothScrollSequencer()->QueueAnimation(this, new_scroll_offset,
                                                 behavior);
    } else {
      ScrollableArea::SetScrollOffset(new_scroll_offset, params->type);
    }
  }

  // Return the newly moved rect to absolute coordinates.
  // TODO(szager): PaintLayerScrollableArea::ScrollIntoView clips the return
  // value to the visible content rect, but this does not.
  rect_in_document.Move(
      -PhysicalOffset::FromFloatSizeRound(LayoutViewport().GetScrollOffset()));
  return rect_in_document;
}

void RootFrameViewport::UpdateScrollOffset(
    const ScrollOffset& offset,
    mojom::blink::ScrollIntoViewParams::Type scroll_type) {
  DistributeScrollBetweenViewports(
      offset, scroll_type,
      mojom::blink::ScrollIntoViewParams::Behavior::kInstant, kVisualViewport);
}

void RootFrameViewport::DistributeScrollBetweenViewports(
    const ScrollOffset& offset,
    mojom::blink::ScrollIntoViewParams::Type scroll_type,
    mojom::blink::ScrollIntoViewParams::Behavior behavior,
    ViewportToScrollFirst scroll_first,
    ScrollCallback on_finish) {
  // Make sure we use the scroll offsets as reported by each viewport's
  // ScrollAnimatorBase, since its ScrollableArea's offset may have the
  // fractional part truncated off.
  // TODO(szager): Now that scroll offsets are stored as floats, can we take the
  // scroll offset directly from the ScrollableArea's rather than the animators?
  ScrollOffset old_offset = ScrollOffsetFromScrollAnimators();

  ScrollOffset delta = offset - old_offset;

  if (delta.IsZero()) {
    if (on_finish)
      std::move(on_finish).Run();
    return;
  }

  ScrollableArea& primary =
      scroll_first == kVisualViewport ? GetVisualViewport() : LayoutViewport();
  ScrollableArea& secondary =
      scroll_first == kVisualViewport ? LayoutViewport() : GetVisualViewport();

  ScrollOffset target_offset = primary.ClampScrollOffset(
      primary.GetScrollAnimator().CurrentOffset() + delta);

  auto all_done = on_finish ? base::BarrierClosure(2, std::move(on_finish))
                            : base::RepeatingClosure();

  // DistributeScrollBetweenViewports can be called from SetScrollOffset,
  // so we assume that aborting sequenced smooth scrolls has been handled.
  // It can also be called from inside an animation to set the offset in
  // each frame. In that case, we shouldn't abort sequenced smooth scrolls.
  primary.SetScrollOffset(target_offset, scroll_type, behavior, all_done);

  // Scroll the secondary viewport if all of the scroll was not applied to the
  // primary viewport.
  ScrollOffset updated_offset =
      secondary.GetScrollAnimator().CurrentOffset() + FloatSize(target_offset);
  ScrollOffset applied = updated_offset - old_offset;
  delta -= applied;

  if (delta.IsZero()) {
    if (all_done)
      all_done.Run();
    return;
  }

  target_offset = secondary.ClampScrollOffset(
      secondary.GetScrollAnimator().CurrentOffset() + delta);
  secondary.SetScrollOffset(target_offset, scroll_type, behavior, all_done);
}

IntSize RootFrameViewport::ScrollOffsetInt() const {
  return FlooredIntSize(GetScrollOffset());
}

ScrollOffset RootFrameViewport::GetScrollOffset() const {
  return LayoutViewport().GetScrollOffset() +
         GetVisualViewport().GetScrollOffset();
}

IntSize RootFrameViewport::MinimumScrollOffsetInt() const {
  return IntSize(LayoutViewport().MinimumScrollOffsetInt() +
                 GetVisualViewport().MinimumScrollOffsetInt());
}

IntSize RootFrameViewport::MaximumScrollOffsetInt() const {
  return LayoutViewport().MaximumScrollOffsetInt() +
         GetVisualViewport().MaximumScrollOffsetInt();
}

ScrollOffset RootFrameViewport::MaximumScrollOffset() const {
  return LayoutViewport().MaximumScrollOffset() +
         GetVisualViewport().MaximumScrollOffset();
}

IntSize RootFrameViewport::ClampScrollOffset(
    const IntSize& scroll_offset) const {
  return scroll_offset.ShrunkTo(MaximumScrollOffsetInt())
      .ExpandedTo(MinimumScrollOffsetInt());
}

ScrollOffset RootFrameViewport::ClampScrollOffset(
    const ScrollOffset& scroll_offset) const {
  return scroll_offset.ShrunkTo(MaximumScrollOffset())
      .ExpandedTo(MinimumScrollOffset());
}

IntSize RootFrameViewport::ContentsSize() const {
  return LayoutViewport().ContentsSize();
}

bool RootFrameViewport::ShouldScrollOnMainThread() const {
  return LayoutViewport().ShouldScrollOnMainThread();
}

bool RootFrameViewport::ScrollbarsCanBeActive() const {
  return LayoutViewport().ScrollbarsCanBeActive();
}

bool RootFrameViewport::UserInputScrollable(
    ScrollbarOrientation orientation) const {
  return GetVisualViewport().UserInputScrollable(orientation) ||
         LayoutViewport().UserInputScrollable(orientation);
}

bool RootFrameViewport::ShouldPlaceVerticalScrollbarOnLeft() const {
  return LayoutViewport().ShouldPlaceVerticalScrollbarOnLeft();
}

void RootFrameViewport::ScrollControlWasSetNeedsPaintInvalidation() {
  LayoutViewport().ScrollControlWasSetNeedsPaintInvalidation();
}

cc::Layer* RootFrameViewport::LayerForScrolling() const {
  return LayoutViewport().LayerForScrolling();
}

cc::Layer* RootFrameViewport::LayerForHorizontalScrollbar() const {
  return LayoutViewport().LayerForHorizontalScrollbar();
}

cc::Layer* RootFrameViewport::LayerForVerticalScrollbar() const {
  return LayoutViewport().LayerForVerticalScrollbar();
}

cc::Layer* RootFrameViewport::LayerForScrollCorner() const {
  return LayoutViewport().LayerForScrollCorner();
}

ScrollResult RootFrameViewport::UserScroll(
    ScrollGranularity granularity,
    const FloatSize& delta,
    ScrollableArea::ScrollCallback on_finish) {
  base::ScopedClosureRunner run_on_return(std::move(on_finish));

  // TODO(bokan/ymalik): Once smooth scrolling is permanently enabled we
  // should be able to remove this method override and use the base class
  // version: ScrollableArea::userScroll.

  UpdateScrollAnimator();

  // Distribute the scroll between the visual and layout viewport.

  float step_x = ScrollStep(granularity, kHorizontalScrollbar);
  float step_y = ScrollStep(granularity, kVerticalScrollbar);

  FloatSize pixel_delta(delta);
  pixel_delta.Scale(step_x, step_y);

  // Precompute the amount of possible scrolling since, when animated,
  // ScrollAnimator::userScroll will report having consumed the total given
  // scroll delta, regardless of how much will actually scroll, but we need to
  // know how much to leave for the layout viewport.
  FloatSize visual_consumed_delta =
      GetVisualViewport().GetScrollAnimator().ComputeDeltaToConsume(
          pixel_delta);

  // Split the remaining delta between scrollable and unscrollable axes of the
  // layout viewport. We only pass a delta to the scrollable axes and remember
  // how much was held back so we can add it to the unused delta in the
  // result.
  FloatSize layout_delta = pixel_delta - visual_consumed_delta;
  FloatSize scrollable_axis_delta(
      LayoutViewport().UserInputScrollable(kHorizontalScrollbar)
          ? layout_delta.Width()
          : 0,
      LayoutViewport().UserInputScrollable(kVerticalScrollbar)
          ? layout_delta.Height()
          : 0);

  // If there won't be any scrolling, bail early so we don't produce any side
  // effects like cancelling existing animations.
  if (visual_consumed_delta.IsZero() && scrollable_axis_delta.IsZero()) {
    return ScrollResult(false, false, pixel_delta.Width(),
                        pixel_delta.Height());
  }

  CancelProgrammaticScrollAnimation();
  if (SmoothScrollSequencer* sequencer = GetSmoothScrollSequencer())
    sequencer->AbortAnimations();

  // TODO(bokan): Why do we call userScroll on the animators directly and
  // not through the ScrollableAreas?
  if (visual_consumed_delta == pixel_delta) {
    ScrollResult visual_result =
        GetVisualViewport().GetScrollAnimator().UserScroll(
            granularity, visual_consumed_delta, run_on_return.Release());
    return visual_result;
  }

  ScrollableArea::ScrollCallback callback = run_on_return.Release();
  auto all_done = callback ? base::BarrierClosure(2, std::move(callback))
                           : base::RepeatingClosure();
  ScrollResult visual_result =
      GetVisualViewport().GetScrollAnimator().UserScroll(
          granularity, visual_consumed_delta, all_done);

  ScrollResult layout_result = LayoutViewport().GetScrollAnimator().UserScroll(
      granularity, scrollable_axis_delta, all_done);

  // Remember to add any delta not used because of !userInputScrollable to the
  // unusedScrollDelta in the result.
  FloatSize unscrollable_axis_delta = layout_delta - scrollable_axis_delta;

  return ScrollResult(
      visual_result.did_scroll_x || layout_result.did_scroll_x,
      visual_result.did_scroll_y || layout_result.did_scroll_y,
      layout_result.unused_scroll_delta_x + unscrollable_axis_delta.Width(),
      layout_result.unused_scroll_delta_y + unscrollable_axis_delta.Height());
}

bool RootFrameViewport::ScrollAnimatorEnabled() const {
  return LayoutViewport().ScrollAnimatorEnabled();
}

CompositorElementId RootFrameViewport::GetScrollElementId() const {
  return LayoutViewport().GetScrollElementId();
}

CompositorElementId RootFrameViewport::GetScrollbarElementId(
    ScrollbarOrientation orientation) {
  return GetVisualViewport().VisualViewportSuppliesScrollbars()
             ? GetVisualViewport().GetScrollbarElementId(orientation)
             : LayoutViewport().GetScrollbarElementId(orientation);
}

ChromeClient* RootFrameViewport::GetChromeClient() const {
  return LayoutViewport().GetChromeClient();
}

SmoothScrollSequencer* RootFrameViewport::GetSmoothScrollSequencer() const {
  return LayoutViewport().GetSmoothScrollSequencer();
}

void RootFrameViewport::ServiceScrollAnimations(double monotonic_time) {
  ScrollableArea::ServiceScrollAnimations(monotonic_time);
  LayoutViewport().ServiceScrollAnimations(monotonic_time);
  GetVisualViewport().ServiceScrollAnimations(monotonic_time);
}

void RootFrameViewport::UpdateCompositorScrollAnimations() {
  ScrollableArea::UpdateCompositorScrollAnimations();
  LayoutViewport().UpdateCompositorScrollAnimations();
  GetVisualViewport().UpdateCompositorScrollAnimations();
}

void RootFrameViewport::CancelProgrammaticScrollAnimation() {
  ScrollableArea::CancelProgrammaticScrollAnimation();
  LayoutViewport().CancelProgrammaticScrollAnimation();
  GetVisualViewport().CancelProgrammaticScrollAnimation();
}

void RootFrameViewport::ClearScrollableArea() {
  ScrollableArea::ClearScrollableArea();
  LayoutViewport().ClearScrollableArea();
  GetVisualViewport().ClearScrollableArea();
}

ScrollbarTheme& RootFrameViewport::GetPageScrollbarTheme() const {
  return LayoutViewport().GetPageScrollbarTheme();
}

const cc::SnapContainerData* RootFrameViewport::GetSnapContainerData() const {
  return LayoutViewport().GetSnapContainerData();
}

void RootFrameViewport::SetSnapContainerData(
    base::Optional<cc::SnapContainerData> data) {
  LayoutViewport().SetSnapContainerData(data);
}

bool RootFrameViewport::SetTargetSnapAreaElementIds(
    cc::TargetSnapAreaElementIds snap_target_ids) {
  return LayoutViewport().SetTargetSnapAreaElementIds(snap_target_ids);
}

bool RootFrameViewport::SnapContainerDataNeedsUpdate() const {
  return LayoutViewport().SnapContainerDataNeedsUpdate();
}

void RootFrameViewport::SetSnapContainerDataNeedsUpdate(bool needs_update) {
  LayoutViewport().SetSnapContainerDataNeedsUpdate(needs_update);
}

bool RootFrameViewport::NeedsResnap() const {
  return LayoutViewport().NeedsResnap();
}

void RootFrameViewport::SetNeedsResnap(bool needs_resnap) {
  LayoutViewport().SetNeedsResnap(needs_resnap);
}

base::Optional<FloatPoint> RootFrameViewport::GetSnapPositionAndSetTarget(
    const cc::SnapSelectionStrategy& strategy) {
  return LayoutViewport().GetSnapPositionAndSetTarget(strategy);
}

void RootFrameViewport::Trace(blink::Visitor* visitor) {
  visitor->Trace(visual_viewport_);
  visitor->Trace(layout_viewport_);
  ScrollableArea::Trace(visitor);
}

}  // namespace blink
