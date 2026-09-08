// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#include "library_file.hpp"

#include <include/core/SkCanvas.h>
#include <include/pathops/SkPathOps.h>
#include <include/private/SkExif.h>

#include <algorithm>
#include <cctype>

#include "control_flow.hpp"
#include "deserializer.hpp"
#include "file_import.hpp"
#include "font.hpp"
#include "menu.hpp"
#include "object.hpp"
#include "root_widget.hpp"
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

File::File(const File& o) : Object(o) {
  Str o_path;
  {
    auto lock = std::lock_guard(o.mutex);
    o_path = o.path;
    owns_file = o.owns_file;
    show_filename = o.show_filename;
  }
  SetPath(o_path);
}

File::~File() {
  if (owns_file && !suspended && !path.empty()) {
    Status status;
    automat::Path(path).Unlink(status, true);
  }
}

void File::Interfaces(const std::function<LoopControl(Interface)>& cb) {
  if (IsImage()) {
    cb(image_provider.Bind());
  }
}

void File::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  if (!path.empty()) {
    Str uri = FileURIFromPath(path);
    writer.Key("path");
    writer.String(uri.data(), uri.size());
  }
  if (show_filename) {
    writer.Key("show_filename");
    writer.Bool(*show_filename);
  }
}

bool File::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "path") {
    Status status;
    Str uri;
    d.Get(uri, status);
    if (!OK(status)) return true;
    automat::Path p = PathFromFileURI(uri, status);
    if (OK(status)) {
      SetPath(p.str);
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

void File::SetPath(StrView new_path) {
  {
    auto lock = std::lock_guard(mutex);
    path = new_path;
    contents = nullptr;
    image = nullptr;
    icon = nullptr;
    Status status;
    StrView mapped = fs::real.MapFile(automat::Path(path), status);
    if (OK(status)) {
      contents = SkData::MakeWithProc(
          mapped.data(), mapped.size(),
          [](const void* ptr, void* size) {
            fs::real.UnmapFile(StrView((const char*)ptr, (size_t)size));
          },
          (void*)mapped.size());
      image = SkImages::DeferredFromEncodedData(contents);
    }
    if (image) {
      image = image->withDefaultMipmaps();
      float screen = ui::root_widget->display_pixels_per_meter;
      Vec2 pixels_per_meter = PixelsPerMeter(mapped).value_or(Vec2(screen, screen));
      size =
          FitAspect(Vec2(image->width(), image->height()) / pixels_per_meter, kMinSize, kMaxSize);
    } else {
      contents = nullptr;
      icon = IconForExtension(ExtensionOf(path));
      size = Vec2(kIconSize, kIconSize);
      if (icon) {
        SkRect bounds = icon->cullRect();
        size = FitAspect(Vec2(bounds.width(), bounds.height()), 0, kIconSize);
      }
    }
  }
  WakeToys();
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

struct ToggleFilenameOption : TextOption {
  WeakPtr<File> weak;

  ToggleFilenameOption(WeakPtr<File> weak) : TextOption("Toggle filename"), weak(weak) {}

  Ptr<Option> Clone() const override { return MAKE_PTR(ToggleFilenameOption, weak); }

  std::unique_ptr<Action> Activate(ui::Pointer& pointer) override {
    if (auto file = weak.lock()) file->ToggleFilename();
    return std::make_unique<EmptyAction>(pointer);
  }
};

struct FileToy : automat::ObjectToy {
  sk_sp<SkImage> image;
  sk_sp<SkPicture> icon;
  Vec2 size;
  Str filename;
  bool show_filename = false;
  Rect label_bounds;

  FileToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) { Pull(); }

  bool CenteredAtZero() const override { return true; }

  Rect Body() const { return Rect::MakeCenterZero(size.x, size.y); }

  ui::TitleText Label() const { return {LabelFont(), filename, kLabelHeight}; }

  bool ShowsLabel() const { return show_filename && !filename.empty(); }

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
    return Tock::Draw;
  }

  void Draw(SkCanvas& canvas) const override {
    Rect body = Body();
    if (image) {
      SkRect src = SkRect::Make(image->dimensions());
      SkMatrix m = SkMatrix::RectToRect(src, body.sk, SkMatrix::kFill_ScaleToFit);
      m.preTranslate(0, image->height() / 2.f);
      m.preScale(1, -1);
      m.preTranslate(0, -image->height() / 2.f);
      canvas.save();
      canvas.concat(m);
      canvas.drawImage(image, 0, 0, kDefaultSamplingOptions, nullptr);
      canvas.restore();
    } else if (icon) {
      DrawIconIn(canvas, *icon, body);
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

  void Options(ui::Pointer& pointer, OptionVisitor& visitor) override {
    ObjectToy::Options(pointer, visitor);
    if (auto file = LockObject<File>()) {
      ToggleFilenameOption toggle(file);
      visitor(toggle);
    }
  }
};

std::unique_ptr<automat::ObjectToy> File::MakeToy(ui::Widget* parent) {
  return std::make_unique<FileToy>(parent, *this);
}

}  // namespace automat::library
