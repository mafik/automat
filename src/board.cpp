// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "board.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/pathops/SkPathOps.h>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "argument.hpp"
#include "control_flow.hpp"
#include "drag_action.hpp"
#include "embedded.hpp"
#include "global_resources.hpp"
#include "location.hpp"
#include "math.hpp"
#include "root_widget.hpp"
#include "sync.hpp"
#include "textures.hpp"
#include "ui_connection_widget.hpp"
#include "vm.hpp"

using namespace std;

namespace automat {

Board::Board() {}

std::unique_ptr<ObjectToy> Board::MakeToy(ui::Widget* parent) {
  return std::make_unique<BoardWidget>(parent, *this);
}

void Board::SerializeState(ObjectSerializer& writer) const {
  writer.Key("position");
  writer.StartArray();
  writer.Double(round(position.x * 1000000.) / 1000000.);
  writer.Double(round(position.y * 1000000.) / 1000000.);
  writer.EndArray();
  if (!locations.empty()) {
    writer.Key("locations");
    writer.StartObject();

    // Serialize the locations.
    for (auto& location : locations) {
      auto& name = writer.ResolveName(*location->object);
      writer.Key(name);
      writer.StartArray();
      Vec2 pos = location->PeekPosition();
      writer.Double(round(pos.x * 1000000.) / 1000000.);  // round to 6 decimal places
      writer.Double(round(pos.y * 1000000.) / 1000000.);
      writer.EndArray();
    }
    writer.EndObject();
  }
}

bool Board::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "position") {
    for (auto i : ArrayView(d, status)) {
      if (i == 0) {
        d.Get(position.x, status);
      } else if (i == 1) {
        d.Get(position.y, status);
      } else {
        d.Skip();
      }
    }
  } else if (key == "locations") {
    for (auto& object_name : ObjectView(d, status)) {
      auto* object = d.LookupObject(object_name);

      auto& loc = CreateEmpty();
      {  // Place the new location below all the others.
        locations.emplace_back(std::move(locations.front()));
        locations.pop_front();
      }
      loc.InsertHere(object->AcquirePtr());

      // Read the [x, y] position array
      auto& direct = *std::get_if<Location::Direct>(&loc.placement);
      for (auto i : ArrayView(d, status)) {
        if (i == 0) {
          d.Get(direct.position.x, status);
        } else if (i == 1) {
          d.Get(direct.position.y, status);
        } else {
          d.Skip();
        }
      }
    }
  } else {
    return false;
  }
  if (!OK(status)) {
    ReportError(status.ToStr());
  }
  return true;
}

Ptr<Location> Board::Extract(Location& location) {
  auto lock = std::lock_guard(vm.mutex);
  auto it = std::find_if(locations.begin(), locations.end(),
                         [&location](const auto& l) { return l.get() == &location; });
  if (it != locations.end()) {
    auto result = std::move(*it);
    locations.erase(it);
    result->board = {};
    WakeToys();
    return result;
  }
  return nullptr;
}

void Board::MoveToTop(Location& location) {
  auto lock = std::lock_guard(vm.mutex);
  auto it = std::find_if(locations.begin(), locations.end(),
                         [&location](const auto& l) { return l.get() == &location; });
  if (it == locations.end() || it == locations.begin()) return;
  auto ptr = std::move(*it);
  locations.erase(it);
  locations.emplace_front(std::move(ptr));
  WakeToys();
}

Location* Board::LocationOrNull(Object& object) {
  auto lock = std::lock_guard(vm.mutex);
  for (auto& loc : locations) {
    if (loc->object.get() == &object) {
      return loc.get();
    }
  }
  return nullptr;
}

Location& Board::CreateEmpty() {
  auto lock = std::lock_guard(vm.mutex);
  auto& it = locations.emplace_front(new Location(AcquireWeakPtr()));
  Location* h = it.get();
  WakeToys();
  return *h;
}

BoardWidget::BoardWidget(ui::Widget* parent, Board& board) : ObjectToy(parent, board) {
  parent->layers.OrderInside(this);
}

BoardWidget* BoardOrNull(const ui::Widget& widget) {
  for (ui::Widget* w = const_cast<ui::Widget*>(&widget); w; w = w->parent) {
    if (auto* bw = dynamic_cast<BoardWidget*>(w)) return bw;
  }
  return nullptr;
}

SkPath BoardWidget::Shape() const { return SkPath::Rect(Rect::MakeCenterZero(100_cm, 100_cm)); }

SkPath BoardWidget::SubtreeShape() const { return shape; }

// Turns the background green - to make it easier to isolate elements of Automat in screenshots.
constexpr bool kGreenScreen = false;

SkPaint& GetBackgroundPaint(float px_per_m) {
  if constexpr (kGreenScreen) {
    static SkPaint paint;
    paint.setColor(SK_ColorGREEN);
    return paint;
  }
  static PersistentImage bg =
      PersistentImage::MakeFromAsset(embedded::assets_bg_webp, PersistentImage::MakeArgs{
                                                                   .height = 100_cm,
                                                               });
  Status status;
  static auto shader = resources::CompileShader(embedded::assets_bg_sksl, status);
  if (!OK(status)) {
    ERROR << status;
    return bg.paint;
  }
  static SkPaint paint;
  SkRuntimeEffectBuilder builder(shader);
  builder.uniform("px_per_m") = px_per_m;
  builder.uniform("background_px") = (float)bg.heightPx();
  builder.child("background_image") = *bg.shader;
  const int kThumbSize = 64;
  auto thumb_info = (*bg.image)->imageInfo().makeWH(kThumbSize, kThumbSize);
  static auto thumb_image = (*bg.image)->makeScaled(thumb_info, kDefaultSamplingOptions);
  static auto thumb_shader = thumb_image->makeShader(
      kDefaultSamplingOptions,
      SkMatrix::Scale(1. / kThumbSize, -1. / kThumbSize).postTranslate(0, 1));
  builder.child("background_thumb") = thumb_shader;
  paint.setShader(builder.makeShader());
  return paint;
}

ui::Tock BoardWidget::Tick(time::Timer& timer) {
  auto board = LockBoard();
  if (!board) return {};
  auto lock = std::lock_guard(vm.mutex);

  for (auto& loc : board->locations) {
    auto& lw = toys.FindOrMake(*loc, this);
    lw.local_to_parent = SkM44();
    lw.ToyForObject();
    loc->object->Each<Argument>([&](Argument arg) {
      if (auto syncable = dyn_cast<Syncable>(arg)) {
        if (arg.IsConnected()) {
          auto* gear_obj = arg.Find().Owner<Object>();
          if (gear_obj && board->LocationOrNull(*gear_obj)) {
            toys.FindOrMake(syncable, this).local_to_parent = SkM44();
          }
        }
      } else {
        bool visible = true;
        if (arg.IsConnected()) {
          auto* end_obj = arg.Find().Owner<Object>();
          visible = end_obj && (end_obj == loc->object.get() || board->LocationOrNull(*end_obj));
        }
        if (visible) {
          toys.FindOrMake(arg, this).local_to_parent = SkM44();
        }
      }
      return LoopControl::Continue;
    });
    layers.OrderAbove(&lw);
  }

  for (auto& loc : board->locations) {
    auto& lw = toys.FindOrMake(*loc, this);
    loc->object->Each<Argument>([&](Argument arg) {
      auto* conn = toys.FindOrNull(arg);
      if (!conn) return LoopControl::Continue;
      ui::Widget* higher = &lw;
      auto end = arg.Find();
      if (auto* end_obj = end.Owner<Object>()) {
        if (auto* end_loc = board->LocationOrNull(*end_obj)) {
          if (auto* end_lw = toys.FindOrNull(*end_loc)) {
            if (end_lw->IsAbove(*higher)) higher = end_lw;
          }
        }
      }
      layers.OrderAbove(conn, higher);
      return LoopControl::Continue;
    });
  }

  bool overlaps_dirty = false;
  int z = 0;
  for (auto& loc : board->locations) {
    if (auto* lw = toys.FindOrNull(*loc)) {
      uint32_t genid = lw->subtree_shape.getGenerationID();
      if (lw->overlap_genid != genid || lw->overlap_zindex != z) {
        lw->overlap_genid = genid;
        lw->overlap_zindex = z;
        overlaps_dirty = true;
      }
    }
    ++z;
  }
  if (overlaps_dirty) {
    RebuildOverlaps(*board);
  }
  return {};
}

void BoardWidget::RebuildOverlaps(Board& board) {
  Vec<LocationWidget*> lws;
  Vec<Location*> locs;
  for (auto& loc : board.locations) {
    if (auto* lw = toys.FindOrNull(*loc)) {
      lws.push_back(lw);
      locs.push_back(loc.get());
    }
  }
  int n = lws.size();
  Vec<Vec<LocationWidget*>> old_above(n);
  for (int i = 0; i < n; ++i) {
    for (LocationWidget& above : lws[i]->overlapping_above) {
      old_above[i].push_back(&above);
    }
    lws[i]->overlapping_above.Clear();
    lws[i]->overlapping_below.Clear();
  }
  for (int upper = 0; upper < n; ++upper) {
    for (int lower = upper + 1; lower < n; ++lower) {
      SkPath intersection;
      if (Op(lws[upper]->subtree_shape, lws[lower]->subtree_shape, kIntersect_SkPathOp,
             &intersection) &&
          intersection.countVerbs() > 0) {
        lws[lower]->overlapping_above.Add(*lws[upper]);
        lws[upper]->overlapping_below.Add(*lws[lower]);
      }
    }
  }
  for (int i = 0; i < n; ++i) {
    Optional<Rect> bounds;
    if (lws[i]->subtree_draw_bounds && !lws[i]->subtree_draw_bounds->sk.isEmpty()) {
      bounds = *lws[i]->subtree_draw_bounds;
    }
    for (LocationWidget& above : lws[i]->overlapping_above) {
      if (above.stack_draw_bounds.sk.isEmpty()) continue;
      if (bounds.has_value()) {
        bounds->ExpandToInclude(above.stack_draw_bounds);
      } else {
        bounds = above.stack_draw_bounds;
      }
    }
    lws[i]->stack_draw_bounds = bounds.value_or(Rect{});
  }
  for (int i = 0; i < n; ++i) {
    Vec<LocationWidget*> now_above;
    for (LocationWidget& above : lws[i]->overlapping_above) {
      now_above.push_back(&above);
    }
    if (now_above != old_above[i]) {
      locs[i]->InvalidateConnectionWidgets(true, false);
    }
  }
}

void BoardWidget::Draw(SkCanvas& canvas) const {
  auto shape = Shape();
  float px_per_m = canvas.getLocalToDeviceAs3x3().mapRadius(1);
  SkPaint background_paint = GetBackgroundPaint(px_per_m);
  canvas.drawPath(shape, background_paint);
  SkPaint border_paint;
  border_paint.setColor("#404040"_color);
  border_paint.setStyle(SkPaint::kStroke_Style);
  canvas.drawPath(shape, border_paint);
  BakeChildren(canvas);  // nothing gets baked actually
}

SkMatrix BoardWidget::DropSnap(const Rect& rect_ref, Vec2 bounds_origin, Vec2* fixed_point) {
  Rect rect = rect_ref;
  SkMatrix matrix;
  Vec2 grid_snap = RoundToMilimeters(bounds_origin) - bounds_origin;
  matrix.postTranslate(grid_snap.x, grid_snap.y);
  matrix.mapRectScaleTranslate(&rect.sk, rect.sk);
  if (rect.left < -50_cm) {
    matrix.postTranslate(-rect.left - 50_cm, 0);
  }
  if (rect.right > 50_cm) {
    matrix.postTranslate(50_cm - rect.right, 0);
  }
  if (rect.bottom < -50_cm) {
    matrix.postTranslate(0, -rect.bottom - 50_cm);
  }
  if (rect.top > 50_cm) {
    matrix.postTranslate(0, 50_cm - rect.top);
  }
  return matrix;
}

void BoardWidget::DropLocation(Ptr<Location>&& l) {
  auto board = LockBoard();
  if (!board) return;
  l->board = board->AcquireWeakPtr();
  Location* dropped;
  {
    auto lock = std::lock_guard(vm.mutex);
    board->locations.insert(board->locations.begin(), std::move(l));
    dropped = board->locations.front().get();
  }
  WakeAnimation();
  audio::Play(embedded::assets_SFX_canvas_drop_wav);
  if (dropped->widget && dropped->widget->parent != this) {  // TODO: FindOrNull
    dropped->widget->Reparent(*this);
  } else if (!dropped->widget) {
    toys.FindOrMake(*dropped, this);
  }
  dropped->object->ForEachToy([](ui::RootWidget&, automat::Toy& w) { w.RedrawThisFrame(); });
}

void BoardWidget::ConnectAtPoint(Argument arg, Vec2 point) {
  auto board = LockBoard();
  if (!board) return;
  auto lock = std::lock_guard(vm.mutex);
  bool connected = false;
  Str refusal;  // the oracle's reason for the first end that matched in kind but refused
  auto TryConnect = [&](Interface end) {
    if (connected) return;
    Status status;
    arg.CanConnect(end, status);
    if (OK(status)) {
      arg.Connect(end);
      connected = true;
    } else if (refusal.empty() && arg.table->end_kind_matches(arg, end) && status.entry) {
      // The entry text alone - ToStr would append source locations.
      refusal = status.entry->message;
    }
  };
  for (auto& loc : board->locations) {
    auto& lw = toys.FindOrMake(*loc, this);
    Vec2 local_point = (point - loc->Position(lw)) / loc->Scale(lw);
    SkPath shape = lw.ToyForObject().Shape();
    if (!shape.contains(local_point.x, local_point.y)) {
      continue;
    }
    auto& obj = *loc->object;
    TryConnect(Interface(obj));
    if (connected) return;
    obj.Interfaces([&](Interface iface) {
      TryConnect(iface);
      if (connected) {
        return LoopControl::Break;
      }
      return LoopControl::Continue;
    });
  }
  if (!connected && !refusal.empty()) {
    if (auto* widget = dynamic_cast<ui::ConnectionWidget*>(toys.FindOrNull(arg))) {
      widget->ShowRefusal(std::move(refusal));
    }
  }
}

void* BoardWidget::Nearby(Vec2 start, float radius, std::function<void*(Location&)> callback) {
  auto board = LockBoard();
  if (!board) return nullptr;
  auto lock = std::lock_guard(vm.mutex);
  float radius2 = radius * radius;
  for (auto& loc : board->locations) {
    auto& lw = toys.FindOrMake(*loc, this);
    auto dist2 = (loc->object ? lw.ToyForObject().CoarseBounds().rect : Rect{})
                     .MoveBy(loc->Position(lw))
                     .DistanceSquared(start);
    if (dist2 > radius2) {
      continue;
    }
    if (auto ret = callback(*loc)) {
      return ret;
    }
  }
  return nullptr;
}

void BoardWidget::NearbyCandidates(
    Location& here, Argument::Table& arg, float radius,
    std::function<void(ObjectToy&, Interface::Table*, Vec<Vec2AndDir>&)> callback) {
  Argument bound(*here.object, arg);
  Vec2 center = toys.FindOrMake(here, this).ToyForObject().ArgStart(arg, this).pos;
  Nearby(center, radius, [&](Location& other) -> void* {
    if (&other == &here) {
      return nullptr;
    }
    auto iface = bound.CanConnect(*other.object);
    if (!iface) {
      return nullptr;
    }
    auto& toy = toys.FindOrMake(other, this).ToyForObject();
    Vec<Vec2AndDir> to_points;
    toy.ConnectionPositions(to_points);
    callback(toy, *iface, to_points);
    return nullptr;
  });
}

void BoardWidget::ForStack(Location& base, std::function<void(Location&, int index)> callback) {
  auto board = LockBoard();
  if (!board) return;
  auto lock = std::lock_guard(vm.mutex);
  std::unordered_map<LocationWidget*, std::pair<Location*, int>> index_of;
  int index = 0;
  for (auto& loc : board->locations) {
    if (auto* lw = toys.FindOrNull(*loc)) {
      index_of[lw] = {loc.get(), index};
    }
    ++index;
  }
  auto* base_lw = toys.FindOrNull(base);
  auto base_it = index_of.find(base_lw);
  if (!base_lw || base_it == index_of.end()) return;
  Vec<LocationWidget*> pile{base_lw};
  std::unordered_set<LocationWidget*> seen{base_lw};
  for (size_t i = 0; i < pile.size(); ++i) {
    for (LocationWidget& above : pile[i]->overlapping_above) {
      if (seen.insert(&above).second && index_of.contains(&above)) {
        pile.push_back(&above);
      }
    }
  }
  std::sort(pile.begin(), pile.end(), [&](LocationWidget* a, LocationWidget* b) {
    return index_of[a].second > index_of[b].second;
  });
  for (auto* lw : pile) {
    auto [loc, idx] = index_of[lw];
    callback(*loc, idx);
  }
}

SkPath BoardWidget::StackShape(Location& base) {
  SkPath stack_shape;
  ForStack(base, [&](Location& loc, int) {
    if (&loc != &base) {
      Op(stack_shape, loc.widget->subtree_shape, kUnion_SkPathOp, &stack_shape);
    }
  });
  return stack_shape;
}

Vec<Ptr<Location>> BoardWidget::DragStack(Location& base) {
  Vec<Ptr<Location>> result;
  ForStack(base,
           [&](Location& loc, int index) { result.insert(result.begin(), loc.AcquirePtr()); });
  if (!result.empty()) {
    RaiseStack(base);
    audio::Play(embedded::assets_SFX_canvas_pick_wav);
  }
  return result;
}

Vec<Ptr<Location>> BoardWidget::CloneStack(Location& base) {
  auto board = LockBoard();
  if (!board) return {};
  Vec<Location*> originals;
  ForStack(base, [&](Location& loc, int) { originals.insert(originals.begin(), &loc); });
  if (originals.empty()) return {};

  Vec<Ptr<Location>> result;
  result.reserve(originals.size());
  std::unordered_map<Object*, Object*> orig_to_clone;
  for (auto* orig : originals) {
    auto clone_loc = orig->Clone().Cast<Location>();
    if (orig->object) {
      clone_loc->InsertHere(orig->object->Clone());
      orig_to_clone[orig->object.get()] = clone_loc->object.get();
    }
    result.push_back(std::move(clone_loc));
  }

  // Arguments within stack should stay within the cloned stack.
  for (auto& clone_loc : result) {
    if (!clone_loc->object) continue;
    clone_loc->object->Each<Argument>([&](Argument arg) {
      if (!arg.IsConnected()) return LoopControl::Continue;
      auto target = arg.Find();
      auto* target_owner = target.Owner<Object>();
      auto it = orig_to_clone.find(target_owner);
      if (it != orig_to_clone.end()) {
        arg.Connect(Interface(it->second, target.Get()));
      }
      return LoopControl::Continue;
    });
  }

  // Prevent default toy appearance animation by pre-creating toys for the clones.
  // Anchoring to the original toys makes the new toys appear in the same place as the originals.
  auto& root = FindRootWidget();
  for (size_t i = 0; i < originals.size(); ++i) {
    auto* orig_lw = originals[i]->widget.Get();
    if (!orig_lw || !orig_lw->toy || !result[i]->object) continue;
    root.toys.FindOrMake(*result[i]->object, orig_lw->toy.Get());
  }

  WakeAnimation();
  return result;
}

void BoardWidget::RaiseStack(Location& base) {
  auto board = LockBoard();
  if (!board) return;
  auto lock = std::lock_guard(vm.mutex);
  Vec<int> stack_indices;
  ForStack(base, [&](Location&, int index) { stack_indices.push_back(index); });
  if (stack_indices.empty()) return;
  board->WakeToys();

  Vec<Ptr<Location>> stack;
  // Indices are in decreasing order (highest first), so erasing in order is safe.
  for (int idx : stack_indices) {
    stack.insert(stack.begin(), std::move(board->locations[idx]));
    board->locations.erase(board->locations.begin() + idx);
  }

  for (int i = stack.size() - 1; i >= 0; --i) {
    board->locations.insert(board->locations.begin(), std::move(stack[i]));
  }
}

struct MoveBoardAction : Action {
  Ptr<Board> board;
  Vec2 grab_offset;

  MoveBoardAction(ui::Pointer& pointer, Ptr<Board>&& board_arg)
      : Action(pointer), board(std::move(board_arg)) {
    grab_offset = board->position - pointer.PositionOnCanvas();
    auto lock = std::lock_guard(vm.mutex);
    auto it = std::find(vm.boards.begin(), vm.boards.end(), board);
    if (it != vm.boards.end()) {
      std::rotate(vm.boards.begin(), it, it + 1);
    }
    audio::Play(embedded::assets_SFX_canvas_pick_wav);
  }

  ~MoveBoardAction() override { audio::Play(embedded::assets_SFX_canvas_drop_wav); }

  void Update() override {
    board->position = pointer.PositionOnCanvas() + grab_offset;
    board->WakeToys();
    pointer.root_widget.WakeAnimation();
  }
};

struct MoveBoardOption : TextOption {
  WeakPtr<Board> weak;
  MoveBoardOption(WeakPtr<Board> weak) : TextOption("Move"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<MoveBoardOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto board = weak.Lock()) {
      return std::make_unique<MoveBoardAction>(pointer, std::move(board));
    }
    return nullptr;
  }
  Dir PreferredDir() const override { return N; }
};

void BoardWidget::VisitOptions(const OptionsVisitor& visitor) const {
  if (auto board = LockBoard()) {
    MoveBoardOption move{board->AcquireWeakPtr()};
    visitor(move);
  }
}

std::unique_ptr<Action> BoardWidget::FindAction(ui::Pointer& pointer, ui::ActionTrigger btn) {
  if (btn == ui::PointerButton::Right) {
    return OpenMenu(pointer);
  }
  return nullptr;
}

}  // namespace automat
