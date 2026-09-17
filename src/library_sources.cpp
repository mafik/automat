// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_sources.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>

#include "embedded.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "path.hpp"
#include "textures.hpp"
#include "virtual_fs.hpp"

using namespace std;

namespace automat::library {

// Image dimensions: 778x1008 pixels
// Aspect ratio: 778/1008 ≈ 0.772
constexpr float kSourcesHeight = 10_cm;
constexpr float kSourcesWidth = kSourcesHeight * 778.f / 1008.f;

static PersistentImage& SourcesImage() {
  static auto image =
      PersistentImage::MakeFromAsset(embedded::assets_sources_webp, {.height = kSourcesHeight});
  return image;
}

Sources::Sources() {}

std::string_view Sources::Name() const { return "Sources"; }

Ptr<Object> Sources::Clone() const { return MAKE_PTR(Sources); }

void Sources::extract_files_Impl::OnRun(std::unique_ptr<RunTask>&) {
  // Extract all embedded files to the current directory
  Status status;
  int file_count = 0;
  for (auto& [path, vfile] : embedded::index) {
    Path out_path(path);
    // Create parent directories if needed
    auto parent = out_path.Parent();
    if (!parent.str.empty()) {
      parent.MakeDirs(status);
      if (!OK(status)) {
        LOG << "Failed to create directory for " << path << ": " << status;
        status = Status();
        continue;
      }
    }
    fs::real.Write(out_path, vfile->content, status);
    if (!OK(status)) {
      LOG << "Failed to extract " << path << ": " << status;
      status = Status();
      continue;
    }
    ++file_count;
  }
  LOG << "Extracted " << file_count << " files";
}

struct SourcesWidget : ObjectToy {
  void FillMenu(ui::Pointer& pointer, Menu& menu) override {
    ObjectToy::FillMenu(pointer, menu);
    auto object = LockObject<Sources>();
    if (!object) return;
    menu.Place(ui::Dir::N, object->extract_files.Bind());
  }
  SourcesWidget(ui::Widget* parent, Object& sources) : ObjectToy(parent, sources) {}

  Ptr<Sources> LockSources() const { return LockObject<Sources>(); }

  static Rect GetRect() { return Rect::MakeCornerZero(kSourcesWidth, kSourcesHeight); }

  SkPath Shape() const override { return SkPath::Rect(GetRect()); }

  RRect CoarseBounds() const override { return RRect::MakeSimple(GetRect(), 0); }

  void Draw(SkCanvas& canvas) const override { SourcesImage().draw(canvas); }
};

std::unique_ptr<ObjectToy> Sources::MakeToy(ui::Widget* parent) {
  return std::make_unique<SourcesWidget>(parent, *this);
}

void Sources::SerializeState(ObjectSerializer& writer) const {
  // No state to serialize
}

bool Sources::DeserializeKey(ObjectDeserializer& d, StrView key) {
  // No state to deserialize
  return false;
}

}  // namespace automat::library
