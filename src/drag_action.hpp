#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>

#include <optional>

#include "action.hpp"
#include "object.hpp"
#include "time.hpp"

namespace automat {

struct Location;
struct LocationWidget;
struct Board;
struct BoardWidget;
struct Toy;

namespace ui {

// Interface for widgets that can receive locations being dropped on them.
struct DropTarget {
  virtual bool CanDrop(Location&) const = 0;

  // Snap the given Rect, which is hovered over this drop target.
  // bounds_origin is a point that should be used for center-aligned snapping.
  // Optionally respecting a "fixed point".
  virtual SkMatrix DropSnap(const Rect& bounds, Vec2 bounds_origin,
                            Vec2* fixed_point = nullptr) = 0;

  // Called for pointer-owned locations released over this target. The drop target is
  // responsible for taking ownership of the location!
  virtual void DropLocation(Ptr<Location>&&) = 0;
};
}  // namespace ui

struct DragLocationAction : Action {
  Vec2 current_position;  // root widget coordinates
  Vec<Ptr<Location>> locations;
  MortalPtr<BoardWidget> board_widget;     // set while board-owned
  Vec<std::unique_ptr<Toy>> held_widgets;  // owns the LocationWidgets while pointer-owned

  // Pointer-owned pickup: the locations belong to no board yet.
  DragLocationAction(ui::Pointer&, Ptr<Location>&&);
  DragLocationAction(ui::Pointer&, Vec<Ptr<Location>>&&);
  // Board-owned pickup: the locations stay in the board (see BoardWidget::DragStack).
  DragLocationAction(ui::Pointer&, Vec<Ptr<Location>>&&, BoardWidget&);
  ~DragLocationAction() override;

  void Update() override;
  void Poll(time::Timer&) override;

  void VisitObjects(std::function<void(Object&)>) override;

 private:
  void Init();
  void Extract();
  void Enter(BoardWidget&);
  // Move location `i` (pointer-owned) into the board, together with its widgets.
  void GiveToBoard(BoardWidget&, Board&, size_t i);
  void SetRadar(BoardWidget&, float target);
};

bool IsDragged(const LocationWidget& location);

}  // namespace automat
