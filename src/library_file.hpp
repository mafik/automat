#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPicture.h>

#include <mutex>

#include "base.hpp"
#include "image_provider.hpp"
#include "optional.hpp"
#include "str.hpp"

namespace automat::library {

struct File : Object {
  mutable std::mutex mutex;

  Str path;               // absolute path on disk, empty when unset
  bool owns_file = true;  // remove the disk file when the object is deleted
  Optional<bool> show_filename;

  DEF_INTERFACE(File, ImageProvider, image_provider, "Image")
  sk_sp<SkImage> GetImage() { return obj->Image(); }
  DEF_END(image_provider);

  DEF_INTERFACE(File, Signal, toggle_filename, "Toggle filename")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->ToggleFilename(); }
  DEF_END(toggle_filename);

  File() = default;
  File(const File&);
  ~File() override;

  StrView Name() const override { return "File"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(File, *this); }

  void Interfaces(const std::function<LoopControl(Interface)>&) override;

  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  void SetPath(StrView new_path);
  void ToggleFilename();

  Str Path() const;
  Str Filename() const;
  bool IsImage() const;
  bool ShowsFilename() const;
  sk_sp<SkImage> Image() const;
  sk_sp<SkPicture> Icon() const;
  Vec2 Size() const;

 private:
  sk_sp<SkData> contents;
  sk_sp<SkImage> image;
  sk_sp<SkPicture> icon;
  Vec2 size;
};

}  // namespace automat::library
