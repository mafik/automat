#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <mutex>

#include "base.hpp"
#include "fd_provider.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"

namespace automat::library {

// A regular file on disk, as recipe data: a path plus the append flag. The
// object is the data element of UNIX pipelines the way the paper is
// Leptonica's: Commands connect to it through the ordinary stream ports, and
// at their start they resolve it to a concrete descriptor (FdProvider) - a
// fresh open per run, so writing reruns rebuild the file exactly like a
// shell `>` redirection, and `append` turns that into `>>`. The face shows
// the file as it is on disk right now: size and the tail of its content,
// polled while the toy is visible.
struct RegularFile : Object {
  mutable std::mutex mutex;  // guards the recipe fields below

  Str path;
  bool append = false;  // write opens O_APPEND instead of O_TRUNC

  DEF_INTERFACE(RegularFile, StreamInput, in_stream, "input")
  Str OnFormat() { return "bytes"; }
  DEF_END(in_stream);

  DEF_INTERFACE(RegularFile, StreamArgument, out_stream, "output")
  Str OnFormat() { return "bytes"; }
  DEF_END(out_stream);

  DEF_INTERFACE(RegularFile, FdProvider, fd_provider, "fd")
  int OnResolve(FdProvider::Dir dir, Status& status) { return obj->Open(dir, status); }
  DEF_END(fd_provider);

  INTERFACES(in_stream, out_stream, fd_provider);

  RegularFile() = default;
  RegularFile(const RegularFile& o)
      : Object(o), path(o.path), append(o.append), out_stream(o.out_stream) {}

  StrView Name() const override { return "File"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(RegularFile, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  void SetPath(StrView new_path);
  void SetAppend(bool a);
  Str Path() const;
  bool Append() const;

  // Opens the path for `dir` and returns the owned descriptor, or -1 with
  // `status` filled (the error also lands on this object's face). Reading is
  // O_RDONLY; writing creates the file and truncates it, or appends when the
  // append flag is set.
  int Open(FdProvider::Dir dir, Status& status);
};

}  // namespace automat::library
