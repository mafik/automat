// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drag_action.hpp"

#include <include/core/SkPath.h>

#include <algorithm>
#include <cassert>
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

static void SettleAnchor(Location& location) {
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
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg,
                                       BoardWidget* board, Optional<Vec2> grab,
                                       std::unique_ptr<Toy>&& toy)
    : Action(pointer), locations(std::move(locations_arg)), board_widget(board) {
  assert(!locations.empty());
  size_t n = locations.size();
  LocationWidget* widgets[n];
  if (board) {
    for (size_t i = 0; i < n; ++i) {
      widgets[i] = &board->toys.FindOrMake(*locations[i], board);
    }
  } else {
    for (size_t i = 0; i < n; ++i) {
      auto widget =
          LocationWidget::MakePointerOwned(pointer.GetWidget(), *locations[i], std::move(toy));
      widgets[i] = widget.get();
      held_widgets.push_back(std::move(widget));
    }
    OrderHeldWidgets();
  }
  auto& base_toy = widgets[n - 1]->ToyForObject();
  if (!grab) {
    grab = base_toy.CoarseBounds().Clamp(pointer.PositionWithin(base_toy));
  }
  for (size_t i = n; i-- > 0;) {
    auto& toy = widgets[i]->ToyForObject();
    widgets[i]->AnchorToPointer(pointer, TransformBetween(base_toy, toy).mapPoint(*grab));
    locations[i]->WakeToys();
  }
  if (board) {
    SetRadar(1);
    board->RedrawThisFrame();  // unbudgeted pick-up frame; other roots' views repaint via the wakes
  }
  auto& root = pointer.root_widget;
  root.drag_action_count++;
  if (root.drag_action_count == 1) {
    root.black_hole.WakeAnimation();
  }
  pointer.GetWidget()->ValidateHierarchy();
  root.WakeAnimation();
  Update();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Ptr<Location>&& location,
                                       BoardWidget* board, Optional<Vec2> grab)
    : DragLocationAction(pointer, MakeVec(std::move(location)), board, grab) {}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Ptr<Location>&& location,
                                       std::unique_ptr<Toy>&& toy)
    : DragLocationAction(pointer, MakeVec(std::move(location)), nullptr, std::nullopt,
                         std::move(toy)) {}

DragLocationAction::~DragLocationAction() {
  if (!locations.empty()) {
    Drop();
  }
  auto& root = pointer.root_widget;
  root.drag_action_count--;
  root.WakeAnimation();
}

Vec2 DragLocationAction::OwnerOffset() {
  return board_widget ? board_widget->LockBoard()->position : Vec2(0, 0);
}

void DragLocationAction::OrderHeldWidgets() {
  auto& layers = pointer.GetWidget()->layers;
  for (size_t i = 1; i < held_widgets.size(); ++i) {
    layers.OrderBelow(held_widgets[i].get(), held_widgets[i - 1].get());
  }
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
    Enter(*hovered_board, *hovered);
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

  Vec2 owner_position = current_position - OwnerOffset();

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
  BoardWidget& bw = *board_widget;
  auto board = bw.LockBoard();
  SetRadar(0);
  board_widget = nullptr;
  for (auto& location : locations) {
    location->InvalidateConnectionWidgets(true, false);
    board->Extract(*location);
    auto lw_unique = bw.toys.Extract(*location);
    if (auto* lw = static_cast<LocationWidget*>(lw_unique.get())) {
      if (location->object) {
        lw->owned_toy = bw.toys.Extract(*location->object);
      }
      lw->Reparent(*pointer.GetWidget());
    } else {
      lw_unique = LocationWidget::MakePointerOwned(pointer.GetWidget(), *location);
    }
    location->Position(static_cast<LocationWidget&>(*lw_unique)) += board->position;
    held_widgets.push_back(std::move(lw_unique));
    location->WakeToys();
  }
  OrderHeldWidgets();
  bw.WakeAnimation();
  audio::Play(embedded::assets_SFX_canvas_pick_wav);
}

void DragLocationAction::Enter(BoardWidget& bw, Board& board) {
  for (size_t i = locations.size(); i-- > 0;) {
    GiveToBoard(bw, board, i);
  }
  held_widgets.clear();
  board_widget = &bw;
  SetRadar(1);
  bw.WakeAnimation();
}

void DragLocationAction::GiveToBoard(BoardWidget& bw, Board& board, size_t i) {
  auto& location = locations[i];
  auto& lw = static_cast<LocationWidget&>(*held_widgets[i]);
  location->board = board.AcquireWeakPtr();
  location->Position(lw) -= board.position;
  {
    auto lock = std::lock_guard(vm.mutex);
    board.locations.insert(board.locations.begin(), location);
  }
  lw.Reparent(bw);
  bw.toys.Insert(*location, std::move(held_widgets[i]));
  if (lw.owned_toy && location->object) {
    lw.owned_toy->Reparent(lw);
    bw.toys.Insert(*location->object, std::move(lw.owned_toy));
  }
  location->WakeToys();
  location->InvalidateConnectionWidgets(true, false);
}

void DragLocationAction::MergeIntoResidents(BoardWidget& bw, Board& board) {
  for (size_t i = locations.size(); i-- > 0;) {
    Location& dragged = *locations[i];
    Location* resident = dragged.object ? board.LocationOrNull(*dragged.object) : nullptr;
    if (!resident || resident == &dragged) continue;
    auto& widget = static_cast<LocationWidget&>(*held_widgets[i]);
    if (widget.toy) {
      widget.owner = resident->AcquireWeakPtr();
      widget.merging = true;
      if (auto* grab = widget.GrabAnchor()) {
        grab->warp_by += pointer.PositionWithin(*widget.toy) - grab->pos;
        grab->pointer = nullptr;
      }
      widget.WakeAnimation();
      pointer.GetWidget()->AdoptZombie(std::move(held_widgets[i]));
    } else if (auto* resident_widget = bw.toys.FindOrNull(*resident)) {
      resident_widget->WakeAnimation();
    }
    held_widgets.erase(held_widgets.begin() + i);
    locations.erase(locations.begin() + i);
  }
}

void DragLocationAction::Drop() {
  auto& root = pointer.root_widget;
  if (!board_widget) {
    ui::DropTarget* drop_target = FindDropTarget(*this);
    auto* bw = dynamic_cast<BoardWidget*>(drop_target);
    auto board = bw ? bw->LockBoard() : nullptr;
    if (board) {
      MergeIntoResidents(*bw, *board);
      Enter(*bw, *board);
    } else if (drop_target) {
      for (size_t i = locations.size(); i-- > 0;) {
        SettleAnchor(*locations[i]);
        pointer.GetWidget()->AdoptZombie(std::move(held_widgets[i]));
        drop_target->DropLocation(std::move(locations[i]));
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
      Enter(new_bw, *new_board);
      vm.WakeToys();
    }
  }
  if (board_widget) {
    SetRadar(0);
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
  }
}

void DragLocationAction::SetRadar(float target) {
  for (auto& [key, toy] : board_widget->toys.container) {
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
    held->Poll(timer);
  }
}

void DragLocationAction::AddToGroup(Ptr<Location>&& loc_arg) {
  Location* loc = loc_arg.get();
  Vec2 pile = Vec2(locations.size() * 6_mm, locations.size() * -6_mm);
  if (auto* direct = std::get_if<Location::Direct>(&loc->placement)) {
    direct->position = pointer.PositionOnCanvas() - OwnerOffset() + pile;
  }
  Location* above = locations.back().get();
  locations.push_back(std::move(loc_arg));
  LocationWidget* lw;
  if (board_widget) {
    auto board = board_widget->LockBoard();
    loc->board = board->AcquireWeakPtr();
    {
      auto lock = std::lock_guard(vm.mutex);
      auto& list = board->locations;
      auto it = std::find_if(list.begin(), list.end(), [&](auto& l) { return l.get() == above; });
      list.insert(it == list.end() ? list.begin() : std::next(it), locations.back());
    }
    lw = &board_widget->toys.FindOrMake(*loc, board_widget.Get());
    SetRadar(1);
  } else {
    auto widget = LocationWidget::MakePointerOwned(pointer.GetWidget(), *loc);
    lw = widget.get();
    held_widgets.push_back(std::move(widget));
    OrderHeldWidgets();
  }
  auto& toy = lw->ToyForObject();
  lw->AnchorToPointer(pointer, toy.CoarseBounds().Clamp(pointer.PositionWithin(toy)));
  loc->WakeToys();
  pointer.GetWidget()->ValidateHierarchy();
  pointer.root_widget.WakeAnimation();
}

Ptr<Location> DragLocationAction::RemoveFromGroup(Location& loc) {
  for (size_t i = 0; i < locations.size(); ++i) {
    if (locations[i].get() != &loc) continue;
    Ptr<Location> result = std::move(locations[i]);
    locations.erase(locations.begin() + i);
    if (board_widget) {
      if (auto board = board_widget->LockBoard()) board->Extract(loc);
      SetRadar(1);
    } else {
      pointer.GetWidget()->AdoptZombie(std::move(held_widgets[i]));
      held_widgets.erase(held_widgets.begin() + i);
    }
    pointer.root_widget.WakeAnimation();
    if (locations.empty()) {
      pointer.ReplaceAction(*this, nullptr);
    }
    return result;
  }
  return nullptr;
}

bool IsDragged(const LocationWidget& location) { return location.GrabAnchor() != nullptr; }

}  // namespace automat
