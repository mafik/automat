// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "board.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPictureRecorder.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/pathops/SkPathOps.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "argument.hpp"
#include "control_flow.hpp"
#include "drag_action.hpp"
#include "drawing.hpp"
#include "embedded.hpp"
#include "engine.hpp"
#include "global_resources.hpp"
#include "library_file.hpp"
#include "location.hpp"
#include "math.hpp"
#include "menu.hpp"
#include "root_widget.hpp"
#include "status.hpp"
#include "textures.hpp"
#include "ui_connection_widget.hpp"

using namespace std;

namespace automat {

Board::Board() {}

Board::~Board() {}

std::unique_ptr<ObjectToy> Board::MakeToy(ui::Widget* parent) {
  return std::make_unique<BoardWidget>(parent, *this);
}

void Board::SerializeState(ObjectSerializer& writer) const {
  writer.Key("position");
  writer.StartArray();
  writer.Double(round(position.x * 1000000.) / 1000000.);
  writer.Double(round(position.y * 1000000.) / 1000000.);
  writer.EndArray();
  writer.Key("size");
  writer.StartArray();
  writer.Double(round(size.width * 1000000.) / 1000000.);
  writer.Double(round(size.height * 1000000.) / 1000000.);
  writer.EndArray();
  writer.Key("frame_visible");
  writer.Bool(frame_visible);
  Ptr<library::File> file;
  {
    auto lock = std::lock_guard(wallpaper_mutex);
    file = wallpaper;
  }
  if (file) {
    writer.Key("wallpaper");
    writer.String(writer.ResolveName(*file));
  }
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
  } else if (key == "size") {
    for (auto i : ArrayView(d, status)) {
      if (i == 0) {
        d.Get(size.width, status);
      } else if (i == 1) {
        d.Get(size.height, status);
      } else {
        d.Skip();
      }
    }
  } else if (key == "frame_visible") {
    Status status;
    d.Get(frame_visible, status);
    if (!OK(status)) {
      d.Skip();
      frame_visible = false;
    }
  } else if (key == "wallpaper") {
    Str name;
    d.Get(name, status);
    if (auto* file = dynamic_cast<library::File*>(d.LookupObject(name))) {
      SetWallpaper(file->AcquirePtr());
    }
  } else if (key == "locations") {
    for (auto& object_name : ObjectView(d, status)) {
      auto* object = d.LookupObject(object_name);
      if (object == nullptr) {
        ERROR << "Warning: While loading a board tried to place \"" << object_name
              << "\" but it wasn't found in the saved state. This may have happened due to changes "
                 "in Automat or some kind of corruption of automat_state.json. Maybe an earlier "
                 "warning may provide more information.";
        d.Skip();
        continue;
      }

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

float Board::PxToMetric() const {
  // TODO: convert pixels to metric units (grab background, position it, figure out effective dpi)
  return 1_m / 3840;
}

bool Board::resizable_Impl::ResizePx(int top, int right, int bottom, int left) {
  auto px_to_m = obj->PxToMetric();
  return ResizeM(Rect(left * px_to_m, bottom * px_to_m, right * px_to_m, top * px_to_m));
}

bool Board::resizable_Impl::ResizeM(Rect grow) {
  obj->position.y += (grow.top - grow.bottom) / 2;
  obj->position.x += (grow.right - grow.left) / 2;
  obj->size.y += (grow.top + grow.bottom) / 2;
  obj->size.x += (grow.right + grow.left) / 2;
  obj->size.y = max(obj->size.y, 10_cm);
  obj->size.x = max(obj->size.y, 10_cm);
  return true;
}

Ptr<Location> Board::Extract(Location& location) {
  auto lock = std::lock_guard(engine.mutex);
  auto it = std::find_if(locations.begin(), locations.end(),
                         [&location](const auto& l) { return l.Get() == &location; });
  if (it != locations.end()) {
    auto result = it->Release();
    locations.erase(it);
    result->board = {};
    WakeToys();
    return result;
  }
  return nullptr;
}

void Board::MoveToTop(Location& location) {
  auto lock = std::lock_guard(engine.mutex);
  auto it = std::find_if(locations.begin(), locations.end(),
                         [&location](const auto& l) { return l.Get() == &location; });
  if (it == locations.end() || it == locations.begin()) return;
  auto ptr = it->Release();
  locations.erase(it);
  locations.emplace_front(*this, std::move(ptr));
  WakeToys();
}

Ptr<Location> Board::LocationOrNull(Object& object) {
  for (auto& owner : object.Owners()) {
    auto location = dyn_cast<Location>(owner);
    if (location && location->board == this) return location;
  }
  return nullptr;
}

Location& Board::CreateEmpty() {
  auto lock = std::lock_guard(engine.mutex);
  auto& it = locations.emplace_front(*this, Ptr<Location>(new Location(AcquireWeakPtr())));
  Location* h = it.Get();
  WakeToys();
  return *h;
}

void Board::SetWallpaper(Ptr<library::File> file) {
  if (auto location = LocationOrNull(*file)) {
    Extract(*location);
  }
  {
    auto lock = std::lock_guard(wallpaper_mutex);
    wallpaper = std::move(file);
  }
  WakeToys();
}

static SkMatrix WallpaperTransform(const library::File& file) {
  Vec2 size = file.Size();
  float scale = 1_m / std::min(size.x, size.y);
  return SkMatrix::Scale(scale, scale);
}

constinit Interface::Table kSetWallpaper = [] {
  Interface::Table t(Interface::kMenu, "Set Wallpaper");
  t.activate = [](Span<Interface> chain, ui::Pointer&, Toy* toy) -> std::unique_ptr<Action> {
    auto* board_widget = toy ? BoardOrNull(*toy) : nullptr;
    auto board = board_widget ? board_widget->LockBoard() : nullptr;
    if (board) {
      board->SetWallpaper(static_cast<library::File&>(*chain.back().object_ptr).AcquirePtr());
    }
    return nullptr;
  };
  return t;
}();

Ptr<Object> Board::take_wallpaper_Impl::OnTake() {
  Ptr<library::File> file;
  {
    auto lock = std::lock_guard(obj->wallpaper_mutex);
    file = obj->wallpaper.Release();
  }
  obj->WakeToys();
  return file;
}

std::unique_ptr<Action> Board::take_wallpaper_Impl::OnActivate(ui::Pointer& pointer,
                                                               automat::Toy*) {
  auto* board_widget = pointer.root_widget.toys.FindOrNull(*obj);
  if (board_widget == nullptr) return nullptr;
  auto file = cast_if_present<library::File>(Take());
  if (!file) return nullptr;
  auto new_toy = file->MakeToy(board_widget);
  new_toy->local_to_parent = SkM44(WallpaperTransform(*file));
  auto* toy = new_toy.get();
  auto drag = DragNew(pointer, std::move(file), std::move(new_toy));
  auto* lw = ui::Closest<LocationWidget>(*toy);
  if (lw->parent == board_widget) {
    board_widget->layers.OrderInside(lw);
  }
  return drag;
}

std::unique_ptr<ui::Widget> Board::take_wallpaper_Impl::OnMakeIcon(ui::Widget* parent) {
  Ptr<library::File> file;
  {
    auto lock = std::lock_guard(obj->wallpaper_mutex);
    file = obj->wallpaper;
  }
  return file ? file->MakeToy(parent) : nullptr;
}

BoardWidget::BoardWidget(ui::Widget* parent, Board& board) : ObjectToy(parent, board) {
  parent->layers.OrderInside(this);
  size.value = board.size;
  frame_width.value = board.frame_visible ? 1_cm : 0_cm;
}

BoardWidget* BoardOrNull(const ui::Widget& widget) {
  for (ui::Widget* w = const_cast<ui::Widget*>(&widget); w; w = w->parent) {
    if (auto* bw = dynamic_cast<BoardWidget*>(w)) return bw;
  }
  return nullptr;
}

SkPath BoardWidget::Shape() const { return SkPath::RRect(FrameBounds()); }

SkPath BoardWidget::SubtreeShape() const { return shape; }

// Turns the background green - to make it easier to isolate elements of Automat in screenshots.
constexpr bool kGreenScreen = false;

constexpr int kThumbSize = 64;

static sk_sp<SkShader> Thumbnail(const sk_sp<SkImage>& image) {
  float m = std::min(image->width(), image->height());
  SkRect crop = SkRect::MakeXYWH((image->width() - m) / 2, (image->height() - m) / 2, m, m);
  SkPictureRecorder recorder;
  recorder.beginRecording(kThumbSize, kThumbSize)
      ->drawImageRect(image, crop, SkRect::MakeWH(kThumbSize, kThumbSize), kDefaultSamplingOptions,
                      nullptr, SkCanvas::kStrict_SrcRectConstraint);
  auto thumb = SkImages::DeferredFromPicture(recorder.finishRecordingAsPicture(),
                                             {kThumbSize, kThumbSize}, nullptr, nullptr,
                                             SkImages::BitDepth::kU8, SkColorSpace::MakeSRGB());
  return thumb->makeShader(
      kDefaultSamplingOptions,
      SkMatrix::Scale(1.f / kThumbSize, -1.f / kThumbSize).postTranslate(0, 1));
}

ui::Tock BoardWidget::Tick(time::Timer& timer) {
  auto board = LockBoard();
  if (!board) return {};
  auto lock = std::lock_guard(engine.mutex);

  ui::Tock tock;
  tock.shaping |= frame_width.SineTowards(board->frame_visible ? 1_cm : 0, timer.d, 0.4);
  tock.shaping |= size.SineTowards(board->size, timer.d, 0.4);

  Ptr<library::File> file;
  {
    auto wallpaper_lock = std::lock_guard(board->wallpaper_mutex);
    file = board->wallpaper;
  }
  sk_sp<SkImage> image = file ? file->Image() : nullptr;
  uint32_t image_id = image ? image->uniqueID() : 0;
  if (file && file->suspended) {
    tock |= Tock::Ing;
  } else if (image_id != wallpaper_image_id || !background_image) {
    wallpaper_image_id = image_id;
    if (image) {
      float m = std::min(image->width(), image->height());
      float s = 1_m / m;
      background_image =
          image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, kDefaultSamplingOptions,
                            SkMatrix::Scale(s, -s).postTranslate(0.5f - image->width() * s / 2,
                                                                 0.5f + image->height() * s / 2));
      background_thumb = Thumbnail(image);
      background_px = m;
    } else {
      static PersistentImage bg =
          PersistentImage::MakeFromAsset(embedded::assets_bg_webp, PersistentImage::MakeArgs{
                                                                       .height = 100_cm,
                                                                   });
      static sk_sp<SkShader> bg_thumb = Thumbnail(*bg.image);
      background_image = *bg.shader;
      background_thumb = bg_thumb;
      background_px = bg.heightPx();
    }
    tock |= Tock::Draw;
  }

  for (auto& loc : board->locations) {
    auto& lw = toys.FindOrMake(*loc, this);
    lw.local_to_parent = SkM44();
    auto& toy = lw.ToyForObject();
    loc->object->Each<Argument>([&](Argument arg) {
      bool visible = arg.table->visible_when_disconnected;
      if (arg.IsConnected()) {
        auto* end_obj = arg.Find().object_ptr;
        visible = end_obj && (end_obj == loc->object.Get() || board->LocationOrNull(*end_obj));
      }
      if (visible) {
        toys.FindOrMake(arg, this).local_to_parent = SkM44();
      }
      return LoopControl::Continue;
    });
    if (IsDragged(lw) && std::ranges::contains(layers.Baked(), &lw) &&
        !BgBounds().Contains(toy.local_to_parent.asM33().mapRect(toy.CoarseBounds().rect))) {
      continue;
    }
    layers.OrderAbove(&lw);
  }

  for (auto& loc : board->locations) {
    auto& lw = toys.FindOrMake(*loc, this);
    loc->object->Each<Argument>([&](Argument arg) {
      auto* conn = toys.FindOrNull(arg);
      if (!conn) return LoopControl::Continue;
      ui::Widget* higher = &lw;
      auto end = arg.Find();
      if (auto* end_obj = end.object_ptr) {
        if (auto end_loc = board->LocationOrNull(*end_obj)) {
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
  return tock;
}

void BoardWidget::RebuildOverlaps(Board& board) {
  Vec<LocationWidget*> lws;
  Vec<Location*> locs;
  for (auto& loc : board.locations) {
    if (auto* lw = toys.FindOrNull(*loc)) {
      lws.push_back(lw);
      locs.push_back(loc.Get());
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
  Rect bg_bounds = BgBounds();
  SkPaint background_paint;
  if constexpr (kGreenScreen) {
    background_paint.setColor(SK_ColorGREEN);
  } else {
    Status status;
    static auto effect = resources::CompileShader(embedded::assets_bg_sksl, status);
    if (!OK(status)) {
      ERROR << status;
    }
    if (effect) {
      SkRuntimeEffectBuilder builder(effect);
      builder.uniform("px_per_m") = canvas.getLocalToDeviceAs3x3().mapRadius(1);
      builder.uniform("background_px") = background_px;
      builder.child("background_image") = background_image;
      builder.child("background_thumb") = background_thumb;
      background_paint.setShader(builder.makeShader());
    } else {
      background_paint.setShader(background_image);
    }
  }
  canvas.drawRect(bg_bounds, background_paint);
  SkPaint border_paint;
  border_paint.setColor("#404040"_color);
  if (frame_width == 0) {
    border_paint.setStyle(SkPaint::kStroke_Style);
    border_paint.setStrokeWidth(0);
    canvas.drawRect(bg_bounds, border_paint);
  } else {
    auto frame_rect = FrameBounds();
    SetRRectShader(border_paint, frame_rect, "#f7f5c2"_color4f, "#e8b044"_color4f,
                   "#66370b"_color4f);
    canvas.drawDRRect(frame_rect, RRect::MakeSimple(bg_bounds, 0), border_paint);
  }
  canvas.save();
  canvas.clipRect(bg_bounds);
  BakeChildren(canvas);
  canvas.restore();
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
    auto lock = std::lock_guard(engine.mutex);
    board->locations.emplace(board->locations.begin(), *board, std::move(l));
    dropped = board->locations.front().Get();
  }
  WakeAnimation();
  audio::Play(embedded::assets_SFX_canvas_drop_wav);
  if (dropped->widget && dropped->widget->parent != this) {  // TODO: FindOrNull
    dropped->widget->Reparent(*this);
  } else if (!dropped->widget) {
    toys.FindOrMake(*dropped, this);
  }
  ForEachToy(*dropped->object, [](ui::RootWidget&, automat::Toy& w) { w.RedrawThisFrame(); });
}

void BoardWidget::ConnectAtPoint(Argument arg, Vec2 point) {
  auto board = LockBoard();
  if (!board) return;
  auto lock = std::lock_guard(engine.mutex);
  bool connected = false;
  Str refusal;  // the refusal reason for the first end that matched in kind but refused
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
  auto lock = std::lock_guard(engine.mutex);
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
  auto lock = std::lock_guard(engine.mutex);
  std::unordered_map<LocationWidget*, std::pair<Location*, int>> index_of;
  int index = 0;
  for (auto& loc : board->locations) {
    if (auto* lw = toys.FindOrNull(*loc)) {
      index_of[lw] = {loc.Get(), index};
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

Vec<Ptr<Location>> BoardWidget::CloneStack(Location& base, Vec<std::unique_ptr<Toy>>& toys) {
  auto board = LockBoard();
  if (!board) return {};
  Vec<Location*> originals;
  ForStack(base, [&](Location& loc, int) { originals.insert(originals.begin(), &loc); });
  if (originals.empty()) return {};

  Vec<Ptr<Location>> result;
  result.reserve(originals.size());
  std::unordered_map<Object*, Object*> orig_to_clone;
  for (auto* orig : originals) {
    auto clone_loc = cast<Location>(orig->Clone());
    if (orig->object) {
      clone_loc->InsertHere(orig->object->Clone());
      orig_to_clone[orig->object.Get()] = clone_loc->object.Get();
    }
    result.push_back(std::move(clone_loc));
  }

  // Arguments within stack should stay within the cloned stack.
  for (auto& clone_loc : result) {
    if (!clone_loc->object) continue;
    clone_loc->object->Each<Argument>([&](Argument arg) {
      if (!arg.IsConnected()) return LoopControl::Continue;
      auto target = arg.Find();
      auto* target_owner = target.object_ptr;
      auto it = orig_to_clone.find(target_owner);
      if (it != orig_to_clone.end()) {
        arg.Connect(it->second->InterfaceFromIdx(target_owner->InterfaceIdx(target.table_ptr)));
      }
      return LoopControl::Continue;
    });
  }

  // Prevent default toy appearance animation by pre-creating toys for the clones.
  // Anchoring to the original toys makes the new toys appear in the same place as the originals.
  toys.resize(result.size());
  for (size_t i = 0; i < originals.size(); ++i) {
    auto* orig_lw = originals[i]->widget.Get();
    if (!orig_lw || !orig_lw->toy || !result[i]->object) continue;
    toys[i] = result[i]->object->MakeToy(orig_lw->toy.Get());
  }

  WakeAnimation();
  return result;
}

void BoardWidget::RaiseStack(Location& base) {
  auto board = LockBoard();
  if (!board) return;
  auto lock = std::lock_guard(engine.mutex);
  Vec<int> stack_indices;
  ForStack(base, [&](Location&, int index) { stack_indices.push_back(index); });
  if (stack_indices.empty()) return;
  board->WakeToys();

  Vec<Ptr<Location>> stack;
  // Indices are in decreasing order (highest first), so erasing in order is safe.
  for (int idx : stack_indices) {
    stack.insert(stack.begin(), board->locations[idx].Release());
    board->locations.erase(board->locations.begin() + idx);
  }

  for (int i = stack.size() - 1; i >= 0; --i) {
    board->locations.emplace(board->locations.begin(), *board, std::move(stack[i]));
  }
}

struct MoveBoardAction : Action {
  Ptr<Board> board;
  Vec2 grab_offset;

  MoveBoardAction(ui::Pointer& pointer, Ptr<Board>&& board_arg)
      : Action(pointer), board(std::move(board_arg)) {
    grab_offset = board->position - pointer.PositionOnCanvas();
    auto lock = std::lock_guard(engine.mutex);
    auto it = std::find(engine.boards.begin(), engine.boards.end(), board);
    if (it != engine.boards.end()) {
      std::rotate(engine.boards.begin(), it, it + 1);
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

std::unique_ptr<Action> Board::move_Impl::OnActivate(ui::Pointer& pointer, automat::Toy*) {
  return std::make_unique<MoveBoardAction>(pointer, obj->AcquirePtr());
}

void BoardWidget::FillMenu(ui::Pointer&, Menu& menu) {
  using enum ui::Dir;
  auto board = LockBoard();
  if (!board) return;
  menu.Place(N, board->move.Bind());
  menu.Place(NE, board->toggle_frame.Bind());
  bool has_wallpaper;
  {
    auto lock = std::lock_guard(board->wallpaper_mutex);
    has_wallpaper = (bool)board->wallpaper;
  }
  if (has_wallpaper) {
    menu.Place(W, board->take_wallpaper.Bind());
  }
  menu.Place(S, Interface(*FindRootWidget().camera, ui::kCameraMenu));
}

}  // namespace automat
