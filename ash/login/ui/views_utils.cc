// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/login/ui/views_utils.h"

#include <algorithm>
#include <memory>

#include "ash/login/ui/non_accessible_view.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/widget/widget.h"

namespace ash {

namespace {

class ContainerView : public NonAccessibleView,
                      public views::ViewTargeterDelegate {
 public:
  ContainerView() {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  }
  ~ContainerView() override = default;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override {
    const auto& children = target->children();
    const auto hits_child = [target, rect](const views::View* child) {
      gfx::RectF child_rect(rect);
      views::View::ConvertRectToTarget(target, child, &child_rect);
      return child->GetVisible() &&
             child->HitTestRect(gfx::ToEnclosingRect(child_rect));
    };
    return std::any_of(children.cbegin(), children.cend(), hits_child);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ContainerView);
};

}  // namespace

namespace login_views_utils {

std::unique_ptr<views::View> WrapViewForPreferredSize(
    std::unique_ptr<views::View> view) {
  auto proxy = std::make_unique<NonAccessibleView>();
  auto layout_manager = std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical);
  layout_manager->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStart);
  proxy->SetLayoutManager(std::move(layout_manager));
  proxy->AddChildView(std::move(view));
  return proxy;
}

bool ShouldShowLandscape(const views::Widget* widget) {
  // |widget| is null when the view is being constructed. Default to landscape
  // in that case. A new layout will happen when the view is attached to a
  // widget (see LockContentsView::AddedToWidget), which will let us fetch the
  // correct display orientation.
  if (!widget)
    return true;

  // Get the orientation for |widget|.
  const display::Display& display =
      display::Screen::GetScreen()->GetDisplayNearestWindow(
          widget->GetNativeWindow());

  // The display bounds are updated after a rotation. This means that if the
  // device has resolution 800x600, and the rotation is
  // display::Display::ROTATE_0, bounds() is 800x600. On
  // display::Display::ROTATE_90, bounds() is 600x800.
  //
  // ash/login/ui assumes landscape means width>height, and portrait means
  // height>width.
  //
  // Considering the actual rotation of the device introduces edge-cases, ie,
  // when the device resolution in display::Display::ROTATE_0 is 768x1024, such
  // as in https://crbug.com/858858.
  return display.bounds().width() > display.bounds().height();
}

bool HasFocusInAnyChildView(views::View* view) {
  // Find the topmost ancestor of the focused view, or |view|, whichever comes
  // first.
  views::View* search = view->GetFocusManager()->GetFocusedView();
  while (search && search != view)
    search = search->parent();
  return search == view;
}

views::Label* CreateBubbleLabel(const base::string16& message,
                                SkColor color,
                                views::View* view_defining_max_width,
                                int font_size_delta,
                                gfx::Font::Weight font_weight) {
  views::Label* label =
      new views::Label(message, views::style::CONTEXT_MESSAGE_BOX_BODY_TEXT,
                       views::style::STYLE_PRIMARY);
  label->SetAutoColorReadabilityEnabled(false);
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetEnabledColor(color);
  label->SetSubpixelRenderingEnabled(false);
  const gfx::FontList& base_font_list = views::Label::GetDefaultFontList();
  label->SetFontList(base_font_list.Derive(
      font_size_delta, gfx::Font::FontStyle::NORMAL, font_weight));
  if (view_defining_max_width != nullptr) {
    label->SetMultiLine(true);
    label->SetAllowCharacterBreak(true);
    // Make sure to set a maximum label width, otherwise text wrapping will
    // significantly increase width and layout may not work correctly if
    // the input string is very long.
    label->SetMaximumWidth(view_defining_max_width->GetPreferredSize().width());
  }
  return label;
}

views::View* GetBubbleContainer(views::View* view) {
  views::View* v = view;
  while (v->parent() != nullptr)
    v = v->parent();

  views::View* root_view = v;
  // An arbitrary id that no other child of root view should use.
  const int kMenuContainerId = 1000;
  views::View* container = nullptr;
  for (auto* child : root_view->children()) {
    if (child->GetID() == kMenuContainerId) {
      container = child;
      break;
    }
  }

  if (!container) {
    container = root_view->AddChildView(std::make_unique<ContainerView>());
    container->SetID(kMenuContainerId);
  }

  return container;
}

gfx::Point CalculateBubblePositionLeftRightStrategy(gfx::Rect anchor,
                                                    gfx::Size bubble,
                                                    gfx::Rect bounds) {
  gfx::Rect result(anchor.x() - bubble.width(), anchor.y(), bubble.width(),
                   bubble.height());
  // Trying to show on the left side.
  // If there is not enough space show on the right side.
  if (result.x() < bounds.x()) {
    result.Offset(anchor.width() + result.width(), 0);
  }
  result.AdjustToFit(bounds);
  return result.origin();
}

gfx::Point CalculateBubblePositionRightLeftStrategy(gfx::Rect anchor,
                                                    gfx::Size bubble,
                                                    gfx::Rect bounds) {
  gfx::Rect result(anchor.x() + anchor.width(), anchor.y(), bubble.width(),
                   bubble.height());
  // Trying to show on the right side.
  // If there is not enough space show on the left side.
  if (result.right() > bounds.right()) {
    result.Offset(-anchor.width() - result.width(), 0);
  }
  result.AdjustToFit(bounds);
  return result.origin();
}

}  // namespace login_views_utils
}  // namespace ash
