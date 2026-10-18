#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkImage.h>
#include <include/core/SkPicture.h>

#include <mutex>

#include "base.hpp"
#include "image_provider.hpp"
#include "optional.hpp"
#include "status.hpp"
#include "str.hpp"

namespace automat::library {

// The goal of the File object is to be an easy & interactive representation of some file.
//
// File objects are a *high-level* representation of something that the user thinks of as "file".
// They're NOT meant to represent filesystem paths, files as inode numbers (abstract Unix files) nor
// file discriptors (handles to Unix files).
//
// The contents of the file should typically be loaded in memory & parsed according to its format
// into some internal representation dependent on the file type.
//
// The in-memory representation is generally the source of truth for file contents and should be
// written back to the filesystem on change.
//
// The on-disk component of the file (typically stored next to automat_state.json) should be tracked
// for live updates from external programs and auto-reloaded. This file synchronization goes both
// ways - so changes from Automat done to the in-memory representation should be propagated back to
// the filesystem. File object internally decides when & how often to write to filesystem (generally
// - immediately) but it may offer some knobs to control this behavior. During serialization the
// filesystem should be synced to guarantee proper deserialization later.
//
// The purpose of this high-level representation is to be a simple starting point for working with
// files - not a universal abstraction. For low-level access there should exist proper low-level
// (probably platform-specific) primitives. For specialized access there should exist better
// format-specific adapters.
//
// Copying a File object should create a carbon copy of the in-memory file contents (doubling the
// used memory) & let the copied file synchronize its contents back to filesystem in the background.
//
// File synchronization options:
// 1. mmap - prevents concurrent access but allows the memory to be released
//                messy on Windows
// 2. read & copy - allows concurrent access but lots of copies & memory usage
//
// Useful interfaces:
// 1. ImageProvider
// 2. SurfaceProvider (write-able bitmap - but how could it work?)
//
// Plan:
// - separate mmapp-ed file (highly fragile but also live)
// - R/W buffer - Ptr-managed & shared-mutex-protected memory buffer
// - make the top-level file synchronize them & also analyze contents as needed
struct File : Object {
  mutable std::mutex mutex;

  Str path;  // absolute path on disk, empty when unset
  Optional<bool> show_filename;
  Str contents;
  uint32_t contents_monitor = 0;
  mutable uint32_t flushed_monitor = 0;
  sk_sp<SkImage> image;
  sk_sp<SkPicture> icon;
  Vec2 size;

  DEF_INTERFACE(File, ImageProvider, image_provider, "Image")
  sk_sp<SkImage> GetImage() { return obj->Image(); }
  DEF_END(image_provider);

  DEF_INTERFACE(File, Command, toggle_filename, "Toggle filename")
  void OnRun() { obj->ToggleFilename(); }
  DEF_END(toggle_filename);

  mutable HeapToC base_toc{2};

  File();
  explicit File(StrView name);
  ~File() override;

  ToC* BaseToc() const override { return &base_toc; }

  StrView Name() const override { return "File"; }
  Ptr<Object> Clone() const override;

  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  void Append(StrView bytes);
  void Analyze();
  void FlushToFilesystem(Status&) const;
  void ToggleFilename();

  Str Path() const;
  Str Filename() const;
  bool IsImage() const;
  bool ShowsFilename() const;
  sk_sp<SkImage> Image() const;
  sk_sp<SkPicture> Icon() const;
  Vec2 Size() const;
};

}  // namespace automat::library
