// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drag_action.hpp"

#include <include/core/SkPath.h>

#include <cmath>
#include <ranges>

#include "action.hpp"
#include "board.hpp"
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
  auto hovered = !board_widget && hovered_board ? hovered_board->LockBoard() : nullptr;
  bool any_owned = false;
  Location* merge_targets[locations.size()];
  for (size_t i = 0; i < locations.size(); ++i) {
    merge_targets[i] =
        hovered && locations[i]->object ? hovered->LocationOrNull(*locations[i]->object) : nullptr;
    any_owned |= merge_targets[i] != nullptr;
  }
  if (hovered && !any_owned) {
    Enter(*hovered_board);
  }

  float weight_target = drop_target ? 1 : 0;
  for (auto& location : locations) {
    // TODO: forbidden access, maybe LocationWidget::Tick could take care of this locally?
    LocationWidget* lw = location->widget.Get();
    if (lw && lw->local_to_parent_weight_target != weight_target) {
      lw->local_to_parent_weight_target = weight_target;
      lw->WakeAnimation();
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
    Vec2 grab = locations[i]->widget->LocalAnchor();
    location_transform[i] = SkMatrix::Scale(scale, scale)
                                .postTranslate(owner_position.x, owner_position.y)
                                .preTranslate(-grab.x, -grab.y);
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
    if (merge_targets[i]) {
      new_position = hovered->position + merge_targets[i]->PeekPosition();
      new_scale = merge_targets[i]->PeekScale();
    }
    LocationWidget& lw = *locations[i]->widget;
    Vec2& loc_position = locations[i]->Position(lw);
    if (!NearlyEqual(new_position, loc_position)) {
      moved = true;
    }
    loc_position = new_position;
    locations[i]->Scale(lw) = new_scale;
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
    auto* connection_widget = dynamic_cast<ArgumentToy*>(toy.get());
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
    if (connection_widget->radar_activation_target != value) {
      connection_widget->radar_activation_target = value;
      connection_widget->WakeAnimation();
    }
  }
}

void DragLocationAction::VisitObjects(std::function<void(Object&)> visitor) {
  for (auto& loc : locations) {
    visitor(*loc->object);
  }
}

void DragLocationAction::Poll(time::Timer& timer) {
  for (auto& held : held_widgets) {
    if (held) held->Poll(timer);
  }
}

void DragLocationAction::Init() {
  auto& root = pointer.root_widget;
  root.drag_action_count++;
  if (root.drag_action_count == 1) {
    root.black_hole.WakeAnimation();
  }
  pointer.GetWidget()->ValidateHierarchy();
  root.WakeAnimation();
  current_position = pointer.PositionOnCanvas();
  Update();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg,
                                       BoardWidget& bw)
    : Action(pointer), locations(std::move(locations_arg)), board_widget(&bw) {
  auto& base_toy = bw.toys.FindOrMake(*locations.back(), &bw).ToyForObject();
  Vec2 base_grab = base_toy.CoarseBounds().Clamp(pointer.PositionWithin(base_toy));
  for (auto& location : std::ranges::reverse_view(locations)) {
    auto& lw = bw.toys.FindOrMake(*location, &bw);
    auto& toy = lw.ToyForObject();
    lw.AnchorToPointer(pointer, TransformBetween(base_toy, toy).mapPoint(base_grab));
    location->WakeToys();
  }
  SetRadar(bw, 1);
  bw.RedrawThisFrame();  // unbudgeted pick-up frame; other roots' views repaint via the wakes
  Init();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg)
    : Action(pointer), locations(std::move(locations_arg)) {
  ObjectToy* base_toy = nullptr;
  Vec2 base_grab;
  for (auto& location : std::ranges::reverse_view(locations)) {
    auto lw_unique = LocationWidget::MakePointerOwned(pointer.GetWidget(), *location);
    auto* lw = lw_unique.get();
    held_widgets.insert(held_widgets.begin(), std::move(lw_unique));
    auto& toy = lw->ToyForObject();
    if (!base_toy) {
      base_toy = &toy;
      base_grab = toy.CoarseBounds().Clamp(pointer.PositionWithin(toy));
    }
    lw->AnchorToPointer(pointer, TransformBetween(*base_toy, toy).mapPoint(base_grab));
    location->WakeToys();
  }
  Init();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Ptr<Location>&& location_arg)
    : DragLocationAction(pointer, MakeVec<Ptr<Location>>(std::move(location_arg))) {}

DragLocationAction::~DragLocationAction() {
  auto& root = pointer.root_widget;

  auto SettleAnchor = [](Location& location) {
    // Use matrix to keep the object in place while clearing the grab anchor
    if (!location.widget) return;
    LocationWidget& widget = *location.widget;
    auto* grab = widget.GrabAnchor();
    if (!grab) return;
    auto matrix = Location::ToMatrix(location.Position(widget), location.Scale(widget), grab->pos);
    widget.toy->texture_anchors.clear();
    widget.toy->local_to_parent_weight = 1;
    widget.local_to_parent_weight_target = 1;
    Location::FromMatrix(matrix, widget.LocalAnchor(), location.Position(widget),
                         location.Scale(widget));
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
        Location& dragged_loc = *locations[i];
        Location* preexisting_loc =
            dragged_loc.object ? board->LocationOrNull(*dragged_loc.object) : nullptr;
        if (preexisting_loc && preexisting_loc != &dragged_loc) {
          auto* dragged_widget = i < held_widgets.size()
                                     ? static_cast<LocationWidget*>(held_widgets[i].get())
                                     : nullptr;
          if (dragged_widget && dragged_widget->toy) {
            dragged_widget->owner = preexisting_loc->AcquireWeakPtr();
            dragged_widget->merging = true;
            // Freeze the displacement that the pointer was producing.
            if (auto* grab = dragged_widget->GrabAnchor()) {
              grab->warp_by += pointer.PositionWithin(*dragged_widget->toy) - grab->pos;
              grab->pointer = nullptr;
            }
            dragged_widget->WakeAnimation();
            if (auto* pw = pointer.GetWidget()) pw->AdoptZombie(std::move(held_widgets[i]));
          } else if (auto* resident_widget = bw->toys.FindOrNull(*preexisting_loc)) {
            resident_widget->WakeAnimation();
          }
        } else {
          SettleAnchor(dragged_loc);
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

bool IsDragged(const LocationWidget& location) { return location.GrabAnchor() != nullptr; }

}  // namespace automat
