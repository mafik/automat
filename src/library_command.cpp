// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_command.hpp"

#include <include/core/SkCanvas.h>

#include <cmath>

#include "animation.hpp"
#include "fd_provider.hpp"
#include "format.hpp"
#include "text_field.hpp"
#include "ui_beta.hpp"
#include "ui_button.hpp"
#include "units.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#include "mux_epoll.hpp"
#include "wayland.hpp"
#include "x11.hpp"

extern "C" char** environ;
#endif

namespace automat::library {

using ui::beta::Hash2;

// ============================================================================
// Object
// ============================================================================

Vec<Str> SplitWords(StrView line) {
  Vec<Str> words;
  size_t i = 0;
  while (i < line.size()) {
    if (line[i] == ' ') {
      ++i;
      continue;
    }
    size_t j = line.find(' ', i);
    if (j == StrView::npos) j = line.size();
    words.push_back(Str(line.substr(i, j - i)));
    i = j;
  }
  return words;
}

Command::~Command() {
#if !defined(_WIN32)
  auto lock = std::lock_guard(mutex);
  if (child_pidfd >= 0) close(child_pidfd);
#endif
}

std::string Command::GetText() const {
  auto lock = std::lock_guard(mutex);
  Str out;
  for (auto& w : argv) {
    if (w.empty()) continue;
    if (!out.empty()) out += ' ';
    out += w;
  }
  return out;
}

void Command::SetText(std::string_view text) {
  {
    auto lock = std::lock_guard(mutex);
    argv = SplitWords(text);
  }
  WakeToys();
}

void Command::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("argv");
  writer.StartArray();
  for (auto& w : argv) {
    if (w.empty()) continue;
    writer.String(w.data(), w.size());
  }
  writer.EndArray();
}

bool Command::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "argv") {
    auto lock = std::lock_guard(mutex);
    argv.clear();
    for (auto i : ArrayView(d, status)) {
      (void)i;
      Str word;
      d.Get(word, status);
      if (OK(status)) argv.push_back(std::move(word));
    }
    return true;
  }
  if (key == "command") {  // legacy flat-string form
    Str line;
    d.Get(line, status);
    auto lock = std::lock_guard(mutex);
    argv = SplitWords(line);
    return true;
  }
  return false;
}

#if defined(_WIN32)

I64 SpawnArgv(const Vec<Str>&, Status& status, const StdioFds&) {
  AppendErrorMessage(status) += "Command is not implemented on Windows yet.";
  return 0;
}
void Command::Launch(std::unique_ptr<RunTask>&) {
  ReportError("Command is not implemented on Windows yet.");
}
I64 Command::AdoptiveLaunch(Status& status) {
  AppendErrorMessage(status) += "Command is not implemented on Windows yet.";
  return 0;
}
bool Command::SpawnStage(const StdioFds&, std::unique_ptr<RunTask>&, Status& status) {
  AppendErrorMessage(status) += "Command is not implemented on Windows yet.";
  return false;
}
void Command::Terminate() {}
StreamStats Command::StdoutStats() { return {}; }

#else

I64 SpawnArgv(const Vec<Str>& argv_in, Status& status, const StdioFds& stdio) {
  Vec<Str> words;
  for (auto& w : argv_in) {
    if (!w.empty()) words.push_back(w);
  }
  if (words.empty()) {
    AppendErrorMessage(status) += "Nothing to run.";
    return 0;
  }
  std::vector<char*> argv;
  argv.reserve(words.size() + 1);
  for (auto& w : words) argv.push_back(w.data());
  argv.push_back(nullptr);

  // Children talk to Automat's own display servers: WAYLAND_DISPLAY and DISPLAY both
  // point at our sockets. GDK is left unset so a toolkit picks its own backend (GTK
  // prefers Wayland when both are offered); a client that only speaks X11 uses DISPLAY.
  Str wayland_socket = wayland::SocketName();
  Str x11_socket = x11::SocketName();
  Str wayland_entry = "WAYLAND_DISPLAY=" + wayland_socket;
  Str display_entry = "DISPLAY=" + x11_socket;
  std::vector<char*> envp;
  for (char** e = environ; *e; ++e) {
    StrView entry(*e);
    if (!wayland_socket.empty() && entry.starts_with("WAYLAND_DISPLAY=")) continue;
    if (!x11_socket.empty() && entry.starts_with("DISPLAY=")) continue;
    if (entry.starts_with("GDK_BACKEND=")) continue;
    envp.push_back(*e);
  }
  if (!wayland_socket.empty()) envp.push_back(wayland_entry.data());
  if (!x11_socket.empty()) envp.push_back(display_entry.data());
  envp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (stdio.in >= 0) posix_spawn_file_actions_adddup2(&actions, stdio.in, 0);
  if (stdio.out >= 0) posix_spawn_file_actions_adddup2(&actions, stdio.out, 1);

  pid_t pid = 0;
  int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (err) {
    AppendErrorMessage(status) += f("{}: {}", words[0], strerror(err));
    return 0;
  }
  return pid;
}

static void WatchChild(Command& cmd, I64 pid) {
  Status status;
  mux::WatchProcess((pid_t)pid,
                    [weak = cmd.AcquireWeakPtr(), pid](int wait_status) {
                      if (auto obj = weak.Lock()) {
                        auto& cmd = static_cast<Command&>(*obj);
                        auto lock = std::lock_guard(cmd.mutex);
                        if (cmd.child_pid == pid) {
                          cmd.child_pid = 0;
                          if (cmd.child_pidfd >= 0) {
                            close(cmd.child_pidfd);
                            cmd.child_pidfd = -1;
                          }
                        }
                        cmd.wait_status = wait_status;
                        // Cancel() may have already consumed the task; Done()
                        // requires one.
                        if (cmd.running->IsRunning()) cmd.running->Done();
                        cmd.WakeToys();
                      }
                    },
                    status);
  if (!OK(status)) cmd.ReportError(status.ToStr());
}

bool Command::SpawnStage(const StdioFds& stdio, std::unique_ptr<RunTask>& task, Status& status) {
  Vec<Str> argv_copy;
  {
    auto lock = std::lock_guard(mutex);
    if (child_pid) {
      AppendErrorMessage(status) += "Already running.";
      return false;
    }
    argv_copy = argv;
  }
  I64 pid = SpawnArgv(argv_copy, status, stdio);
  if (!pid) return false;
  {
    auto lock = std::lock_guard(mutex);
    child_pid = pid;
    ever_ran = true;
    io_wchar = 0;
    stdout_piped = stdio.out_pipe;
    if (child_pidfd >= 0) close(child_pidfd);
    child_pidfd = (int)syscall(SYS_pidfd_open, (pid_t)pid, 0);
    if (!task) task = std::make_unique<RunTask>(AcquireWeakPtr(), &Command::run_tbl);
    running->BeginLongRunning(std::move(task));
  }
  ClearOwnError();
  WakeToys();
  WatchChild(*this, pid);
  return true;
}

void Command::Launch(std::unique_ptr<RunTask>& task) {
  {
    auto lock = std::lock_guard(mutex);
    if (child_pid) return;  // already running; STOP is the way to a restart
  }

  // The downstream chain connected through stdout -> stdin. An anonymous pipe
  // needs both ends at spawn, so the chain starts together, the way a shell
  // starts every stage of `a | b | c` before any of them runs. Stages that
  // are already running end the chain: their descriptors are fixed.
  Vec<Command*> chain;
  Vec<NestedPtr<StreamInput::Table> > keep_alive;  // holds the chain owners
  chain.push_back(this);
  for (Command* cur = this; chain.size() < 32;) {
    auto target = cur->out_stream->FindInterface();
    auto* next = dynamic_cast<Command*>(target.Owner<Object>());
    if (!next) break;
    bool seen = false;
    for (auto* c : chain) seen |= (c == next);
    if (seen) break;  // a cycle; stop where the pipe would bite its own tail
    {
      auto lock = std::lock_guard(next->mutex);
      if (next->child_pid) break;
    }
    keep_alive.push_back(std::move(target));
    chain.push_back(next);
    cur = next;
  }

  int n = (int)chain.size();
  Vec<StdioFds> stdio(n);
  Vec<int> to_close;

  // A file at either end of the chain resolves to a descriptor instead of a
  // pipe: the head's stdin reads it, the tail's stdout writes it - shell
  // redirection. A failed open aborts the launch, the way a shell refuses
  // the whole command when its redirection fails.
  if (auto producer = in_stream->Producer()) {
    if (auto fd_provider = FindFdProvider(*producer)) {
      Status status;
      int fd = fd_provider.Resolve(FdProvider::Dir::Read, status);
      if (fd < 0) {
        ReportError(f("stdin: {}", status.ToStr()));
        return;
      }
      stdio[0].in = fd;
      to_close.push_back(fd);
    }
  }
  {
    auto target = chain.back()->out_stream->FindInterface();
    if (auto* end_owner = target.Owner<Object>()) {
      if (auto fd_provider = FindFdProvider(*end_owner)) {
        Status status;
        int fd = fd_provider.Resolve(FdProvider::Dir::Write, status);
        if (fd < 0) {
          chain.back()->ReportError(f("stdout: {}", status.ToStr()));
          for (int held : to_close) close(held);
          return;
        }
        stdio[n - 1].out = fd;
        to_close.push_back(fd);
      }
    }
  }

  // Pipes between consecutive stages. O_CLOEXEC keeps the parent's copies
  // out of every child; posix_spawn's dup2 clears it on the installed ends.
  for (int i = 0; i + 1 < n; ++i) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
      ReportError(f("pipe2: {}", strerror(errno)));
      for (int fd : to_close) close(fd);
      return;
    }
    stdio[i].out = fds[1];
    stdio[i].out_pipe = true;
    stdio[i + 1].in = fds[0];
    to_close.push_back(fds[0]);
    to_close.push_back(fds[1]);
  }

  for (int i = 0; i < n; ++i) {
    Status status;
    // The head consumes the caller's task; downstream stages synthesize their
    // own, the way AdoptiveLaunch does.
    std::unique_ptr<RunTask> stage_task;
    if (!chain[i]->SpawnStage(stdio[i], i == 0 ? task : stage_task, status)) {
      // A failed stage reports its own error; the rest of the chain still
      // runs and the kernel propagates EOF / SIGPIPE past the gap.
      chain[i]->ReportError(status.ToStr());
    }
  }
  // The parent never keeps pipe ends: a held read end would suppress the
  // writer's SIGPIPE and a held write end would suppress the reader's EOF.
  for (int fd : to_close) close(fd);
}

I64 Command::AdoptiveLaunch(Status& status) {
  std::unique_ptr<RunTask> task;
  if (!SpawnStage({}, task, status)) return 0;
  auto lock = std::lock_guard(mutex);
  return child_pid;
}

void Command::Terminate() {
  auto lock = std::lock_guard(mutex);
  if (child_pid) kill((pid_t)child_pid, SIGTERM);
}

static bool ReadProcFile(const char* path, char* buf, size_t size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  ssize_t len = read(fd, buf, size - 1);
  close(fd);
  if (len <= 0) return false;
  buf[len] = 0;
  return true;
}

// True when /proc/<pid>/syscall says the process is blocked in the given
// syscall on the given fd (x86_64 numbering; read = 0, write = 1).
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

StreamStats Command::StdoutStats() {
  StreamStats stats;
  I64 pid = 0;
  int pidfd = -1;
  bool piped = false;
  {
    auto lock = std::lock_guard(mutex);
    if (child_pid) {
      char path[64];
      snprintf(path, sizeof(path), "/proc/%lld/io", (long long)child_pid);
      char buf[512];
      if (ReadProcFile(path, buf, sizeof(buf))) {
        if (const char* p = strstr(buf, "wchar: ")) io_wchar = strtoull(p + 7, nullptr, 10);
      }
      pid = child_pid;
      pidfd = child_pidfd;
      piped = stdout_piped;
    }
    stats.bytes = io_wchar;
  }
  if (!pid || !piped) return stats;

  // Pipe fill and capacity, read through a transient dup of the child's own
  // write end. The dup lives only for these two calls: a held write end
  // would suppress the reader's EOF.
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

  // The blocked side: this child stuck writing its stdout, or the
  // downstream peer's child stuck reading its stdin.
  if (BlockedInSyscall(pid, /*write*/ 1, 1)) {
    stats.blocked = StreamBlocked::Producer;
  } else {
    auto target = out_stream->FindInterface();
    if (auto* peer = dynamic_cast<Command*>(target.Owner<Object>())) {
      I64 peer_pid = 0;
      {
        auto lock = std::lock_guard(peer->mutex);
        peer_pid = peer->child_pid;
      }
      if (peer_pid && BlockedInSyscall(peer_pid, /*read*/ 0, 0)) {
        stats.blocked = StreamBlocked::Consumer;
      }
    }
  }
  return stats;
}

static bool IsExecutableFile(const char* path) {
  struct stat st;
  return access(path, X_OK) == 0 && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// execvp-style lookup: a word with '/' is a path; anything else is searched
// for in $PATH. Used only for the toy's readiness display - the kernel does
// the authoritative lookup at spawn time.
static bool ResolvesOnPath(const Str& prog) {
  if (prog.empty()) return false;
  if (prog.find('/') != Str::npos) return IsExecutableFile(prog.c_str());
  const char* path = getenv("PATH");
  if (!path) return false;
  StrView rest = path;
  while (!rest.empty()) {
    size_t colon = rest.find(':');
    StrView dir = rest.substr(0, colon == StrView::npos ? rest.size() : colon);
    rest = colon == StrView::npos ? StrView{} : rest.substr(colon + 1);
    if (dir.empty()) continue;
    Str candidate = Str(dir) + "/" + prog;
    if (IsExecutableFile(candidate.c_str())) return true;
  }
  return false;
}

#endif  // !_WIN32

// ============================================================================
// Toy
// ============================================================================

namespace {

constexpr float kPlateW = 7_cm;
// The title band matches what Panel draws internally for the same tokens.
constexpr float kBand = ui::beta::kTitleSize + 2 * ui::beta::kPadS + 0.45_mm;
constexpr float kCreditRow = 2.0_mm;
constexpr float kFieldH = 9.0_mm;
constexpr float kCaptionRow = 2.3_mm;
constexpr float kRowGap = 0.9_mm;
constexpr float kStatusRow = 5.0_mm;
constexpr float kBottomPad = 1.5_mm;
constexpr float kSide = 2.0_mm;
constexpr float kPlateH =
    kBand + kCreditRow + kFieldH + kCaptionRow + kRowGap + kStatusRow + kBottomPad;

constexpr float kTileText = 3.5_mm;
constexpr float kTilePad = 1.5_mm;     // horizontal padding inside a tile
constexpr float kTileGap = 1.3_mm;     // visual width of the gap between tiles
constexpr float kEmptyTile = 1.31_cm;  // the empty program slot

// Text baseline within the field, measured up from the field's bottom edge.
constexpr float kBaseline = kFieldH - 5.8_mm;

constexpr uint32_t kSeed = 0xC3D;

// Caret coordinates are flat byte offsets into the canonical join of argv
// with single separators: position i sits before byte i. One separator byte
// stands between consecutive elements, so flat indices map 1:1 onto
// (tile, offset) pairs even when an element itself contains spaces.

size_t JoinLength(const Vec<Str>& argv) {
  if (argv.empty()) return 0;
  size_t n = argv.size() - 1;
  for (auto& w : argv) n += w.size();
  return n;
}

struct TilePos {
  int tile;
  int offset;
};

TilePos ResolveFlat(const Vec<Str>& argv, int flat) {
  if (argv.empty()) return {0, 0};
  int t = 0;
  for (;;) {
    int len = (int)argv[t].size();
    if (flat <= len || t == (int)argv.size() - 1) return {t, std::min(flat, len)};
    flat -= len + 1;
    ++t;
  }
}

int FlatOf(const Vec<Str>& argv, int tile, int offset) {
  int flat = 0;
  for (int i = 0; i < tile; ++i) flat += (int)argv[i].size() + 1;
  return flat + offset;
}

int PrevCodepoint(StrView s, int i) {
  do {
    --i;
  } while (i > 0 && (s[i] & 0xC0) == 0x80);
  return i;
}

int NextCodepoint(StrView s, int i) {
  do {
    ++i;
  } while (i < (int)s.size() && (s[i] & 0xC0) == 0x80);
  return i;
}

Str StripControl(StrView text) {
  Str out;
  for (char c : text) {
    if ((unsigned char)c >= 0x20) out += c;
  }
  return out;
}

// Positions for every caret index, plus one tile span per argv element.
struct ArgvLayout {
  struct Tile {
    int tile;        // index into argv
    int flat_begin;  // caret index of the element's first byte
    float x0, x1;    // tile span
  };
  Vec<Tile> tiles;
  Vec<float> char_x;  // caret x for flat index 0..JoinLength
  float total = kEmptyTile;
};

ArgvLayout LayoutArgv(const Vec<Str>& argv) {
  ArgvLayout l;
  l.char_x.resize(JoinLength(argv) + 1);
  if (argv.empty()) {
    l.char_x[0] = kTilePad;
    return l;
  }
  float x = 0;
  int flat = 0;
  for (int t = 0; t < (int)argv.size(); ++t) {
    if (t) {
      // The caret position before the separator was already placed at the end
      // of the previous tile; the gap itself holds no caret position.
      x += kTileGap;
      flat += 1;
    }
    const Str& word = argv[t];
    float glyph_base = x + kTilePad;
    float prev = 0;
    for (int k = 0; k < (int)word.size(); ++k) {
      if ((word[k] & 0xC0) == 0x80) {  // UTF-8 continuation byte: same caret slot
        l.char_x[flat + k] = glyph_base + prev;
        continue;
      }
      prev = ui::beta::TextWidth(StrView(word).substr(0, k), kTileText);
      l.char_x[flat + k] = glyph_base + prev;
    }
    float w = ui::beta::TextWidth(word, kTileText);
    l.char_x[flat + (int)word.size()] = glyph_base + w;
    float x1 = x + w + 2 * kTilePad;
    if (word.empty()) x1 += 1.5_mm;  // a small tile, forming
    l.tiles.push_back({t, flat, x, x1});
    x = x1;
    flat += (int)word.size();
  }
  l.total = x;
  return l;
}

}  // namespace

Vec2 CommandPlateSize() { return Vec2(kPlateW, kPlateH); }

struct CommandToy;

// The argv editor. The backing model is the object's argv vector; one tile
// per element. The space KEY always commits a tile (that is the teaching
// gesture); spaces INSIDE an element - arriving via deserialization, links
// or future paste - stay literal and render as a gray midline dot.
struct ArgvField : ui::TextFieldBase {
  CommandToy& toy;
  ArgvField(ui::Widget* parent, CommandToy& toy);
  StrView Name() const override { return "ArgvField"; }
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
  void TextVisit(const ui::TextVisitor&) override;
  int IndexFromPosition(float x) const override;
  Vec2 PositionFromIndex(int index) const override;
  void KeyDown(ui::Caret&, ui::Key) override;
  Vec<Str> Snapshot() const;
};

struct CommandToy : ui::beta::ObjectToy {
  std::unique_ptr<ArgvField> field;
  std::unique_ptr<ui::beta::RunButton> button;

  // Tick-cached object state (UI thread only):
  Vec<Str> argv_;
  Str program_;
  bool program_resolves_ = false;
  bool running_ = false;
  bool ever_ran_ = false;
  I64 pid_ = 0;
  int wait_status_ = 0;
  float spinner_phase_ = 0;
  float field_scale_ = 1;
  float first_tile_x1_ = kEmptyTile;
  bool has_args_ = false;
  Str resolve_query_;
  bool resolve_answer_ = false;

  CommandToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    field = std::make_unique<ArgvField>(this, *this);
    button = std::make_unique<ui::beta::RunButton>(this, [this] { OnButton(); }, Seed(0x12B));
    UpdateFromObject();
  }

  Ptr<Command> LockCommand() const { return LockObject<Command>(); }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kPlateH), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&decltype(Command::out_stream)::tbl)) {
      // The stdout port exits at the lower left, clear of the run disc.
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kPlateH / 2), .dir = -90_deg};
    }
    return ui::beta::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  void UpdateFromObject() {
    Vec<Str> old_argv = argv_;
    if (auto cmd = LockCommand()) {
      auto lock = std::lock_guard(cmd->mutex);
      argv_ = cmd->argv;
      running_ = cmd->child_pid != 0;
      pid_ = cmd->child_pid;
      ever_ran_ = cmd->ever_ran;
      wait_status_ = cmd->wait_status;
    }
    if (argv_ != old_argv) field->WakeAnimation();
    program_ = argv_.empty() ? Str{} : argv_[0];
    if (program_ != resolve_query_) {
      resolve_query_ = program_;
#if defined(_WIN32)
      resolve_answer_ = false;
#else
      resolve_answer_ = ResolvesOnPath(program_);
#endif
    }
    program_resolves_ = resolve_answer_;

    ArgvLayout l = LayoutArgv(argv_);
    has_args_ = argv_.size() > 1;
    first_tile_x1_ = l.tiles.empty() ? kEmptyTile : l.tiles[0].x1;
    float natural = std::max(l.total, kEmptyTile);
    float avail = kPlateW - 2 * kSide;
    field_scale_ = std::min(1.f, avail / natural);

    float left = -kPlateW / 2 + kSide;
    float field_bottom = kPlateH / 2 - (kBand + kCreditRow + kFieldH);
    field->local_to_parent =
        SkM44::Translate(left, field_bottom) * SkM44::Scale(field_scale_, field_scale_, 1);

    bool enabled = running_ || program_resolves_;
    if (running_ != button->running || enabled != button->enabled) {
      button->running = running_;
      button->enabled = enabled;
      button->WakeAnimation();
    }
  }

  Tock Tick(time::Timer& t) override {
    UpdateFromObject();
    if (running_) {
      spinner_phase_ = fmodf(spinner_phase_ + (float)t.d * 0.7f, 1.f);
      return Tock::Drawing;
    }
    return Tock::Draw;
  }

  void ScheduleRunIfReady() {
    if (auto cmd = LockCommand()) {
      if (!cmd->running->IsRunning() && program_resolves_) cmd->run->ScheduleRun();
    }
    WakeAnimation();
  }

  void OnButton() {
    if (auto cmd = LockCommand()) {
      if (cmd->running->IsRunning()) {
        cmd->running->Cancel();
      } else if (program_resolves_) {
        cmd->run->ScheduleRun();
      }
    }
    WakeAnimation();
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kPlateH), "Command", ui::beta::kBlue,
                    ui::beta::State::Default, Seed(kSeed), true);

    {
      StrView credit = "posix_spawnp()";
      float w = ui::beta::TextWidth(credit, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit, {kPlateW / 2 - kSide - w, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    {
      float cap_y = kPlateH / 2 - (kBand + kCreditRow + kFieldH + 1.6_mm);
      ui::beta::DrawText(canvas, "program", {-kPlateW / 2 + kSide + 0.6_mm, cap_y},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      if (has_args_) {
        float ax = -kPlateW / 2 + kSide + (first_tile_x1_ + kTileGap) * field_scale_;
        ui::beta::DrawText(canvas, "arguments...", {ax, cap_y}, ui::beta::kMicroSize,
                           ui::beta::kInkSoft, false, Seed(kSeed));
      }
    }

    {  // stdio port labels: stdout above its port, stdin where connections land
      ui::beta::DrawText(canvas, "stdout", {-kPlateW / 2 + 6.2_mm, -kPlateH / 2 + 0.8_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      StrView stdin_label = "stdin";
      float stdin_w = ui::beta::TextWidth(stdin_label, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, stdin_label, {-stdin_w / 2, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    // Status corner, lower-left: pid while alive, exit chip afterwards.
    // (The run disc sits at the lower center, so the row splits naturally:
    // readouts left of it, the right side free for future affordances.)
    float row_mid = -kPlateH / 2 + kBottomPad + kStatusRow * 0.5f;
    if (running_) {
      ui::beta::Spinner(canvas, {-kPlateW / 2 + kSide + 1.75_mm, row_mid}, 1.6_mm, spinner_phase_,
                        Seed(Hash2(kSeed, 0x59)));
      ui::beta::DrawText(canvas, f("pid {}", pid_),
                         {-kPlateW / 2 + kSide + 4.7_mm, row_mid - 0.7_mm},
                         ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk, false, Seed(kSeed));
    } else if (ever_ran_) {
      Str label;
      SkColor color;
#if !defined(_WIN32)
      if (WIFEXITED(wait_status_)) {
        int code = WEXITSTATUS(wait_status_);
        label = f("exit {}", code);
        color = code == 0 ? ui::beta::kGreen : ui::beta::kRed;
      } else if (WIFSIGNALED(wait_status_)) {
        int sig = WTERMSIG(wait_status_);
        const char* abbr = sigabbrev_np(sig);
        label = abbr ? f("SIG{}", abbr) : f("signal {}", sig);
        color = ui::beta::kRed;
      } else
#endif
      {
        label = "done";
        color = ui::beta::kGreen;
      }
      float w = ui::beta::TextWidth(label, ui::beta::kMicroSize + 0.3_mm) + 2.6_mm;
      float chip_left = -kPlateW / 2 + kSide;
      float chip_bottom = row_mid - 1.6_mm;
      Rect chip{chip_left, chip_bottom, chip_left + w, chip_bottom + 3.2_mm};
      uint32_t cs = Seed(Hash2(kSeed, 0xC1));
      SkPath path = ui::beta::WonkyRoundRect(chip, 1.2_mm, ui::beta::kWonk * 0.8f, cs);
      ui::beta::HandShadow(canvas, path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, cs);
      ui::beta::MisregFill(canvas, path, color, cs);
      ui::beta::SketchyStroke(canvas, path, ui::beta::kInk, ui::beta::kStroke * 0.8f, cs, 1);
      ui::beta::DrawTextIn(canvas, label, chip, ui::beta::kMicroSize + 0.3_mm,
                           ui::beta::TextOn(color), ui::beta::TextAlign::Center, false, cs);
    }
    BakeChildren(canvas);
  }
};

// ---------------------------------------------------------------- ArgvField --

ArgvField::ArgvField(ui::Widget* parent, CommandToy& toy) : TextFieldBase(parent), toy(toy) {}

Vec<Str> ArgvField::Snapshot() const {
  if (auto cmd = toy.LockCommand()) {
    auto lock = std::lock_guard(cmd->mutex);
    return cmd->argv;
  }
  return {};
}

SkPath ArgvField::Shape() const {
  float w = std::max(LayoutArgv(Snapshot()).total, kEmptyTile);
  return SkPath::Rect(Rect{0, 0, w, kFieldH});
}

void ArgvField::TextVisit(const ui::TextVisitor& visitor) {
  // Base-class compatibility only; the editor manipulates argv directly.
  Str joined;
  if (auto cmd = toy.LockCommand()) joined = cmd->GetText();
  if (visitor(joined)) {
    if (auto cmd = toy.LockCommand()) cmd->SetText(joined);
    WakeAnimation();
    toy.WakeAnimation();
  }
}

int ArgvField::IndexFromPosition(float x) const {
  Vec<Str> argv = Snapshot();
  if (argv.empty()) return 0;
  ArgvLayout l = LayoutArgv(argv);
  int join_len = (int)JoinLength(argv);
  int best = 0;
  float best_d = 1e9f;
  for (int i = 0; i <= join_len; ++i) {
    auto [t, off] = ResolveFlat(argv, i);
    if (off < (int)argv[t].size() && (argv[t][off] & 0xC0) == 0x80) continue;  // mid-codepoint
    float d = fabsf(l.char_x[i] - x);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

Vec2 ArgvField::PositionFromIndex(int index) const {
  Vec<Str> argv = Snapshot();
  ArgvLayout l = LayoutArgv(argv);
  index = std::clamp(index, 0, (int)JoinLength(argv));
  return Vec2(l.char_x[index], kBaseline);
}

void ArgvField::KeyDown(ui::Caret& caret, ui::Key k) {
  using enum ui::AnsiKey;
  if (k.physical == Enter || k.physical == NumpadEnter) {
    toy.ScheduleRunIfReady();
    return;
  }
  auto cmd = toy.LockCommand();
  if (!cmd) return;
  int& index = caret_positions[&caret].index;
  bool modified = false;
  {
    auto lock = std::lock_guard(cmd->mutex);
    auto& argv = cmd->argv;
    auto [t, off] = ResolveFlat(argv, index);
    switch (k.physical) {
      case Backspace:
        if (argv.empty()) break;
        if (off > 0) {
          int prev = PrevCodepoint(argv[t], off);
          argv[t].erase(prev, off - prev);
          index = FlatOf(argv, t, prev);
          modified = true;
        } else if (t > 0) {
          // Joining tiles deletes the gap; literal spaces only ever come from
          // atomic insertion, never from edits.
          int new_off = (int)argv[t - 1].size();
          argv[t - 1] += argv[t];
          argv.erase(argv.begin() + t);
          index = FlatOf(argv, t - 1, new_off);
          modified = true;
        }
        break;
      case Delete:
        if (argv.empty()) break;
        if (off < (int)argv[t].size()) {
          int next = NextCodepoint(argv[t], off);
          argv[t].erase(off, next - off);
          modified = true;
        } else if (t + 1 < (int)argv.size()) {
          argv[t] += argv[t + 1];
          argv.erase(argv.begin() + t + 1);
          modified = true;
        }
        break;
      case Left:
        if (argv.empty()) break;
        if (off > 0) {
          index = FlatOf(argv, t, PrevCodepoint(argv[t], off));
        } else if (t > 0) {
          index -= 1;
        }
        break;
      case Right:
        if (argv.empty()) break;
        if (off < (int)argv[t].size()) {
          index = FlatOf(argv, t, NextCodepoint(argv[t], off));
        } else if (t + 1 < (int)argv.size()) {
          index += 1;
        }
        break;
      case Home:
        index = 0;
        break;
      case End:
        index = (int)JoinLength(argv);
        break;
      default: {
        Str clean = StripControl(k.text);
        if (clean.empty()) break;
        if (argv.empty()) {
          argv.push_back("");
          t = 0;
          off = 0;
        }
        // Keystroke spaces commit tiles - that is the teaching gesture. Only
        // atomic insertions may put a space INSIDE an element.
        size_t s = 0;
        while (s <= clean.size()) {
          size_t sp = clean.find(' ', s);
          StrView seg = StrView(clean).substr(s, sp == StrView::npos ? clean.size() - s : sp - s);
          argv[t].insert(off, seg);
          off += (int)seg.size();
          if (sp == StrView::npos) break;
          Str tail = argv[t].substr(off);
          argv[t].resize(off);
          argv.insert(argv.begin() + t + 1, tail);
          ++t;
          off = 0;
          s = sp + 1;
        }
        index = FlatOf(argv, t, off);
        modified = true;
      }
    }
  }
  if (modified) {
    cmd->WakeToys();
    WakeAnimation();
    toy.WakeAnimation();
  }
  UpdateCaret(caret);
}

void ArgvField::Draw(SkCanvas& canvas) const {
  Vec<Str> argv = Snapshot();
  ArgvLayout l = LayoutArgv(argv);

  if (l.tiles.empty()) {  // the empty program slot, waiting for a name
    Rect r{0, 0.9_mm, kEmptyTile, kFieldH - 0.9_mm};
    uint32_t s = toy.Seed(Hash2(kSeed, 1));
    SkPath outline = ui::beta::WonkyRoundRect(r, 1.0_mm, ui::beta::kWonk, s);
    ui::beta::MisregFill(canvas, outline, ui::beta::kPaper, s);
    ui::beta::SketchyStroke(canvas, outline, ui::beta::kInkSoft, ui::beta::kStroke, s, 1);
    return;
  }

  for (auto& tile : l.tiles) {
    bool first = tile.tile == 0;
    const Str& word = argv[tile.tile];
    // The program tile stands taller than the args (smaller inset).
    float inset = first ? 0.45_mm : 1.0_mm;
    Rect r{tile.x0, inset, tile.x1, kFieldH - inset};
    uint32_t s = toy.Seed(Hash2(kSeed, (uint32_t)tile.tile + 2));
    SkPath outline = ui::beta::WonkyRoundRect(r, 1.0_mm, ui::beta::kWonk, s);
    bool bad = first && !toy.program_resolves_;
    ui::beta::MisregFill(canvas, outline, ui::beta::kPaper, s);
    ui::beta::SketchyStroke(canvas, outline, bad ? ui::beta::kRed : ui::beta::kInk,
                            first ? ui::beta::kStrokeBold : ui::beta::kStroke, s, first ? 2 : 1);
    ui::beta::DrawText(canvas, word, {tile.x0 + kTilePad, kBaseline}, kTileText, ui::beta::kInk,
                       false, s);
    // A literal space inside the element: content stays ink, structure gets a
    // gray midline dot so the invisible byte is visibly NOT a tile boundary.
    for (int k = 0; k < (int)word.size(); ++k) {
      if (word[k] != ' ') continue;
      float cx = (l.char_x[tile.flat_begin + k] + l.char_x[tile.flat_begin + k + 1]) * 0.5f;
      SkPaint dot;
      dot.setColor(ui::beta::kGrayDark);
      dot.setAntiAlias(true);
      canvas.drawCircle(cx, kBaseline + kTileText * 0.28f, 0.32_mm, dot);
    }
    if (bad) {  // not found on PATH: the brand error squiggle
      canvas.drawPath(
          ui::beta::WobbleLine({tile.x0 + kTilePad, kBaseline - 1.0_mm},
                               {tile.x1 - kTilePad, kBaseline - 1.0_mm}, 0.45_mm, 0.7_mm, s),
          ui::beta::InkPaint(ui::beta::kRed, ui::beta::kStrokeHair));
    }
  }
}

// ----------------------------------------------------------------- MakeToy --

std::unique_ptr<ObjectToy> Command::MakeToy(ui::Widget* parent) {
  return std::make_unique<CommandToy>(parent, *this);
}

}  // namespace automat::library
