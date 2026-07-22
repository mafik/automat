// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drag_action.hpp"

#include <include/core/SkPath.h>
#include <include/effects/SkDashPathEffect.h>

#include <cmath>
#include <ranges>

#include "action.hpp"
#include "board.hpp"
#include "color.hpp"
#include "embedded.hpp"
#include "math.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "ui_connection_widget.hpp"

using namespace automat::ui;

namespace automat {

static ui::DropTarget* FindDropTarget(DragLocationAction& a, Widget& widget) {
  for (auto* child : widget.layers) {
    if (auto drop_target = FindDropTarget(a, *child)) {
      return drop_target;
    }
  }
  if (auto drop_target = widget.AsDropTarget()) {
    Vec2 point = a.pointer.PositionWithin(widget);
    if (widget.shape.isEmpty() || widget.shape.contains(point.x, point.y)) {
      if (drop_target->CanDrop(*a.locations.back())) {
        return drop_target;
      }
    }
  }
  return nullptr;
}

static ui::DropTarget* FindDropTarget(DragLocationAction& a) {
  return FindDropTarget(a, a.pointer.root_widget);
}

void DragLocationAction::Update() {
  current_position = pointer.PositionOnCanvas();

  ui::DropTarget* drop_target = FindDropTarget(*this);
  auto* hovered_board = dynamic_cast<BoardWidget*>(drop_target);

  if (board_widget && hovered_board != board_widget.Get()) {
    Extract();
  }
  if (!board_widget && hovered_board) {
    if (auto board = hovered_board->LockBoard()) {
      bool any_owned = false;
      for (auto& location : locations) {
        if (location->object && board->LocationOrNull(*location->object)) {
          any_owned = true;
          break;
        }
      }
      if (!any_owned) {
        Enter(*hovered_board);
      }
    }
  }

  Vec2 owner_offset = {0, 0};
  if (board_widget) {
    if (auto board = board_widget->LockBoard()) {
      owner_offset = board->position;
    }
  }
  Vec2 owner_position = current_position - owner_offset;

  int n = locations.size();
  ObjectToy* widgets[n];
  for (int i = 0; i < n; ++i) {
    widgets[i] = &locations[i]->widget->ToyForObject();
  }
  Rect location_bounds[n];
  for (int i = 0; i < n; ++i) {
    location_bounds[i] = widgets[i]->CoarseBounds().rect;
  }
  SkMatrix location_transform[n];
  for (int i = 0; i < n; ++i) {
    float scale = widgets[i]->GetBaseScale();
    location_transform[i] = SkMatrix::Scale(scale, scale)
                                .postTranslate(owner_position.x, owner_position.y)
                                .preTranslate(-locations[i]->widget->local_anchor->x,
                                              -locations[i]->widget->local_anchor->y);
  }

  Vec2 bounds_origin;
  if (widgets[n - 1]->CenteredAtZero()) {
    bounds_origin = location_transform[n - 1].mapOrigin();
  } else {
    bounds_origin = location_transform[n - 1].mapPoint(location_bounds[n - 1].Center());
  }

  for (int i = 0; i < n; ++i) {
    location_transform[i].mapRect(&location_bounds[i].sk);
  }

  Rect bounds_all = location_bounds[0];
  for (int i = 1; i < n; ++i) {
    bounds_all.ExpandToInclude(location_bounds[i]);
  }

  SkMatrix snap = {};
  if (board_widget) {
    snap = board_widget->DropSnap(bounds_all, bounds_origin, &owner_position);
  } else if (drop_target && !hovered_board) {
    snap = drop_target->DropSnap(bounds_all, bounds_origin, &owner_position);
  }

  bool moved = false;
  for (int i = 0; i < n; ++i) {
    location_transform[i].postConcat(snap);
    Vec2 new_position;
    float new_scale;
    Location::FromMatrix(location_transform[i], locations[i]->widget->LocalAnchor(), new_position,
                         new_scale);
    Vec2& loc_position = locations[i]->Position(*locations[i]->widget);
    if (!NearlyEqual(new_position, loc_position)) {
      moved = true;
      Vec2 fix = current_position - last_position;
      widgets[i]->local_to_parent.postTranslate(fix.x, fix.y);
    }
    loc_position = new_position;
    locations[i]->Scale(*locations[i]->widget) = new_scale;
  }

  if (moved) {
    if (board_widget) {
      board_widget->WakeAnimation();
    }
    for (auto& location : locations) {
      if (location->widget) location->widget->UpdateAutoconnectArgs();
    }
    for (auto& location : locations) {
      location->WakeToys();
      location->InvalidateConnectionWidgets(true, false);
    }
  }

  last_position = current_position;
}

void DragLocationAction::Extract() {
  auto* bw = board_widget.Get();
  board_widget = nullptr;
  auto board = bw->LockBoard();
  if (!board) return;
  SetRadar(*bw, 0);
  for (auto& location : locations) {
    location->InvalidateConnectionWidgets(true, false);
    board->Extract(*location);
    location->Position(*location->widget) += board->position;
    auto lw_unique = bw->toys.Extract(*location);
    if (auto* lw = static_cast<LocationWidget*>(lw_unique.get())) {
      if (location->object) {
        lw->owned_toy = bw->toys.Extract(*location->object);
      }
      lw->Reparent(*pointer.GetWidget());
      held_widgets.push_back(std::move(lw_unique));
    }
    location->WakeToys();
  }
  bw->WakeAnimation();
  widget->WakeAnimation();  // restart the ownership-marker animation
  audio::Play(embedded::assets_SFX_canvas_pick_wav);
}

void DragLocationAction::GiveToBoard(BoardWidget& bw, Board& board, size_t i) {
  auto& location = locations[i];
  location->board = board.AcquireWeakPtr();
  location->Position(*location->widget) -= board.position;
  {
    auto lock = std::lock_guard(vm.mutex);
    board.locations.insert(board.locations.begin(), location);
  }
  if (i < held_widgets.size() && held_widgets[i]) {
    auto* lw = static_cast<LocationWidget*>(held_widgets[i].get());
    held_widgets[i]->Reparent(bw);
    bw.toys.Insert(*location, std::move(held_widgets[i]));
    if (lw->owned_toy && location->object) {
      lw->owned_toy->Reparent(*lw);
      bw.toys.Insert(*location->object, std::move(lw->owned_toy));
    }
  }
  location->WakeToys();
  location->InvalidateConnectionWidgets(true, false);
}

void DragLocationAction::Enter(BoardWidget& bw) {
  auto board = bw.LockBoard();
  if (!board) return;
  for (size_t i = locations.size(); i-- > 0;) {
    GiveToBoard(bw, *board, i);
  }
  held_widgets.clear();
  board_widget = &bw;
  SetRadar(bw, 1);
  bw.WakeAnimation();
}

void DragLocationAction::SetRadar(BoardWidget& bw, float target) {
  for (auto& [key, toy] : bw.toys.container) {
    auto* connection_widget = dynamic_cast<ui::ConnectionWidget*>(toy.get());
    if (!connection_widget) continue;
    float value = 0;
    if (target > 0) {
      auto start = connection_widget->LockOwner<Object>();
      if (!start || !connection_widget->iface) continue;
      Argument arg = connection_widget->Bind<Argument>(*start);
      for (auto& location : locations) {
        if (start.Get() == location->object.Get()) {
          // We grabbed the start object of this connection widget
          value = target;
        } else if (arg.CanConnect(*location->object)) {
          // This connection widget can be connected to one of dragged locations
          value = target;
        }
      }
    }
    if (connection_widget->animation_state.radar_alpha_target != value) {
      connection_widget->animation_state.radar_alpha_target = value;
      connection_widget->WakeAnimation();
    }
  }
}

void DragLocationAction::VisitObjects(std::function<void(Object&)> visitor) {
  for (auto& loc : locations) {
    visitor(*loc->object);
  }
}

SkPath DragLocationWidget::Shape() const { return SkPath(); }

void DragLocationAction::Poll(time::Timer& timer) {
  for (auto& held : held_widgets) {
    if (held) held->Poll(timer);
  }
}

ui::Tock DragLocationWidget::Tick(time::Timer& timer) {
  if (action.held_widgets.empty()) {
    return {};
  }
  time_seconds = timer.NowSeconds();
  return Tock::Drawing;
}

void DragLocationWidget::Draw(SkCanvas& canvas) const {
  if (action.board_widget || action.held_widgets.empty()) return;

  constexpr SkColor kMarkerColor = "#ffd76a"_color;
  static const SkScalar kDashPattern[] = {3_mm, 2_mm};

  SkPaint ring_paint;
  ring_paint.setColor(kMarkerColor);
  ring_paint.setStyle(SkPaint::kStroke_Style);
  ring_paint.setStrokeWidth(0.5_mm);
  ring_paint.setAntiAlias(true);
  ring_paint.setPathEffect(SkDashPathEffect::Make(kDashPattern, (float)(-time_seconds * 8_mm)));

  SkPaint tether_paint = ring_paint;
  tether_paint.setAlphaf(0.8f);

  auto lock = std::lock_guard(vm.mutex);
  Vec2 drag_point = action.current_position;
  for (auto& location : action.locations) {
    if (!location || !location->object) continue;
    for (auto& board : vm.boards) {
      auto* resident = board->LocationOrNull(*location->object);
      if (!resident || resident == location.get()) continue;

      Vec2 center = board->position + resident->PeekPosition();
      float radius = 1_cm;
      if (resident->widget && resident->widget->toy) {
        auto m = ui::TransformBetween(*resident->widget->toy, *this);
        Rect bounds = resident->widget->toy->CoarseBounds().rect;
        m.mapRect(&bounds.sk);
        center = bounds.Center();
        radius = bounds.Hypotenuse() / 2 + 2_mm;
      }
      canvas.drawCircle(center, radius, ring_paint);

      Vec2 within_board = drag_point - board->position;
      bool hovering = fabsf(within_board.x) <= 50_cm && fabsf(within_board.y) <= 50_cm;
      if (hovering && location->widget && location->widget->toy) {
        auto m = ui::TransformBetween(*location->widget->toy, *this);
        Vec2 from = m.mapPoint(location->widget->toy->LocalAnchor());
        Vec2 dir = Normalize(center - from);
        canvas.drawLine(from, center - dir * radius, tether_paint);
      }
    }
  }
}

void DragLocationAction::Init() {
  auto& root = pointer.root_widget;
  root.drag_action_count++;
  if (root.drag_action_count == 1) {
    root.black_hole.WakeAnimation();
  }
  widget->ValidateHierarchy();
  widget->RedrawThisFrame();
  root.WakeAnimation();
  last_position = current_position = pointer.PositionOnCanvas();
  Update();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg,
                                       BoardWidget& bw)
    : Action(pointer),
      locations(std::move(locations_arg)),
      board_widget(&bw),
      widget(new DragLocationWidget(pointer.GetWidget(), *this)) {
  for (auto& location : std::ranges::reverse_view(locations)) {
    auto& lw = bw.toys.FindOrMake(*location, &bw);
    lw.local_anchor = pointer.PositionWithin(lw.ToyForObject());
    location->WakeToys();
  }
  SetRadar(bw, 1);
  bw.RedrawThisFrame();  // unbudgeted pick-up frame; other roots' views repaint via the wakes
  Init();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg)
    : Action(pointer),
      locations(std::move(locations_arg)),
      widget(new DragLocationWidget(pointer.GetWidget(), *this)) {
  for (auto& location : std::ranges::reverse_view(locations)) {
    auto lw_unique = LocationWidget::MakePointerOwned(pointer.GetWidget(), *location);
    auto* lw = lw_unique.get();
    held_widgets.insert(held_widgets.begin(), std::move(lw_unique));
    lw->local_anchor = pointer.PositionWithin(lw->ToyForObject());
    location->WakeToys();
  }
  Init();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Ptr<Location>&& location_arg)
    : DragLocationAction(pointer, MakeVec<Ptr<Location>>(std::move(location_arg))) {}

DragLocationAction::~DragLocationAction() {
  auto& root = pointer.root_widget;

  auto SettleAnchor = [](Location& location) {
    // Use matrix to keep the object in place while clearing the local anchor
    if (!location.widget || !location.widget->local_anchor) return;
    LocationWidget& widget = *location.widget;
    auto matrix =
        Location::ToMatrix(location.Position(widget), location.Scale(widget), *widget.local_anchor);
    widget.local_anchor.reset();
    Location::FromMatrix(matrix, widget.LocalAnchor(), location.Position(widget),
                         location.Scale(widget));
    widget.RedrawThisFrame();
  };

  if (board_widget) {
    SetRadar(*board_widget, 0);
    for (auto& location : std::ranges::reverse_view(locations)) {
      location->WakeToys();
      SettleAnchor(*location);
    }
    if (auto board = board_widget->LockBoard()) {
      for (auto& location : std::ranges::reverse_view(locations)) {
        board->MoveToTop(*location);
      }
    }
    audio::Play(embedded::assets_SFX_canvas_drop_wav);
  } else {
    ui::DropTarget* drop_target = FindDropTarget(*this);
    auto* bw = dynamic_cast<BoardWidget*>(drop_target);
    auto board = bw ? bw->LockBoard() : nullptr;
    if (board) {
      for (size_t i = locations.size(); i-- > 0;) {
        auto& location = locations[i];
        SettleAnchor(*location);
        Location* resident = location->object ? board->LocationOrNull(*location->object) : nullptr;
        if (resident && resident != location.get()) {
          if (resident->widget) {
            auto* lw = i < held_widgets.size() ? static_cast<LocationWidget*>(held_widgets[i].get())
                                               : nullptr;
            if (lw && lw->toy) {
              resident->widget->AddIncomingFlight(
                  ui::TransformBetween(*lw->toy, *resident->widget));
            } else {
              resident->widget->WakeAnimation();
            }
          }
        } else {
          GiveToBoard(*bw, *board, i);
        }
      }
      audio::Play(embedded::assets_SFX_canvas_drop_wav);
    } else if (drop_target) {
      for (size_t i = locations.size(); i-- > 0;) {
        auto& location = locations[i];
        SettleAnchor(*location);
        if (i < held_widgets.size() && held_widgets[i]) {
          if (auto* pw = pointer.GetWidget()) pw->AdoptZombie(std::move(held_widgets[i]));
        }
        drop_target->DropLocation(std::move(location));
      }
    } else {
      auto new_board = MAKE_PTR(Board);
      new_board->position = RoundToMilimeters(current_position);
      {
        auto lock = std::lock_guard(vm.mutex);
        vm.boards.insert(vm.boards.begin(), new_board);
      }
      auto& new_bw = root.toys.FindOrMake(*new_board, &root);
      SkM44 board_transform(root.CanvasToWindow());
      board_transform.preTranslate(new_board->position.x, new_board->position.y);
      new_bw.local_to_parent = board_transform;
      for (size_t i = locations.size(); i-- > 0;) {
        SettleAnchor(*locations[i]);
        GiveToBoard(new_bw, *new_board, i);
      }
      vm.WakeToys();
      audio::Play(embedded::assets_SFX_canvas_drop_wav);
    }
  }

  root.drag_action_count--;
  root.WakeAnimation();
}

bool IsDragged(const LocationWidget& location) { return location.local_anchor.has_value(); }

}  // namespace automat
