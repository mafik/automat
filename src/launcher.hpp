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
#include "library_process.hpp"
#include "move_only_function.hpp"
#include "object.hpp"
#include "object_source.hpp"
#include "pointer.hpp"
#include "ptr.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"
#include "time.hpp"
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
  Ptr<library::Process> process;
#if defined(_WIN32)
  void* process_handle = nullptr;
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
    return dyn_cast_if_present<WindowT>(restoring.Lock());
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
};

void LaunchRestoredWindows();

Str MintActivationToken();

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
  static constexpr auto kStyle = Argument::Style::Invisible;
  static constexpr float kAutoconnectRadius = 0.f;
  DEF_END(launcher);

  DEF_INTERFACE(ClientWindow, ObjectSource, process, "Process")
  static constexpr ui::Cursor kCursor = ui::Cursor::Copy;
  Ptr<Object> OnTake();
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* parent);
  DEF_END(process);

  INTERFACES(launcher_tbl, kDecorationMenu, process_tbl);

  ClientWindow() = default;
  ClientWindow(const ClientWindow&);

  bool ClientRunning() const;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

Ptr<Launch> LaunchClone(const ClientWindow& original, ClientWindow& clone);

}  // namespace automat::library
