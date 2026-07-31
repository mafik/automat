#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>
#include <include/core/SkSize.h>

#include <functional>
#include <mutex>

#include "argument.hpp"
#include "base.hpp"
#include "object.hpp"
#include "ptr.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"
#include "time.hpp"
#include "ui_beta.hpp"
#include "vec.hpp"
#include "window_frame.hpp"

namespace automat::library {
struct ClientWindow;
}  // namespace automat::library

namespace automat {

struct Launch;

struct Pipe : ReferenceCounted {
  WeakPtr<Launch> reader;
};

#if defined(_WIN32)
using StdioHandle = void*;
inline constexpr StdioHandle kNoStdio = nullptr;
#else
using StdioHandle = int;
inline constexpr StdioHandle kNoStdio = -1;
#endif

struct SpawnFds {
  StdioHandle in = kNoStdio;
  StdioHandle out = kNoStdio;
  Ptr<Pipe> in_pipe;
  Ptr<Pipe> out_pipe;
};

bool MakeStdioPipe(StdioHandle& read_end, StdioHandle& write_end, Status&);
void CloseStdio(StdioHandle);
StdioHandle AdoptFileDescriptor(int fd);

struct StreamCapture {
  Vec<char> data;
  size_t ring_start = 0;
  uint64_t total = 0;
};

struct Launch : Object {
  mutable std::mutex mutex;

  I64 pid = 0;
  Str token;
  time::SteadyPoint when = {};
  Vec<Str> argv;
  int pidfd = -1;
#if defined(_WIN32)
  void* process = nullptr;
  void* job = nullptr;
  uintptr_t job_key = 0;
  bool job_tracked = false;
  StdioHandle child_stdin = kNoStdio;
#endif
  WeakPtr<Object> source;
  Vec<std::move_only_function<void()>> on_exit;

  WeakPtr<library::ClientWindow> restoring;
  Ptr<Pipe> stdout_pipe;
  StreamCapture out_capture;
  StreamCapture err_capture;
  uint64_t io_wchar = 0;
  bool exited = false;
  int wait_status = 0;
  bool window_appeared = false;

  static Ptr<Launch> Spawn(const Vec<Str>& argv, Object* source, library::ClientWindow* restoring,
                           Status&, const SpawnFds& = {});
  static Ptr<Launch> Find(I64 client_pid, StrView token = {});

  Launch() = default;
  Launch(const Launch&) = delete;
  ~Launch() override;

  template <typename WindowT>
  Ptr<WindowT> LockRestoring() {
    auto lock = std::lock_guard(mutex);
    return restoring.template LockAs<WindowT>();
  }
  void RestoredInto(library::ClientWindow&);
  void WindowAppeared();
  void Terminate(bool keep_connected = false);
  bool OwnsProcess(I64 candidate_pid);
  void NotifyOnExit(std::move_only_function<void()>);

  StreamStats StdoutStats();
  Vec<Str> TailLines(bool err, int max_lines, int max_columns);

  StrView Name() const override { return "Launch"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

void LaunchRestoredWindows();

Str MintActivationToken();

struct LaunchWidget : ui::beta::ObjectToy {
  float radius = 6_mm;

  Str pid_label_;
  bool exited_ = false;
  float saturation_ = 1;
  float rotation_ = 0;

  LaunchWidget(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {}

  Ptr<Launch> LockLaunch() const { return LockObject<Launch>(); }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override { return SkPath::Circle(0, 0, radius + 1_mm); }
  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(3_mm, 4_mm); }
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
};

}  // namespace automat

namespace automat::library {

struct ClientWindow : Object, DecoratedWindow {
  mutable std::mutex mutex;

  Str title;
  Str app_id;
  bool client_gone = false;
  bool client_decorated = false;  // the client draws its own frame
  Vec<Str> recipe;
  I64 client_pid = 0;
  Ptr<Launch> launched_by;

  // The composited snapshot of this window and its size in client pixels.
  sk_sp<SkImage> image;
  SkISize content_size = {};

  DEF_INTERFACE(ClientWindow, ObjectArgument<Object>, launcher, "Launcher")
  static constexpr auto kStyle = Argument::Style::Cable;
  static constexpr float kAutoconnectRadius = 0.f;
  DEF_END(launcher);

  INTERFACES(launcher);

  ClientWindow() = default;
  ClientWindow(const ClientWindow&);

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

Ptr<Launch> LaunchClone(const ClientWindow& original, ClientWindow& clone);

}  // namespace automat::library
