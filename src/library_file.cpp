// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#include "library_file.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/pathops/SkPathOps.h>
#include <include/private/SkExif.h>

#include <algorithm>
#include <cctype>

#include "action.hpp"
#include "board.hpp"
#include "deserializer.hpp"
#include "file_import.hpp"
#include "font.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "object.hpp"
#include "pointer.hpp"
#include "tasks.hpp"
#include "textures.hpp"
#include "units.hpp"
#include "virtual_fs.hpp"
#include "window_frame.hpp"
#include "xdg_icon.hpp"

#pragma comment(lib, "skia")

namespace automat::library {

namespace {

constexpr float kIconSize = 1.5_cm;
constexpr float kMinSize = 1_mm;
constexpr float kMaxSize = 100_cm;
constexpr float kLabelHeight = ui::WindowFrame::kTitleH / 2;
constexpr Vec2 kDefaultCropSize = Vec2(25_mm, 22_mm);
constexpr float kMinCropSize = 2_mm;

Str Basename(StrView path) {
  auto slash = path.find_last_of("/\\");
  return Str(slash == StrView::npos ? path : path.substr(slash + 1));
}

Str ExtensionOf(StrView path) {
  Str name = Basename(path);
  auto dot = name.rfind('.');
  if (dot == Str::npos) return "";
  Str ext = name.substr(dot + 1);
  for (char& c : ext) c = std::tolower((unsigned char)c);
  return ext;
}

uint32_t BigEndian32(StrView s, size_t i) {
  return (U8)s[i] << 24 | (U8)s[i + 1] << 16 | (U8)s[i + 2] << 8 | (U8)s[i + 3];
}

uint32_t BigEndian16(StrView s, size_t i) { return (U8)s[i] << 8 | (U8)s[i + 1]; }

uint32_t LittleEndian32(StrView s, size_t i) {
  return (U8)s[i + 3] << 24 | (U8)s[i + 2] << 16 | (U8)s[i + 1] << 8 | (U8)s[i];
}

Optional<Vec2> ExifPixelsPerMeter(StrView tiff) {
  SkExif::Metadata metadata;
  SkExif::Parse(metadata, SkData::MakeWithoutCopy(tiff.data(), tiff.size()).get());
  if (!metadata.fXResolution || !metadata.fYResolution) return std::nullopt;
  switch (metadata.fResolutionUnit.value_or(2)) {
    case 2:
      return Vec2(*metadata.fXResolution, *metadata.fYResolution) / kMetersPerInch;
    case 3:
      return Vec2(*metadata.fXResolution, *metadata.fYResolution) * 100;
    default:
      return std::nullopt;
  }
}

Optional<Vec2> PixelsPerMeter(StrView bytes) {
  if (bytes.starts_with("\x89PNG\r\n\x1a\n")) {
    for (size_t i = 8; i + 12 <= bytes.size();) {
      size_t length = BigEndian32(bytes, i);
      StrView type = bytes.substr(i + 4, 4);
      if (i + 12 + length > bytes.size()) break;
      StrView data = bytes.substr(i + 8, length);
      if (type == "pHYs" && length >= 9 && data[8] == 1) {
        return Vec2(BigEndian32(data, 0), BigEndian32(data, 4));
      }
      if (type == "eXIf") {
        if (auto exif = ExifPixelsPerMeter(data)) return exif;
      }
      if (type == "IDAT" || type == "IEND") break;
      i += 12 + length;
    }
    return std::nullopt;
  }
  if (bytes.starts_with("\xFF\xD8")) {
    Optional<Vec2> jfif;
    for (size_t i = 2; i + 4 <= bytes.size() && (U8)bytes[i] == 0xFF;) {
      U8 marker = bytes[i + 1];
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        i += 2;
        continue;
      }
      if (marker == 0xDA || marker == 0xD9) break;
      size_t length = BigEndian16(bytes, i + 2);
      if (length < 2 || i + 2 + length > bytes.size()) break;
      StrView segment = bytes.substr(i + 4, length - 2);
      if (marker == 0xE0 && segment.size() >= 12 && segment.starts_with(StrView("JFIF\0", 5))) {
        U8 units = segment[7];
        Vec2 density(BigEndian16(segment, 8), BigEndian16(segment, 10));
        if (units == 1) jfif = density / kMetersPerInch;
        if (units == 2) jfif = density * 100;
      }
      if (marker == 0xE1 && segment.starts_with(StrView("Exif\0\0", 6))) {
        if (auto exif = ExifPixelsPerMeter(segment.substr(6))) return exif;
      }
      i += 2 + length;
    }
    return jfif;
  }
  if (bytes.starts_with("BM") && bytes.size() >= 46 && LittleEndian32(bytes, 14) >= 40) {
    int32_t x = LittleEndian32(bytes, 38), y = LittleEndian32(bytes, 42);
    if (x > 0 && y > 0) return Vec2(x, y);
    return std::nullopt;
  }
  if (bytes.starts_with("RIFF") && bytes.size() >= 12 && bytes.substr(8, 4) == "WEBP") {
    for (size_t i = 12; i + 8 <= bytes.size();) {
      StrView fourcc = bytes.substr(i, 4);
      size_t length = LittleEndian32(bytes, i + 4);
      if (i + 8 + length > bytes.size()) break;
      if (fourcc == "EXIF") {
        StrView data = bytes.substr(i + 8, length);
        if (data.starts_with(StrView("Exif\0\0", 6))) data.remove_prefix(6);
        return ExifPixelsPerMeter(data);
      }
      i += 8 + length + (length & 1);
    }
  }
  return std::nullopt;
}

Vec2 FitAspect(Vec2 size, float min_side, float max_side) {
  float longer = std::max(size.x, size.y);
  if (longer > max_side) size *= max_side / longer;
  float shorter = std::min(size.x, size.y);
  if (shorter < min_side) size *= min_side / shorter;
  return size;
}

ui::Font& LabelFont() {
  static auto font = ui::Font::MakeV2(ui::Font::GetBelanosimaRegular(), kLabelHeight);
  return *font;
}

}  // namespace

File::File() {
  base_toc[2] = &toggle_filename_tbl;
  base_toc[3] = &croppable_tbl;
  toc.store(&base_toc, std::memory_order_relaxed);
}

File::File(StrView name) : File() {
  Status status;
  path = CreateUniqueFile(AutomatDir(), name, status).str;
  if (!OK(status)) ReportError(status.ToStr());
}

File::~File() {
  if (!suspended && !path.empty()) {
    Status status;
    automat::Path(path).Unlink(status, true);
  }
}

Ptr<Object> File::Clone() const {
  auto lock = std::lock_guard(mutex);
  if (path.empty()) return MAKE_PTR(File);
  auto copy = MAKE_PTR(File, Basename(path));
  copy->Append(contents);
  copy->image = image;
  copy->icon = icon;
  copy->size = size;
  copy->show_filename = show_filename;
  copy->base_toc[1] = base_toc[1];
  (new FunctionTask(copy->AcquireWeakPtr(), [](Object& o) {
    auto& file = static_cast<File&>(o);
    Status status;
    file.FlushToFilesystem(status);
    if (!OK(status)) file.ReportError(status.ToStr());
  }))->Schedule();
  return copy;
}

void File::SerializeState(ObjectSerializer& writer) const {
  Status status;
  FlushToFilesystem(status);
  if (!OK(status)) ERROR << status.ToStr();
  auto lock = std::lock_guard(mutex);
  if (!path.empty()) {
    Str serialized = GetSerializablePath(path);
    writer.Key("path");
    writer.String(serialized.data(), serialized.size());
  }
  if (show_filename) {
    writer.Key("show_filename");
    writer.Bool(*show_filename);
  }
}

bool File::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "path") {
    Status status;
    Str serialized;
    d.Get(serialized, status);
    if (!OK(status)) return true;
    automat::Path p = DeserializePath(serialized, status);
    if (OK(status)) {
      Str new_contents = fs::real.Read(p, status);
      {
        auto lock = std::lock_guard(mutex);
        path = p.str;
        contents = std::move(new_contents);
      }
      Analyze();
    } else {
      ReportError(status.ToStr());
    }
    return true;
  }
  if (key == "show_filename") {
    Status status;
    bool value;
    d.Get(value, status);
    if (OK(status)) {
      auto lock = std::lock_guard(mutex);
      show_filename = value;
    }
    return true;
  }
  return false;
}

void File::Append(StrView bytes) {
  {
    auto lock = std::lock_guard(mutex);
    contents += bytes;
    ++contents_monitor;
  }
  WakeToys();
}

void File::Analyze() {
  {
    auto lock = std::lock_guard(mutex);
    image = DecodeTemporaryImage(contents);
    icon = nullptr;
    if (image) {
      image = image->withDefaultMipmaps();
      Vec2 pixels_per_meter = PixelsPerMeter(contents).value_or(Vec2(96 / kMetersPerInch));
      size =
          FitAspect(Vec2(image->width(), image->height()) / pixels_per_meter, kMinSize, kMaxSize);
    } else {
      icon = IconForExtension(ExtensionOf(path));
      size = Vec2(kIconSize, kIconSize);
      if (icon) {
        SkRect bounds = icon->cullRect();
        size = FitAspect(Vec2(bounds.width(), bounds.height()), 0, kIconSize);
      }
    }
    base_toc[1] = image ? &image_provider_tbl : nullptr;
    ReattachHooks();
  }
  WakeToys();
}

void File::FlushToFilesystem(Status& status) const {
  auto lock = std::lock_guard(mutex);
  if (path.empty() || contents_monitor == flushed_monitor) return;
  fs::real.Write(automat::Path(path), contents, status);
  if (OK(status)) flushed_monitor = contents_monitor;
}

void File::ToggleFilename() {
  {
    auto lock = std::lock_guard(mutex);
    show_filename = !show_filename.value_or(image == nullptr);
  }
  WakeToys();
}

Str File::Path() const {
  auto lock = std::lock_guard(mutex);
  return path;
}

Str File::Filename() const {
  auto lock = std::lock_guard(mutex);
  return Basename(path);
}

bool File::IsImage() const {
  auto lock = std::lock_guard(mutex);
  return image != nullptr;
}

bool File::ShowsFilename() const {
  auto lock = std::lock_guard(mutex);
  return show_filename.value_or(image == nullptr);
}

sk_sp<SkImage> File::Image() const {
  auto lock = std::lock_guard(mutex);
  return image;
}

sk_sp<SkPicture> File::Icon() const {
  auto lock = std::lock_guard(mutex);
  return icon;
}

Vec2 File::Size() const {
  auto lock = std::lock_guard(mutex);
  return size;
}

struct FileToy : automat::ObjectToy {
  struct DragCropHandleAction;

  sk_sp<SkImage> image;
  sk_sp<SkPicture> icon;
  Vec2 size;
  Str filename;
  bool show_filename = false;
  Rect label_bounds;
  Optional<Rect> crop;

  FileToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) { Pull(); }

  bool CenteredAtZero() const override { return true; }

  Rect Body() const {
    if (!crop) return Rect::MakeCenterZero(size.x, size.y);
    float scale = std::min(1 / crop->Width(), 1 / crop->Height());
    return Rect::MakeCenterZero(crop->Width() * size.x * scale, crop->Height() * size.y * scale);
  }

  Rect PictureRect() const {
    Rect body = Body();
    if (!crop) return body;
    Vec2 picture_size = body.Size() / crop->Size();
    Vec2 bottom_left = body.BottomLeftCorner() - crop->BottomLeftCorner() * picture_size;
    return Rect(bottom_left.x, bottom_left.y, bottom_left.x + picture_size.x,
                bottom_left.y + picture_size.y);
  }

  SkMatrix ImageToLocal() const {
    SkMatrix m = SkMatrix::RectToRect(SkRect::Make(image->dimensions()), PictureRect().sk,
                                      SkMatrix::kFill_ScaleToFit);
    m.preTranslate(0, image->height() / 2.f);
    m.preScale(1, -1);
    m.preTranslate(0, -image->height() / 2.f);
    return m;
  }

  ui::TitleText Label() const { return {LabelFont(), filename, kLabelHeight}; }

  bool ShowsLabel() const { return show_filename && !filename.empty() && !crop; }

  void Pull() {
    auto file = LockObject<File>();
    if (!file) return;
    image = file->Image();
    icon = file->Icon();
    size = file->Size();
    filename = file->Filename();
    show_filename = file->ShowsFilename();
    label_bounds =
        ShowsLabel() ? Rect(Label().Shape().getBounds()).MoveBy({0, Body().top}) : Rect();
  }

  SkPath Shape() const override {
    SkPath shape = SkPath::Rect(Body());
    if (ShowsLabel()) {
      if (auto with_label =
              Op(shape, Label().Shape().makeOffset(0, Body().top), SkPathOp::kUnion_SkPathOp)) {
        shape = *with_label;
      }
    }
    return shape;
  }

  Optional<Rect> DrawBounds() const override {
    Rect bounds = Body();
    if (ShowsLabel()) bounds.ExpandToInclude(label_bounds);
    return bounds.Outset(1_mm);
  }

  Tock Tick(time::Timer&) override {
    Pull();
    return Tock::Shape;
  }

  void FillMenu(ui::Pointer& pointer, Menu& menu) override {
    ObjectToy::FillMenu(pointer, menu);
    auto file = LockObject<File>();
    if (file && image && BoardOrNull(*this)) {
      menu.Place(ui::Dir::E, Interface(*file, kSetWallpaper));
    }
  }

  void Draw(SkCanvas& canvas) const override {
    Rect body = Body();
    {
      SkAutoCanvasRestore restore(&canvas, true);
      if (crop) canvas.clipRect(body, true);
      if (image) {
        canvas.concat(ImageToLocal());
        canvas.drawImage(image, 0, 0, kDefaultSamplingOptions, nullptr);
      } else if (icon) {
        DrawIconIn(canvas, *icon, PictureRect());
      }
    }
    if (ShowsLabel()) {
      ui::TitleText label = Label();
      canvas.save();
      canvas.translate(0, body.top);
      label.DrawSide(canvas);
      label.DrawOutline(canvas);
      label.DrawFill(canvas);
      canvas.restore();
    }
  }
};

std::unique_ptr<automat::ObjectToy> File::MakeToy(ui::Widget* parent) {
  return std::make_unique<FileToy>(parent, *this);
}

struct File::CropParameters : CropViewParameters {
  Rect rect = Rect(0, 0, 1, 1);
};

constexpr Vec2 kCropCorners[] = {Vec2(-1, -1), Vec2(1, -1), Vec2(1, 1), Vec2(-1, 1)};

static Rect CropRectAt(Rect body, Vec2 point, Vec2 size) {
  Vec2 center = (point - body.BottomLeftCorner()) / body.Size();
  center.x = std::clamp(center.x, size.x / 2, 1 - size.x / 2);
  center.y = std::clamp(center.y, size.y / 2, 1 - size.y / 2);
  return Rect::MakeCenter(center, size.x, size.y);
}

static void PlaceCropGizmos(CropScissors::Group& group, Interface::Table* option) {
  Vec2 size = group.DrawnSize() * group.extent;
  static_cast<CropBox&>(*group.gizmos[0]).size = size;
  for (int i = 0; i < 4; ++i) {
    auto& handle = static_cast<CropHandle&>(*group.gizmos[i + 1]);
    handle.origin = size * kCropCorners[i] / 2;
    handle.scale = group.unfolding.value;
    handle.option = option;
  }
}

struct FileToy::DragCropHandleAction : Action {
  MortalPtr<CropScissors> scissors;
  Ptr<CropView> view;
  Linked<Croppable> croppable;
  SkMatrix scissors_to_fraction;
  Vec2 min_size;
  Vec2 corner;
  Vec2 grab_offset;

  DragCropHandleAction(ui::Pointer& pointer, CropScissors& crop_scissors, Ptr<CropView> crop_view,
                       Linked<Croppable> file_croppable)
      : Action(pointer),
        scissors(&crop_scissors),
        view(std::move(crop_view)),
        croppable(std::move(file_croppable)) {
    Rect rect(0, 0, 1, 1);
    {
      auto lock = std::lock_guard(view->mutex);
      if (view->chain_end == croppable) {
        rect = static_cast<File::CropParameters&>(*view->parameters).rect;
      }
    }
    Vec2 body_size = crop_scissors.box->size / rect.Size();
    min_size = Vec2(kMinCropSize, kMinCropSize) / body_size;
    SkMatrix scissors_to_box;
    (void)crop_scissors.box->ToScissors().invert(&scissors_to_box);
    scissors_to_fraction = SkMatrix::Translate(rect.Center().x, rect.Center().y)
                               .preScale(1 / body_size.x, 1 / body_size.y)
                               .preConcat(scissors_to_box);
    Vec2 in_box = scissors_to_box.mapPoint(pointer.PositionWithin(crop_scissors));
    corner = Vec2(in_box.x < 0 ? -1 : 1, in_box.y < 0 ? -1 : 1);
    Vec2 corner_fraction(corner.x > 0 ? rect.right : rect.left,
                         corner.y > 0 ? rect.top : rect.bottom);
    grab_offset = corner_fraction - PointerFraction();
  }

  Vec2 PointerFraction() const {
    return scissors_to_fraction.mapPoint(pointer.PositionWithin(*scissors));
  }

  void Update() override {
    if (!scissors) {
      pointer.ReplaceAction(*this, nullptr);
      return;
    }
    Vec2 target = PointerFraction() + grab_offset;
    Rect dragged;
    {
      auto lock = std::lock_guard(view->mutex);
      if (view->chain_end != croppable) return;
      Rect& rect = static_cast<File::CropParameters&>(*view->parameters).rect;
      if (corner.x > 0) {
        rect.right = std::min(std::max(target.x, rect.left + min_size.x), 1.f);
      } else {
        rect.left = std::max(std::min(target.x, rect.right - min_size.x), 0.f);
      }
      if (corner.y > 0) {
        rect.top = std::min(std::max(target.y, rect.bottom + min_size.y), 1.f);
      } else {
        rect.bottom = std::max(std::min(target.y, rect.top - min_size.y), 0.f);
      }
      dragged = rect;
    }
    if (auto* group = scissors->FindGroup(croppable)) {
      group->center = dragged.Center();
      group->size = dragged.Size();
    }
    view->WakeToys();
  }
};

static constinit Interface::Table kDragCropHandle = [] {
  Interface::Table t(Interface::kMenu, "Crop Handle");
  t.cursor = ui::Cursor::Crosshair;
  t.activate = [](Span<Interface> chain, ui::Pointer& pointer,
                  Toy* toy) -> std::unique_ptr<Action> {
    auto* scissors = dynamic_cast<CropScissors*>(toy);
    if (!scissors || !scissors->box) return nullptr;
    auto& view = static_cast<CropView&>(*chain.back().object_ptr);
    Linked<Croppable> croppable;
    {
      auto lock = std::lock_guard(view.mutex);
      croppable = view.chain_end;
    }
    if (croppable.Unsafe().table_ptr != &File::croppable_tbl) return nullptr;
    return std::make_unique<FileToy::DragCropHandleAction>(pointer, *scissors, view.AcquirePtr(),
                                                           std::move(croppable));
  };
  return t;
}();

void File::croppable_Impl::CropViewSerialize(const CropView& view, ObjectSerializer& writer) {
  Rect rect(0, 0, 1, 1);
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr == &*obj) {
      rect = static_cast<const File::CropParameters&>(*view.parameters).rect;
    }
  }
  writer.StartArray();
  writer.Double(rect.left);
  writer.Double(rect.bottom);
  writer.Double(rect.right);
  writer.Double(rect.top);
  writer.EndArray();
}

void File::croppable_Impl::CropViewDeserialize(CropView& view, ObjectDeserializer& d) {
  Rect rect(0, 0, 1, 1);
  float* sides[] = {&rect.left, &rect.bottom, &rect.right, &rect.top};
  Status status;
  for (int i : ArrayView(d, status)) {
    if (i < 4) {
      d.Get(*sides[i], status);
    } else {
      d.Skip();
    }
  }
  if (!OK(status)) {
    view.ReportError(status.ToStr());
    return;
  }
  auto lock = std::lock_guard(view.mutex);
  if (view.chain_end.object_ptr != &*obj) return;
  static_cast<File::CropParameters&>(*view.parameters).rect = rect;
}

std::unique_ptr<CropViewParameters> File::croppable_Impl::CropViewMakeParameters() {
  return std::make_unique<File::CropParameters>();
}

void File::croppable_Impl::CropScissorsOpen(CropView& view, automat::Toy& croppable_toy,
                                            CropScissors&, Vec2 point) {
  Rect body = static_cast<FileToy&>(croppable_toy).Body();
  Vec2 size(std::min(1.f, kDefaultCropSize.x / body.Width()),
            std::min(1.f, kDefaultCropSize.y / body.Height()));
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr != &*obj) return;
    static_cast<File::CropParameters&>(*view.parameters).rect = CropRectAt(body, point, size);
  }
  view.WakeToys();
}

void File::croppable_Impl::CropScissorsMove(CropView& view, automat::Toy& croppable_toy,
                                            CropScissors& scissors, Vec2 point) {
  Rect body = static_cast<FileToy&>(croppable_toy).Body();
  Rect moved;
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr != &*obj) return;
    Rect& rect = static_cast<File::CropParameters&>(*view.parameters).rect;
    moved = CropRectAt(body, point, rect.Size());
    if (moved.sk == rect.sk) return;
    rect = moved;
  }
  if (auto* group = scissors.FindGroup(*this)) {
    group->center = moved.Center();
    group->size = moved.Size();
  }
  view.WakeToys();
}

ui::Tock File::croppable_Impl::CropScissorsTick(CropView& view, automat::Toy& croppable_toy,
                                                CropScissors& scissors, time::Timer& timer) {
  auto& file_toy = static_cast<FileToy&>(croppable_toy);
  Rect rect(0, 0, 1, 1);
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr != &*obj) return {};
    rect = static_cast<File::CropParameters&>(*view.parameters).rect;
  }
  auto& group = scissors.FindOrMakeGroup(*this);
  if (group.gizmos.empty()) {
    group.gizmos.push_back(std::make_unique<CropBox>(&scissors));
    for (Vec2 corner : kCropCorners) {
      auto handle = std::make_unique<CropHandle>(&scissors);
      handle->outline = CropHandle::Corner(corner);
      group.gizmos.push_back(std::move(handle));
    }
    group.center = rect.Center();
    group.size = rect.Size();
  }
  ui::Tock tock = group.Unfold(rect.Center(), rect.Size(), timer);
  Rect body = file_toy.Body();
  group.extent = body.Size();
  SkMatrix toy_to_scissors = TransformBetween(file_toy, scissors);
  Vec2 x_axis = toy_to_scissors.mapVector(1, 0);
  scissors.box = CropScissors::Box{
      .center =
          toy_to_scissors.mapPoint(body.BottomLeftCorner() + group.DrawnCenter() * group.extent),
      .turn = SinCos::FromVec2(x_axis),
      .scale = Length(x_axis),
      .size = group.DrawnSize() * group.extent,
  };
  PlaceCropGizmos(group, &kDragCropHandle);
  return tock;
}

ui::Tock File::croppable_Impl::CropScissorsClose(CropScissors& scissors, time::Timer& timer) {
  auto* group = scissors.FindGroup(*this);
  if (!group) return {};
  ui::Tock tock = group->Fold(timer);
  if (!group->gizmos.empty()) PlaceCropGizmos(*group, nullptr);
  return tock;
}

std::unique_ptr<automat::Toy> File::croppable_Impl::CropMakeToy(CropView&, ui::Widget* parent) {
  return std::make_unique<FileToy>(parent, *obj);
}

ui::Tock File::croppable_Impl::CropTickToy(CropView& view, automat::Toy& cropped_toy,
                                           time::Timer&) {
  Rect crop(0, 0, 1, 1);
  CropView* link = &view;
  Locked<Croppable> next;
  while (true) {
    {
      auto lock = std::lock_guard(link->mutex);
      if (link->chain_end.object_ptr != &*obj) break;
      Rect outer = static_cast<File::CropParameters&>(*link->parameters).rect;
      crop =
          Rect(outer.left + crop.left * outer.Width(), outer.bottom + crop.bottom * outer.Height(),
               outer.left + crop.right * outer.Width(), outer.bottom + crop.top * outer.Height());
    }
    next = link->scissors->FindInterface();
    if (next.table_ptr != &CropView::croppable_tbl) break;
    link = static_cast<CropView*>(next.object_ptr);
  }
  auto& file_toy = static_cast<FileToy&>(cropped_toy);
  if (file_toy.crop && file_toy.crop->sk == crop.sk) return {};
  file_toy.crop = crop;
  return ui::Tock::Shape;
}

}  // namespace automat::library
