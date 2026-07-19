// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "launcher.hpp"

#include <include/core/SkPathBuilder.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <csignal>
#include <cstring>
#include <vector>

#include "animation.hpp"
#include "board.hpp"
#include "color.hpp"
#include "deserializer.hpp"
#include "fd.hpp"
#include "format.hpp"
#include "hex.hpp"
#include "library_command.hpp"
#include "mux.hpp"
#include "random.hpp"
#include "sincos.hpp"
#include "ui_beta.hpp"
#include "vm.hpp"
#include "x11.hpp"

#if !defined(_WIN32)
#include "wayland.hpp"
#endif

extern char** environ;

namespace automat {

using library::ClientWindow;

static std::mutex registry_mutex;
static Vec<WeakPtr<Launch>> registry;

constexpr size_t kCaptureCap = 256 * 1024;

Str MintActivationToken() {
  char bytes[16];
  RandomBytesSecure(bytes);
  return BytesToHex(bytes, sizeof(bytes));
}

static void Append(StreamCapture& c, const char* buf, size_t n) {
  c.total += n;
  for (size_t i = 0; i < n; ++i) {
    if (c.data.size() < kCaptureCap) {
      c.data.push_back(buf[i]);
    } else {
      c.data[c.ring_start] = buf[i];
      c.ring_start = (c.ring_start + 1) % kCaptureCap;
    }
  }
}

static Str Linearize(const StreamCapture& c) {
  Str out;
  out.reserve(c.data.size());
  out.append(c.data.data() + c.ring_start, c.data.size() - c.ring_start);
  out.append(c.data.data(), c.ring_start);
  return out;
}

#if !defined(_WIN32)

struct CaptureListener : mux::Epoll::Listener {
  WeakPtr<Launch> launch;
  StreamCapture Launch::* capture;

  CaptureListener(FD fd, WeakPtr<Launch> launch, StreamCapture Launch::* capture)
      : Listener(std::move(fd)), launch(std::move(launch)), capture(capture) {}

  StrView Name() const override { return "LaunchCapture"sv; }

  void NotifyRead(Status&) override {
    char buf[4096];
    for (;;) {
      ssize_t n = read(fd.fd, buf, sizeof(buf));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      Ptr<Launch> l = n > 0 ? launch.Lock() : nullptr;
      if (!l) {
        Status status;
        mux::epoll.Del(this, status);
        mux::epoll.Post([this] { delete this; });
        return;
      }
      {
        auto lock = std::lock_guard(l->mutex);
        Append(l.get()->*capture, buf, (size_t)n);
      }
      l->WakeToys();
      if (auto src = l->source.Lock()) src->WakeToys();
    }
  }
};

static void RegisterCapture(FD fd, const Ptr<Launch>& launch, StreamCapture Launch::* capture) {
  int flags = fcntl(fd.fd, F_GETFL);
  if (flags >= 0) fcntl(fd.fd, F_SETFL, flags | O_NONBLOCK);
  auto* listener = new CaptureListener(std::move(fd), launch->AcquireWeakPtr(), capture);
  mux::epoll.Post([listener] {
    Status status;
    mux::epoll.Add(listener, status);
    if (!OK(status)) delete listener;
  });
}

#endif

Ptr<Launch> Launch::Spawn(const Vec<Str>& argv_in, Object* source, ClientWindow* restoring,
                          Status& status, const SpawnFds& fds) {
#if defined(_WIN32)
  AppendErrorMessage(status) += "Launching processes is not implemented on Windows yet.";
  return nullptr;
#else
  Vec<Str> words;
  for (auto& w : argv_in) {
    if (!w.empty()) words.push_back(w);
  }
  if (words.empty()) {
    AppendErrorMessage(status) += "Nothing to run.";
    return nullptr;
  }
  std::vector<char*> argv;
  argv.reserve(words.size() + 1);
  for (auto& w : words) argv.push_back(w.data());
  argv.push_back(nullptr);

  Str token = MintActivationToken();
  Str wayland_socket = wayland::SocketName();
  Str x11_socket = x11::SocketName();
  Str wayland_entry = "WAYLAND_DISPLAY=" + wayland_socket;
  Str display_entry = "DISPLAY=" + x11_socket;
  Str token_entry = "XDG_ACTIVATION_TOKEN=" + token;
  Str startup_entry = "DESKTOP_STARTUP_ID=" + token;
  std::vector<char*> envp;
  for (char** e = environ; *e; ++e) {
    StrView entry(*e);
    if (!wayland_socket.empty() && entry.starts_with("WAYLAND_DISPLAY=")) continue;
    if (!x11_socket.empty() && entry.starts_with("DISPLAY=")) continue;
    if (entry.starts_with("GDK_BACKEND=")) continue;
    if (entry.starts_with("XDG_ACTIVATION_TOKEN=")) continue;
    if (entry.starts_with("DESKTOP_STARTUP_ID=")) continue;
    envp.push_back(*e);
  }
  if (!wayland_socket.empty()) envp.push_back(wayland_entry.data());
  if (!x11_socket.empty()) envp.push_back(display_entry.data());
  envp.push_back(token_entry.data());
  envp.push_back(startup_entry.data());
  envp.push_back(nullptr);

  int err_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  if (pipe2(err_pipe, O_CLOEXEC) != 0) {
    AppendErrorMessage(status) += f("pipe2: {}", strerror(errno));
    return nullptr;
  }
  bool capture_out = fds.out < 0;
  if (capture_out && pipe2(out_pipe, O_CLOEXEC) != 0) {
    AppendErrorMessage(status) += f("pipe2: {}", strerror(errno));
    close(err_pipe[0]);
    close(err_pipe[1]);
    return nullptr;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (fds.in >= 0) posix_spawn_file_actions_adddup2(&actions, fds.in, 0);
  posix_spawn_file_actions_adddup2(&actions, capture_out ? out_pipe[1] : fds.out, 1);
  posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);

  pid_t pid = 0;
  int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  close(err_pipe[1]);
  if (capture_out) close(out_pipe[1]);
  if (err) {
    close(err_pipe[0]);
    if (capture_out) close(out_pipe[0]);
    AppendErrorMessage(status) += f("{}: {}", words[0], strerror(err));
    return nullptr;
  }

  auto launch = MAKE_PTR(Launch);
  launch->pid = pid;
  launch->token = std::move(token);
  launch->when = time::SteadyNow();
  launch->argv = std::move(words);
  launch->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
  if (source) launch->source = source->AcquireWeakPtr();
  if (restoring) launch->restoring = restoring->AcquireWeakPtr();
  if (fds.in_pipe) fds.in_pipe->reader = launch->AcquireWeakPtr();
  if (fds.out_pipe) launch->stdout_pipe = fds.out_pipe;

  RegisterCapture(FD(err_pipe[0]), launch, &Launch::err_capture);
  if (capture_out) RegisterCapture(FD(out_pipe[0]), launch, &Launch::out_capture);

  {
    auto lock = std::lock_guard(registry_mutex);
    std::erase_if(registry, [](WeakPtr<Launch>& w) { return w.IsExpired(); });
    registry.push_back(launch->AcquireWeakPtr());
  }

  Status watch_status;
  mux::WatchProcess(
      pid,
      [weak = launch->AcquireWeakPtr()](int wait_status) {
        if (auto l = weak.Lock()) {
          {
            auto lock = std::lock_guard(l->mutex);
            l->exited = true;
            l->wait_status = wait_status;
          }
          l->WakeToys();
          if (auto src = l->source.Lock()) src->WakeToys();
        }
      },
      watch_status);
  if (!OK(watch_status)) {
    auto lock = std::lock_guard(launch->mutex);
    launch->exited = true;
  }
  return launch;
#endif
}

Ptr<Launch> Launch::Find(I64 client_pid, StrView token) {
  auto lock = std::lock_guard(registry_mutex);
  if (!token.empty()) {
    for (auto& w : registry) {
      if (auto l = w.Lock(); l && l->token == token) return l;
    }
  }
  if (client_pid) {
    for (auto& w : registry) {
      if (auto l = w.Lock(); l && l->pid == client_pid) return l;
    }
  }
  return nullptr;
}

Launch::~Launch() {
#if !defined(_WIN32)
  if (pidfd >= 0) close(pidfd);
  if (!exited && !window_appeared && pid > 0) kill((pid_t)pid, SIGTERM);
#endif
}

void Launch::RestoredInto(ClientWindow&) {
  {
    auto lock = std::lock_guard(mutex);
    restoring = {};
    window_appeared = true;
  }
  WakeToys();
}

void Launch::WindowAppeared() {
  {
    auto lock = std::lock_guard(mutex);
    window_appeared = true;
  }
  WakeToys();
}

#if !defined(_WIN32)

static bool ReadProcFile(const char* path, char* buf, size_t size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  ssize_t len = read(fd, buf, size - 1);
  close(fd);
  if (len <= 0) return false;
  buf[len] = 0;
  return true;
}

static bool BlockedInSyscall(I64 pid, long syscall_nr, long fd_arg) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%lld/syscall", (long long)pid);
  char buf[256];
  if (!ReadProcFile(path, buf, sizeof(buf))) return false;
  char* end = nullptr;
  long nr = strtol(buf, &end, 10);
  if (end == buf || nr != syscall_nr) return false;
  long arg0 = strtol(end, nullptr, 0);
  return arg0 == fd_arg;
}

#endif

StreamStats Launch::StdoutStats() {
  StreamStats stats;
#if !defined(_WIN32)
  bool alive;
  Ptr<Pipe> pipe;
  {
    auto lock = std::lock_guard(mutex);
    alive = !exited;
    if (alive) {
      char path[64];
      snprintf(path, sizeof(path), "/proc/%lld/io", (long long)pid);
      char buf[512];
      if (ReadProcFile(path, buf, sizeof(buf))) {
        if (const char* p = strstr(buf, "wchar: ")) io_wchar = strtoull(p + 7, nullptr, 10);
      }
    }
    stats.bytes = io_wchar;
    pipe = stdout_pipe;
  }
  if (!alive || !pipe) return stats;

  if (pidfd >= 0) {
    int fd1 = (int)syscall(SYS_pidfd_getfd, pidfd, 1, 0);
    if (fd1 >= 0) {
      int fill = 0;
      if (ioctl(fd1, FIONREAD, &fill) == 0) stats.fill = (uint64_t)std::max(0, fill);
      int capacity = fcntl(fd1, F_GETPIPE_SZ);
      if (capacity > 0) stats.capacity = (uint64_t)capacity;
      close(fd1);
    }
  }

  if (BlockedInSyscall(pid, /*write*/ 1, 1)) {
    stats.blocked = StreamBlocked::Producer;
  } else if (auto peer = pipe->reader.Lock()) {
    bool peer_alive;
    {
      auto lock = std::lock_guard(peer->mutex);
      peer_alive = !peer->exited;
    }
    if (peer_alive && BlockedInSyscall(peer->pid, /*read*/ 0, 0)) {
      stats.blocked = StreamBlocked::Consumer;
    }
  }
#endif
  return stats;
}

Vec<Str> Launch::TailLines(bool err, int max_lines, int max_columns) {
  Str text;
  {
    auto lock = std::lock_guard(mutex);
    text = Linearize(err ? err_capture : out_capture);
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  Vec<Str> lines;
  size_t end = text.size();
  while (end > 0 && (int)lines.size() < max_lines) {
    size_t nl = text.rfind('\n', end - 1);
    size_t begin = nl == Str::npos ? 0 : nl + 1;
    Str line;
    for (size_t i = begin; i < end && (int)line.size() < max_columns; ++i) {
      char c = text[i];
      line += (unsigned char)c >= 0x20 ? c : ' ';
    }
    lines.push_back(std::move(line));
    if (nl == Str::npos) break;
    end = nl;
  }
  std::reverse(lines.begin(), lines.end());
  return lines;
}

Ptr<Object> Launch::Clone() const {
  Status status;
  Ptr<Object> src = source.Lock();
  Vec<Str> argv_copy;
  {
    auto lock = std::lock_guard(mutex);
    argv_copy = argv;
  }
  if (auto launch = Spawn(argv_copy, src.get(), nullptr, status)) return launch;
  auto dead = MAKE_PTR(Launch);
  dead->argv = std::move(argv_copy);
  dead->exited = true;
  return dead;
}

void LaunchRestoredWindows() {
  Vec<Ptr<ClientWindow>> pending;
  Vec<std::pair<Ptr<Board>, Location*>> dead_launches;
  {
    auto lock = std::lock_guard(vm.mutex);
    for (auto& board : vm.boards) {
      for (auto& loc : board->locations) {
        if (auto* launch = dynamic_cast<Launch*>(loc->object.get())) {
          auto launch_lock = std::lock_guard(launch->mutex);
          if (launch->pid == 0) dead_launches.emplace_back(board, loc.get());
          continue;
        }
        auto* win = dynamic_cast<ClientWindow*>(loc->object.get());
        if (!win) continue;
        auto win_lock = std::lock_guard(win->mutex);
        if (win->client_gone && !win->recipe.empty() && !win->launched_by) {
          pending.push_back(win->AcquirePtr());
        }
      }
    }
  }
  for (auto& [board, loc] : dead_launches) {
    board->Extract(*loc);
    board->WakeToys();
  }
  for (auto& win : pending) {
    Vec<Str> recipe;
    {
      auto lock = std::lock_guard(win->mutex);
      recipe = win->recipe;
    }
    Ptr<Launch> launch;
    Status status;
    auto found = win->launcher->Find();
    if (auto* cmd = dynamic_cast<library::Command*>(found.Owner<Object>())) {
      launch = cmd->RunFor(*win, status);
      if (!launch) status.Reset();
    }
    if (!launch) launch = Launch::Spawn(recipe, nullptr, win.get(), status);
    if (!launch) {
      win->ReportError(status.ToStr());
      continue;
    }
    auto lock = std::lock_guard(win->mutex);
    win->launched_by = std::move(launch);
  }
}

static SkPath GearPath(Vec2 c, float r, float rotation_phase) {
  SkPathBuilder path;
  constexpr int kTeeth = 9;
  float inner = r * 0.78f;
  SinCos step = SinCos::FromDegrees(360.f / (kTeeth * 4));
  SinCos a = SinCos::FromDegrees(90.f + rotation_phase * 360.f / kTeeth);
  for (int i = 0; i < kTeeth; ++i) {
    Vec2 p0 = c + Vec2::Polar(a, r);
    a = a + step;
    Vec2 p1 = c + Vec2::Polar(a, r);
    a = a + step;
    Vec2 p2 = c + Vec2::Polar(a, inner);
    a = a + step;
    Vec2 p3 = c + Vec2::Polar(a, inner);
    a = a + step;
    if (i == 0) {
      path.moveTo(p0);
    } else {
      path.lineTo(p0);
    }
    path.lineTo(p1);
    path.lineTo(p2);
    path.lineTo(p3);
  }
  path.close();
  path.setFillType(SkPathFillType::kEvenOdd);
  path.addCircle(c.x, c.y, r * 0.28f);
  return path.detach();
}

static SkColor MixSk(SkColor zero, SkColor one, float ratio) {
  return color::MixColors(SkColor4f::FromColor(zero), SkColor4f::FromColor(one), ratio).toSkColor();
}

ui::Tock LaunchWidget::Tick(time::Timer& t) {
  if (auto launch = LockLaunch()) {
    auto lock = std::lock_guard(launch->mutex);
    pid_label_ = launch->pid ? f("{}", launch->pid) : Str{};
    exited_ = launch->exited;
  }
  ui::Tock tock = ui::Tock::Draw;
  if (!exited_) {
    rotation_ = fmodf(rotation_ + (float)t.d * 0.7f, 1.f);
    tock.drawing |= true;
  }
  tock.drawing |= animation::LinearApproach(exited_ ? 0.f : 1.f, (float)t.d, 4, saturation_);
  return tock;
}

void LaunchWidget::Draw(SkCanvas& canvas) const {
  uint32_t seed = Seed(0x7A);
  SkPath gear = GearPath({0, 0}, radius, rotation_);
  ui::beta::MisregFill(canvas, gear, MixSk(ui::beta::kGray, ui::beta::kYellow, saturation_), seed);
  ui::beta::SketchyStroke(canvas, gear, MixSk(ui::beta::kGrayDark, ui::beta::kInk, saturation_),
                          ui::beta::kStroke * 0.8f, seed, 1);
  if (!pid_label_.empty()) {
    float w = ui::beta::TextWidth(pid_label_, ui::beta::kMicroSize);
    ui::beta::DrawText(canvas, pid_label_, Vec2(-w / 2, radius + 0.4_mm), ui::beta::kMicroSize,
                       MixSk(ui::beta::kInkSoft, ui::beta::kInk, saturation_), false, seed);
  }
}

std::unique_ptr<ObjectToy> Launch::MakeToy(ui::Widget* parent) {
  return std::make_unique<LaunchWidget>(parent, *this);
}

}  // namespace automat

namespace automat::library {

ClientWindow::ClientWindow(const ClientWindow& o) : launcher(o.launcher) {
  auto lock = std::lock_guard(o.mutex);
  recipe = o.recipe;
  title = o.title;
  app_id = o.app_id;
  client_gone = true;
  decoration_preference.store(o.decoration_preference.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
}

void ClientWindow::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  if (!recipe.empty()) {
    writer.Key("recipe");
    writer.StartArray();
    for (auto& w : recipe) {
      if (w.empty()) continue;
      writer.String(w.data(), w.size());
    }
    writer.EndArray();
  }
  if (!title.empty()) {
    writer.Key("title");
    writer.String(title.data(), title.size());
  }
  SerializeDecoration(writer);
}

bool ClientWindow::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "recipe") {
    recipe.clear();
    for (auto i : ArrayView(d, status)) {
      (void)i;
      Str word;
      d.Get(word, status);
      if (OK(status)) recipe.push_back(std::move(word));
    }
    client_gone = true;
    return true;
  }
  if (key == "title") {
    d.Get(title, status);
    return true;
  }
  return DeserializeDecoration(d, key);
}

Ptr<Launch> LaunchClone(const ClientWindow& original, ClientWindow& clone) {
  bool was_live;
  {
    auto lock = std::lock_guard(original.mutex);
    was_live = !original.client_gone;
  }
  Vec<Str> recipe;
  {
    auto lock = std::lock_guard(clone.mutex);
    recipe = clone.recipe;
  }
  if (!was_live || recipe.empty()) return nullptr;
  Status status;
  auto launch = Launch::Spawn(recipe, nullptr, &clone, status);
  if (!launch) {
    clone.ReportError(status.ToStr());
    return nullptr;
  }
  auto lock = std::lock_guard(clone.mutex);
  clone.launched_by = launch;
  return launch;
}

}  // namespace automat::library
