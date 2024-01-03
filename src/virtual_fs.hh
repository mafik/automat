#pragma once

#include <sys/types.h>

#include "fn.hh"
#include "path.hh"
#include "status.hh"
#include "str.hh"
#include "vec.hh"

namespace maf::fs {

struct VirtualFS {
  virtual ~VirtualFS() = default;

  // Read the given file and call the callback with the file content.
  //
  // The file is read using mmap so virtual filesystems like procfs won't work.
  virtual void Map(const Path &, Fn<void(StrView)> callback, Status &) = 0;

  // Read the given file and return the file content.
  //
  // The file is read sequentially so virtual filesystems like procfs will work.
  virtual Str Read(const Path &, Status &) = 0;

  // Write the given file with the given contents.
  virtual void Write(const Path &, StrView contents, Status &,
                     mode_t = 0644) = 0;
};

struct EmbeddedFS final : VirtualFS {
  void Map(const Path &, Fn<void(StrView)> callback, Status &) override;
  Str Read(const Path &, Status &) override;
  void Write(const Path &, StrView contents, Status &, mode_t = 0644) override;
};

struct RealFS final : VirtualFS {
  void Map(const Path &, Fn<void(StrView)> callback, Status &) override;
  Str Read(const Path &, Status &) override;
  void Write(const Path &, StrView contents, Status &, mode_t = 0644) override;
  void Copy(const Path &from, const Path &to, Status &, mode_t = 0644);
};

struct OverlayFS final : VirtualFS {
  Vec<VirtualFS *> layers;
  void Map(const Path &, Fn<void(StrView)> callback, Status &) override;
  Str Read(const Path &, Status &) override;
  void Write(const Path &, StrView contents, Status &, mode_t = 0644) override;
};

// Used to access an embedded filesystem, embedded within the binary at build
// time.
extern EmbeddedFS embedded;

// Used to access the real filesystem, provided by the base OS.
extern RealFS real;

// Filesystem that usually serves files from the embedded filesystem BUT if a
// real file with the same path exists - it overrides the embedded one. This can
// be used for development - so that you don't have to recompile the program
// every time you change a file.
extern OverlayFS real_then_embedded;

void Map(VirtualFS &, const Path &, Fn<void(StrView)> callback, Status &);
Str Read(VirtualFS &, const Path &, Status &);
void Write(VirtualFS &, const Path &, StrView contents, Status &,
           mode_t = 0644);
void Copy(VirtualFS &from_fs, const Path &from, VirtualFS &to_fs,
          const Path &to, Status &, mode_t = 0644);

struct VFile {
  Str path;
  Str content;
};

void CopyFile(const Path &from, const Path &to, Status &, mode_t = 0644);

} // namespace maf::fs