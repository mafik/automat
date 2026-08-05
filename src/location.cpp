// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "location.hpp"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkShader.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkImageFilters.h>

#include <algorithm>
#include <cmath>

#include "animation.hpp"
#include "automat.hpp"
#include "base.hpp"
#include "color.hpp"
#include "control_flow.hpp"
#include "drag_action.hpp"
#include "embedded.hpp"
#include "font.hpp"
#include "format.hpp"
#include "global_resources.hpp"
#include "interface.hpp"
#include "math.hpp"
#include "object_iconified.hpp"
#include "raycast.hpp"
#include "render_shadows.hpp"
#include "root_widget.hpp"
#include "time.hpp"
#include "timer_thread.hpp"
#include "ui_connection_widget.hpp"
#include "ui_constants.hpp"
#include "widget.hpp"

using namespace automat::ui;
using namespace std;

namespace automat {

constexpr float kFrameCornerRadius = 0.001;

///////////////////////////////////////////////////////////////////////////////
// Location
///////////////////////////////////////////////////////////////////////////////

Location::Location(WeakPtr<Board> board) : board(std::move(board)) {}

Ptr<Board> Location::LockBoard() const { return board.Lock(); }

Ptr<Object> Location::Clone() const {
  auto clone = MAKE_PTR(Location);
  clone->placement = placement;
  return clone;
}

Location::~Location() {
  board = {};
  for (auto other : update_observers) {
    other->observing_updates.erase(this);
  }
  for (auto other : observing_updates) {
    other->update_observers.erase(this);
  }
  CancelScheduledAt(*this);
}

std::unique_ptr<ObjectToy> Location::MakeToy(ui::Widget* parent) {
  auto toy = LocationWidget::MakeBoardOwned(parent, *this);
  widget = toy.get();
  return toy;
}

Object* Location::Follow() {
  if (object == nullptr) {
    return nullptr;
  }
  return object.get();
}

void Location::Put(Ptr<Object> obj) {
  if (object == nullptr) {
    object = std::move(obj);
    return;
  }
  object = std::move(obj);
}

Ptr<Object> Location::Take() {
  auto ptr = std::move(object);
  WakeToys();
  return std::move(ptr);
}

Ptr<Object> Location::InsertHere(Ptr<Object>&& object) {
  this->object.Swap(object);
  this->object->WakeToys();
  vm.WakeToys();
  return object;
}

Ptr<Object> Location::Create(const Object& prototype) { return InsertHere(prototype.Clone()); }

void Location::SetNumber(double number) { SetText(f("{:g}", number)); }

void Location::Iconify() {
  automat::Iconify(*object);
  if (widget && widget->toy) {
    Scale(*widget) = widget->toy->GetBaseScale();
    widget->toy->WakeAnimation();
  }
  WakeToys();
}

void Location::Deiconify() {
  automat::Deiconify(*object);
  if (widget && widget->toy) {
    Scale(*widget) = widget->toy->GetBaseScale();
    widget->toy->WakeAnimation();
  }
  WakeToys();
}

void Location::FillPosition(LocationWidget& w) {
  auto request = std::exchange(placement, Direct{});
  if (auto* ahead = std::get_if<PlaceAhead>(&request)) {
    auto origin = ahead->origin.Lock();
    if (origin && origin->widget && ahead->arg) {
      std::get_if<Direct>(&placement)->position =
          PositionAhead(*origin, *static_cast<Argument::Table*>(ahead->arg), w.ToyForObject());
      PositionBelow(*this, *origin);
    }
  } else if (auto* between = std::get_if<PlaceBetween>(&request)) {
    auto a = between->a.Lock();
    auto b = between->b.Lock();
    if (a && b) {
      std::get_if<Direct>(&placement)->position = (a->PeekPosition() + b->PeekPosition()) / 2;
    }
  } else if (auto* beside = std::get_if<PlaceBeside>(&request)) {
    if (auto origin = beside->origin.Lock()) {
      std::get_if<Direct>(&placement)->position =
          origin->widget ? PositionBeside(*origin, *this, w.ToyForObject())
                         : origin->PeekPosition();
    }
  }
  WakeToys();
  InvalidateConnectionWidgets(true, false);
}

SkMatrix Location::ToMatrix(Vec2 position, float scale, Vec2 anchor) {
  return SkMatrix::Translate(position.x, position.y).preScale(scale, scale, anchor.x, anchor.y);
}

void Location::FromMatrix(const SkMatrix& matrix, const Vec2& anchor, Vec2& out_position,
                          float& out_scale) {
  out_scale = matrix.getScaleX();
  auto matrix_copy = matrix;
  matrix_copy.preScale(1.f / out_scale, 1.f / out_scale, anchor.x, anchor.y);
  out_position = Vec2(matrix_copy.getTranslateX(), matrix_copy.getTranslateY());
}

///////////////////////////////////////////////////////////////////////////////
// LocationWidget
///////////////////////////////////////////////////////////////////////////////

LocationWidget::LocationWidget(ui::Widget* parent, Location& loc)
    : ObjectToy(parent, loc), elevation(0) {
  loc.widget = this;
}

std::unique_ptr<LocationWidget> LocationWidget::MakeBoardOwned(ui::Widget* parent, Location& loc) {
  auto widget = std::unique_ptr<LocationWidget>(new LocationWidget(parent, loc));
  if (auto* obj_toy = widget->ToyStore().FindOrNull(*loc.object)) {
    // If the object already has a toy, reparent it to keep transform
    widget->toy = obj_toy;
    obj_toy->Reparent(*widget);
    widget->layers.OrderInside(widget->toy.Get());
  }
  return widget;
}

std::unique_ptr<LocationWidget> LocationWidget::MakePointerOwned(ui::Widget* parent,
                                                                 Location& loc) {
  auto widget = std::unique_ptr<LocationWidget>(new LocationWidget(parent, loc));
  if (auto premade = widget->FindRootWidget().toys.Extract(*loc.object)) {
    widget->toy = static_cast<ObjectToy*>(premade.get());
    widget->toy->Reparent(*widget);
    widget->layers.OrderInside(widget->toy.Get());
    widget->owned_toy = std::move(premade);
  }
  return widget;
}

ObjectToy& LocationWidget::ToyForObject() {
  if (!toy) {
    if (auto loc = LockLocation()) {
      if (loc->object) {
        if (loc->LockBoard() == nullptr) {
          owned_toy = loc->object->MakeToy(this);
          toy = static_cast<ObjectToy*>(owned_toy.get());
        } else {
          toy = &ToyStore().FindOrMake(*loc->object, this);
        }
        float& scale = loc->Scale(*this);
        scale = toy->GetBaseScale();
        toy->local_to_parent =
            SkM44(Location::ToMatrix(loc->Position(*this), scale * 1.2, LocalAnchor()));
        alpha = 0;
        layers.OrderInside(toy.Get());
      }
    }
  }
  return *toy;
}

ui::Widget::TextureAnchor* LocationWidget::GrabAnchor() const {
  if (toy) {
    for (auto& anchor : toy->texture_anchors) {
      if (anchor.pointer) {
        return &anchor;
      }
    }
  }
  return nullptr;
}

Vec2 LocationWidget::LocalAnchor() const {
  if (auto* grab = GrabAnchor()) {
    return grab->pos;
  }
  if (toy) {
    return toy->LocalAnchor();
  }
  return Vec2();
}

void LocationWidget::AnchorToPointer(ui::Pointer& pointer, Vec2 grab) {
  auto& toy = ToyForObject();
  Vec2 offset = grab - pointer.PositionWithin(toy);
  toy.texture_anchors = {{grab, ui::Widget::TextureAnchor::NewId(), &pointer, offset, 1_cm}};
}

SkPath LocationWidget::Shape() const {
  static SkPath empty_path = SkPath();
  return empty_path;
}

Optional<Rect> LocationWidget::DrawBounds() const { return nullopt; }

ui::Tock LocationWidget::Tick(time::Timer& timer) {
  Tock tock;
  auto loc = LockLocation();

  if (toy && toy->parent != this) {
    toy = nullptr;
  }

  if (!loc) {
    // Location is dead — fade out and eventually expire
    auto alpha_progress = animation::ExponentialApproach(0, timer.d, 0.1, alpha);
    tock.drawing |= alpha_progress;
    if (!tock.ing) {
      // Bring our cached ObjectToy with us so it doesn't outlive the path back to RootWidget.
      if (toy && toy->parent == this) {
        toy->MarkDead(timer.now);
      }
      MarkDead(timer.now);
    }
    return tock;
  }

  auto alpha_progress = animation::LinearApproach(merging ? 0 : 1, timer.d, 2, alpha);
  tock.drawing |= alpha_progress;

  ToyForObject();  // fills toy

  if (overlap_genid != subtree_shape.getGenerationID()) {
    if (auto* bw = BoardOrNull(*this)) {
      bw->WakeAnimation();
    }
  }

  if (toy) {
    toy->shadow_elevation = 1_mm + elevation * 8_mm;
    Vec2 local_pivot = LocalAnchor();
    Vec2 snap_position;
    float snap_scale;
    Location::FromMatrix(toy->local_to_parent.asM33(), local_pivot, snap_position, snap_scale);

    Vec2 target_position;
    float target_scale;
    if (merging) {
      target_position = loc->PeekPosition();
      target_scale = loc->PeekScale();
      if (auto board = loc->LockBoard()) {
        target_position += board->position;
      }
    } else {
      target_position = loc->Position(*this);
      target_scale = loc->Scale(*this);
    }
    auto scale_progress = animation::LowLevelSpringTowards(
        target_scale, timer.d, kScaleSpringPeriod, kSpringHalfTime, snap_scale, scale_vel);
    auto x_progress = animation::LowLevelSineTowards(
        target_position.x, timer.d, kPositionSpringPeriod, snap_position.x, position_vel.x);
    auto y_progress = animation::LowLevelSineTowards(
        target_position.y, timer.d, kPositionSpringPeriod, snap_position.y, position_vel.y);
    tock.drawing |= scale_progress;
    tock.drawing |= x_progress;
    tock.drawing |= y_progress;

    toy->local_to_parent = SkM44(Location::ToMatrix(snap_position, snap_scale, local_pivot));

    if (merging) {
      if (!toy->texture_anchors.empty()) {
        auto& anchor = toy->texture_anchors.front();
        struct AnchorAnimationState {
          Vec2 warp_by_velocity = {};
        };
        if (anchor.animation_state.size() != sizeof(AnchorAnimationState)) {
          anchor.animation_state.resize(sizeof(AnchorAnimationState));
          new (anchor.animation_state.data()) AnchorAnimationState;
        }
        AnchorAnimationState& animation_state =
            *reinterpret_cast<AnchorAnimationState*>(anchor.animation_state.data());
        animation::Progress release;
        constexpr float kPeriod = 0.2;
        constexpr float kHalfTime = 0.03;
        release |= animation::LowLevelSpringTowards(
            0, timer.d, kPeriod, kHalfTime, anchor.warp_by.x, animation_state.warp_by_velocity.x);
        release |= animation::LowLevelSpringTowards(
            0, timer.d, kPeriod, kHalfTime, anchor.warp_by.y, animation_state.warp_by_velocity.y);
        tock.drawing |= release;
        if (release.settled) {
          toy->texture_anchors.clear();
        }
      }
      if ((toy->texture_anchors.empty() && scale_progress.settled && x_progress.settled &&
           y_progress.settled) ||
          alpha_progress.settled) {
        toy->MarkDead(timer.now);
        MarkDead(timer.now);
      }
    }

    tock.drawing |= animation::ExponentialApproach(local_to_parent_weight_target, timer.d, 0.1,
                                                   toy->local_to_parent_weight);
  }

  // Connection widgets rely on position, scale & alpha so make sure they're updated.
  if (!merging) {
    loc->InvalidateConnectionWidgets(true, false);
  }

  {
    float target_elevation = IsDragged(*this) ? 1 : 0;
    auto elevation_progress = elevation.SineTowards(target_elevation, timer.d, 0.2);
    tock.drawing |= elevation_progress;
  }
  if (HasError(*loc->object)) {
    tock |= Tock::Drawing;
  }
  return tock;
}

void LocationWidget::Draw(SkCanvas& canvas) const {
  if (toy == nullptr) {
    return;  // TODO: Draw a placeholder, we should support empty locations
  }
  SkPath my_shape = toy->shape;
  Rect bounds = Rect(my_shape.getBounds());
  toy->local_to_parent.asM33().mapRect(&bounds.sk);

  if constexpr (false) {  // Gray frame
    SkPaint frame_bg;
    SkColor frame_bg_colors_raw[2] = {0xffcccccc, 0xffaaaaaa};
    SkColor4f frame_bg_colors[2] = {SkColor4f::FromColor(frame_bg_colors_raw[0]),
                                    SkColor4f::FromColor(frame_bg_colors_raw[1])};
    SkPoint gradient_pts[2] = {{0, bounds.top}, {0, bounds.bottom}};
    sk_sp<SkShader> frame_bg_shader = SkShaders::LinearGradient(
        gradient_pts, SkGradient{SkGradient::Colors{frame_bg_colors, SkTileMode::kClamp}, {}});
    frame_bg.setShader(frame_bg_shader);
    canvas.drawPath(my_shape, frame_bg);

    SkPaint frame_border;
    SkColor4f frame_border_colors[2] = {color::AdjustLightness(frame_bg_colors[0], 5),
                                        color::AdjustLightness(frame_bg_colors[1], -5)};
    sk_sp<SkShader> frame_border_shader = SkShaders::LinearGradient(
        gradient_pts, SkGradient{SkGradient::Colors{frame_border_colors, SkTileMode::kClamp}, {}});
    frame_border.setShader(frame_border_shader);
    frame_border.setStyle(SkPaint::kStroke_Style);
    frame_border.setStrokeWidth(0.00025);
    canvas.drawRoundRect(bounds, kFrameCornerRadius, kFrameCornerRadius, frame_border);
  }

  // Draw debug text log below the Location
  float n_lines = 1;
  float offset_y = bounds.bottom;
  float offset_x = bounds.left;
  float line_height = ui::kLetterSize * 1.5;
  auto& font = ui::GetFont();

  canvas.concat(toy->local_to_parent);
  toy->DrawStack(canvas);

  auto loc = LockLocation();
  if (!loc) {
    return;
  }

  HasError(*loc->object, [&](Error& error) {
    constexpr float b = 0.00025;
    SkPaint error_paint;
    error_paint.setColor(SK_ColorRED);
    error_paint.setStyle(SkPaint::kStroke_Style);
    error_paint.setStrokeWidth(2 * b);
    error_paint.setAntiAlias(true);
    offset_x -= b;
    offset_y -= 3 * b;
    error_paint.setStyle(SkPaint::kFill_Style);
    canvas.translate(offset_x, offset_y - n_lines * line_height);
    font.DrawText(canvas, error.text, error_paint);
    canvas.translate(-offset_x, -offset_y + n_lines * line_height);
    n_lines += 1;

    auto ctm = canvas.getLocalToDeviceAs3x3().preConcat(toy->local_to_parent.asM33());
    auto my_shape_px = my_shape.makeTransform(ctm);
    Rect bounds_px = my_shape_px.getBounds();
    float blur_radius = ctm.mapRadius(7_mm);
    auto clip = bounds.Outset(blur_radius * 3);
    auto clip_px = bounds_px.Outset(blur_radius * 3);
    Status status;
    static auto shader = resources::CompileShader(embedded::assets_error_sksl, status);
    if (!OK(status)) {
      ERROR << status;
    }

    SkRuntimeEffectBuilder builder(shader);
    static auto t0 = time::SecondsSinceEpoch();
    builder.uniform("iTime") = (float)(time::SecondsSinceEpoch() - t0) * 3;
    builder.uniform("iLeft") = bounds_px.left;
    builder.uniform("iRight") = bounds_px.right;
    builder.uniform("iTop") = bounds_px.top;
    builder.uniform("iBottom") = bounds_px.bottom;

    SkPaint fire_paint;
    fire_paint.setImageFilter(SkImageFilters::RuntimeShader(builder, "iMask", nullptr));

    // Note that we're saving the canvas state twice.
    // This is because of https://issues.skia.org/issues/447458443
    // Otherwise the coordinates passed to the shader will be messed up.
    canvas.save();
    canvas.resetMatrix();
    canvas.clipRect(clip_px.sk);
    canvas.saveLayer(&clip_px.sk, &fire_paint);

    SkPaint paint;
    paint.setAntiAlias(false);

    SkPaint stroke_paint;
    stroke_paint.setAntiAlias(false);
    stroke_paint.setStyle(SkPaint::kStroke_Style);
    stroke_paint.setStrokeWidth(1);
    canvas.drawPath(my_shape_px, stroke_paint);

    paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, blur_radius, false));
    canvas.drawPath(my_shape_px, paint);

    paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, blur_radius / 4, false));
    my_shape_px = my_shape_px.makeToggleInverseFillType();
    canvas.drawPath(my_shape_px, paint);

    canvas.restore();
    canvas.restore();
  });
}

std::unique_ptr<Action> LocationWidget::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  return nullptr;
}

void Location::InvalidateConnectionWidgets(bool moved, bool value_changed) const {
  if (!object) return;
  object->Each<Argument>([&](Argument arg) {
    arg.WakeToys();
    return LoopControl::Continue;
  });

  // Incoming: iterate all ToyStore entries to find ConnectionWidgets pointing to this location
  if (auto board = LockBoard()) {
    auto lock = std::lock_guard(vm.mutex);
    for (auto& other_loc : board->locations) {
      if (other_loc.get() == this) continue;
      other_loc->object->Each<Argument>([&](Argument arg) {
        if (arg.ObjectOrNull() == object.Get()) {
          arg.WakeToys();
        }
        return LoopControl::Continue;
      });
    }
  }
}

void LocationWidget::UpdateAutoconnectArgs() {
  auto loc = LockLocation();
  if (!loc || loc->object == nullptr) {
    return;
  }
  auto board = loc->LockBoard();
  auto* parent_mw = BoardOrNull(*this);
  if (!board || !parent_mw) return;  // pointer-owned: no board, no connections
  auto& toys = parent_mw->toys;
  auto& toy = ToyForObject();
  loc->object->Each<Argument>([&](Argument arg) {
    float autoconnect_radius = arg.table->autoconnect_radius;
    if (autoconnect_radius <= 0) {
      return LoopControl::Continue;
    }

    auto start = toy.ArgStart(*arg.table, parent_mw);

    // Find the current distance & target of this connection
    // Use optional to distinguish "no connection" from "top-level connection" (nullptr)
    std::optional<Interface::Table*> old_iface;
    float old_dist2 = HUGE_VALF;
    if (auto end = arg.Find()) {
      old_iface = end.Get();
      if (auto* end_loc = board->LocationOrNull(*end.Owner<Object>())) {
        Vec<Vec2AndDir> to_positions;
        auto& end_toy = toys.FindOrMake(*end_loc, parent_mw).ToyForObject();
        end_toy.ConnectionPositions(to_positions);
        auto other_up = TransformBetween(end_toy, *parent_mw);
        for (auto& to : to_positions) {
          Vec2 to_pos = other_up.mapPoint(to.pos);
          float dist2 = LengthSquared(start.pos - to_pos);
          if (dist2 <= old_dist2) {
            old_dist2 = dist2;
          }
        }
      }
    }

    // Find the nearest compatible interface
    float new_dist2 = autoconnect_radius * autoconnect_radius;
    ObjectToy* new_toy = nullptr;
    std::optional<Interface::Table*> new_iface;
    parent_mw->NearbyCandidates(
        *loc, *arg.table, autoconnect_radius,
        [&](ObjectToy& toy, Interface::Table* iface, Vec<Vec2AndDir>& to_points) {
          auto other_up = TransformBetween(toy, *parent_mw);
          for (auto& to : to_points) {
            Vec2 to_pos = other_up.mapPoint(to.pos);
            float dist2 = LengthSquared(start.pos - to_pos);
            if (dist2 <= new_dist2) {
              new_dist2 = dist2;
              new_toy = &toy;
              new_iface = iface;
            }
          }
        });

    if (new_iface == old_iface) {
      return LoopControl::Continue;
    }
    if (new_toy && new_iface) {
      auto end_obj = new_toy->LockOwner<Object>();
      arg.Connect(Interface(end_obj.Get(), *new_iface));
    } else {
      arg.Disconnect();
    }
    return LoopControl::Continue;
  });

  // Now check other locations & their arguments that might want to connect to this location

  auto here_up = TransformBetween(toy, *parent_mw);
  Vec<Vec2AndDir> to_points;
  toy.ConnectionPositions(to_points);
  for (auto& to : to_points) {
    to.pos = here_up.mapPoint(to.pos);
  }

  auto lock = std::lock_guard(vm.mutex);
  for (auto& other : board->locations) {
    if (other.get() == loc.get()) {
      continue;
    }
    auto& other_widget = toys.FindOrMake(*other, parent_mw).ToyForObject();
    auto other_up = TransformBetween(other_widget, *parent_mw);
    other->object->Each<Argument>([&](Argument arg) {
      float autoconnect_radius = arg.table->autoconnect_radius;
      if (autoconnect_radius <= 0) {
        return LoopControl::Continue;
      }
      auto this_iface_opt = arg.CanConnect(*loc->object);
      if (!this_iface_opt) {
        return LoopControl::Continue;  // `this` location can't be connected to `other`s `arg`
      }
      Interface::Table* this_iface = *this_iface_opt;

      if (auto arg_widget = toys.FindOrNull(arg)) {
        arg_widget->WakeAnimation();
      }

      auto start = other_widget.ArgStart(*arg.table);
      start.pos = other_up.mapPoint(start.pos);

      // Find the current distance & target of this connection
      // Use optional to distinguish "no connection" from "top-level connection" (nullptr)
      std::optional<Interface::Table*> old_iface;
      float old_dist2 = HUGE_VALF;
      if (auto end = arg.Find()) {
        old_iface = end.Get();
        if (auto* end_loc = board->LocationOrNull(*end.Owner<Object>())) {
          Vec<Vec2AndDir> to_positions;
          auto& end_toy = toys.FindOrMake(*end_loc, parent_mw).ToyForObject();
          end_toy.ConnectionPositions(to_positions);
          auto to_up = TransformBetween(end_toy, *parent_mw);
          for (auto& to : to_positions) {
            Vec2 to_pos = to_up.mapPoint(to.pos);
            float dist2 = LengthSquared(start.pos - to_pos);
            if (dist2 <= old_dist2) {
              old_dist2 = dist2;
            }
          }
        }
      }

      // Find the new distance & target
      float radius2 = autoconnect_radius * autoconnect_radius;
      bool old_is_here = old_iface.has_value() && *old_iface == this_iface;
      float new_dist2 = old_is_here ? radius2 : std::min(radius2, old_dist2);
      std::optional<Interface::Table*> new_iface = old_is_here ? std::nullopt : old_iface;
      for (auto& to : to_points) {
        float dist2 = LengthSquared(start.pos - to.pos);
        if (dist2 <= new_dist2) {
          new_dist2 = dist2;
          new_iface = this_iface;
        }
      }

      if (new_iface == old_iface) {
        return LoopControl::Continue;
      }
      if (new_iface) {
        arg.Connect(Interface(loc->object.Get(), *new_iface));
      } else {
        arg.Disconnect();
      }
      return LoopControl::Continue;
    });
  }
}

///////////////////////////////////////////////////////////////////////////////
// Free functions
///////////////////////////////////////////////////////////////////////////////

void AppendObscurers(Location* loc, Location* other_end, Vec<ui::Widget*>& wanted) {
  if (!loc || !loc->widget) return;
  ui::Widget* other_widget = other_end && other_end->widget ? other_end->widget.Get() : nullptr;
  for (LocationWidget& above : loc->widget->overlapping_above) {
    if (&above == other_widget) continue;
    if (std::find(wanted.begin(), wanted.end(), &above) == wanted.end()) {
      wanted.push_back(&above);
    }
  }
}

void PositionBelow(Location& origin, Location& below) {
  auto m = origin.LockBoard();
  if (!m) return;
  auto lock = std::lock_guard(vm.mutex);
  Size origin_index = SIZE_MAX;
  Size below_index = SIZE_MAX;
  for (Size i = 0; i < m->locations.size(); i++) {
    if (m->locations[i].get() == &origin) {
      origin_index = i;
      if (below_index != SIZE_MAX) {
        break;
      }
    }
    if (m->locations[i].get() == &below) {
      below_index = i;
      if (origin_index != SIZE_MAX) {
        break;
      }
    }
  }
  if (origin_index > below_index) {
    std::swap(m->locations[origin_index], m->locations[below_index]);
    m->WakeToys();
  }
}

Vec2 PositionAhead(Location& origin, const Argument::Table& arg, const ObjectToy& target_widget) {
  auto& origin_toy = origin.widget->ToyForObject();
  auto origin_shape = origin_toy.Shape();           // origin's local coordinates
  Vec2AndDir arg_start = origin_toy.ArgStart(arg);  // origin's local coordinates
  Vec2 drop_point;

  // Construct a matrix that transforms from the origin's local coordinates to the canvas
  // coordinates. Normally this could be done with TransformUp but that would include the animation.
  // We don't want to include the animation when placing objects around.
  {
    SkMatrix m = Location::ToMatrix(origin.Position(*origin.widget), origin.Scale(*origin.widget),
                                    origin.widget->LocalAnchor());
    if (auto intersection = Raycast(origin_shape, arg_start)) {
      // Try to drop the target location so that it overlaps with the origin shape by 1mm.
      drop_point = m.mapPoint(*intersection - Vec2::Polar(arg_start.dir, 1_mm));
    } else {
      // Otherwise put it 3cm ahead of the argument start point.
      drop_point = m.mapPoint(arg_start.pos + Vec2::Polar(arg_start.dir, 3_cm));
    }
  }

  // Pick the position that allows the cable to come in most horizontally (left to right).
  Vec2 best_connector_pos = Vec2(0, 0);
  float best_angle_diff = 100;
  Vec<Vec2AndDir> connector_positions;
  target_widget.ConnectionPositions(connector_positions);
  for (auto& pos : connector_positions) {
    float angle_diff = (pos.dir - arg_start.dir).ToRadians();
    if (fabs(angle_diff) < fabs(best_angle_diff)) {
      best_connector_pos = pos.pos;
      best_angle_diff = angle_diff;
    }
  }
  {
    SkMatrix m =
        Location::ToMatrix(Vec2{}, target_widget.GetBaseScale(), target_widget.LocalAnchor());
    best_connector_pos = m.mapPoint(best_connector_pos);
  }

  return RoundToMilimeters(drop_point - best_connector_pos);
}

Vec2 PositionBeside(Location& origin, Location& target, const ObjectToy& target_widget) {
  constexpr float kGap = 8_mm;
  constexpr float kStep = 12_mm;
  auto& origin_toy = origin.widget->ToyForObject();
  SkMatrix origin_m = Location::ToMatrix(
      origin.Position(*origin.widget), origin.Scale(*origin.widget), origin.widget->LocalAnchor());
  Rect origin_bounds = origin_m.mapRect(origin_toy.CoarseBounds().rect);
  SkMatrix target_m =
      Location::ToMatrix(Vec2{}, target_widget.GetBaseScale(), target_widget.LocalAnchor());
  Rect target_bounds = target_m.mapRect(target_widget.CoarseBounds().rect);
  Vec2 pos =
      Vec2(origin_bounds.right + kGap - target_bounds.left, origin_bounds.top - target_bounds.top);
  if (auto board = origin.LockBoard()) {
    auto lock = std::lock_guard(vm.mutex);
    for (int tries = 0; tries < 16; ++tries) {
      bool occupied = false;
      for (auto& loc : board->locations) {
        if (loc.get() == &origin || loc.get() == &target) continue;
        if (Length(loc->PeekPosition() - pos) < kStep / 2) {
          occupied = true;
          break;
        }
      }
      if (!occupied) break;
      pos.y -= kStep;
    }
  }
  return RoundToMilimeters(pos);
}

void AnimateGrowFrom(Location& source, Location& grown) {
  // No-op for now. Might implement this eventually.
  // Objects appearing at their final destination isn't looking that bad actually.
  //
  // The plan for this function is to put a WeakPtr in grown, pointing at source - so that
  // LocationWidget can initialize its position at grown's position, and then animate move towards
  // its own target position.
}

void LocationWidget::OnReparent(ui::Widget& new_parent, SkM44& fix) {
  auto& toy = ToyForObject();
  toy.local_to_parent.postConcat(fix);
  auto fix33 = fix.asM33();
  fix33.mapVector(position_vel);
  fix33.mapRadius(scale_vel);
}

void LocationWidget::OnChildReparentedAway(ui::Widget& child) {
  // Reparent already dropped `child` from our stack; just forget the cached pointer.
  if (toy.Get() == &child) {
    toy = nullptr;
  }
}
}  // namespace automat
