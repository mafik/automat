// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "launcher.hpp"

#include <include/core/SkPathBuilder.h>

#if defined(_WIN32)
#include <io.h>

#include "thread_name.hpp"
#include "win32.hpp"
#include "win32_window_manager.hpp"
#else
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
#include <thread>
#include <unordered_map>
#include <vector>

#include "animation.hpp"
#include "board.hpp"
#include "color.hpp"
#include "deserializer.hpp"
#include "fd.hpp"
#include "format.hpp"
#include "hex.hpp"
#include "library_command.hpp"
#include "log.hpp"
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

static void FireExit(Launch& launch, Vec<std::move_only_function<void()>>&& callbacks) {
  for (auto& cb : callbacks) cb();
  launch.WakeToys();
  if (auto src = launch.source.Lock()) src->WakeToys();
}

void Launch::NotifyOnExit(std::move_only_function<void()> fn) {
  {
    auto lock = std::lock_guard(mutex);
    if (!exited) {
      on_exit.push_back(std::move(fn));
      return;
    }
  }
  fn();
}

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

#if defined(_WIN32)

constexpr DWORD kPipeBuffer = 64 * 1024;

static SECURITY_ATTRIBUTES Inheritable() {
  return SECURITY_ATTRIBUTES{
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
}

static bool MakeCapturePipe(HANDLE& read_end, HANDLE& write_end, Status& status) {
  static std::atomic<uint32_t> counter{0};
  std::wstring name = L"\\\\.\\pipe\\automat-capture-" + std::to_wstring(GetCurrentProcessId()) +
                      L"-" + std::to_wstring(counter.fetch_add(1, std::memory_order_relaxed));
  HANDLE reading = CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0, kPipeBuffer, 0, nullptr);
  if (reading == INVALID_HANDLE_VALUE) {
    AppendErrorMessage(status) += f("CreateNamedPipe: {}", win32::GetLastErrorStr());
    return false;
  }
  auto attributes = Inheritable();
  HANDLE writing = CreateFileW(name.c_str(), GENERIC_WRITE, 0, &attributes, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  if (writing == INVALID_HANDLE_VALUE) {
    AppendErrorMessage(status) += f("Opening the capture pipe: {}", win32::GetLastErrorStr());
    CloseHandle(reading);
    return false;
  }
  read_end = reading;
  write_end = writing;
  return true;
}

// Reads one capture pipe. Lives on the mux thread and deletes itself when the
// program closes its end or the Launch goes away.
struct CaptureReader {
  HANDLE pipe;
  HANDLE event;
  OVERLAPPED overlapped = {};
  WeakPtr<Launch> launch;
  StreamCapture Launch::* capture;
  char buf[4096];

  CaptureReader(HANDLE pipe, WeakPtr<Launch> launch, StreamCapture Launch::* capture)
      : pipe(pipe),
        event(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        launch(std::move(launch)),
        capture(capture) {}

  ~CaptureReader() {
    CancelIoEx(pipe, &overlapped);
    CloseHandle(pipe);
    CloseHandle(event);
  }

  void Start() {
    overlapped = {};
    overlapped.hEvent = event;
    ResetEvent(event);
    if (!ReadFile(pipe, buf, sizeof(buf), nullptr, &overlapped) &&
        GetLastError() != ERROR_IO_PENDING) {
      delete this;
      return;
    }
    Status status;
    mux::WatchHandle(
        event, [this] { Finish(); }, status);
    if (!OK(status)) delete this;
  }

  void Finish() {
    DWORD n = 0;
    if (!GetOverlappedResult(pipe, &overlapped, &n, FALSE) || n == 0) {
      delete this;
      return;
    }
    auto l = launch.Lock();
    if (!l) {
      delete this;
      return;
    }
    {
      auto lock = std::lock_guard(l->mutex);
      Append(l.get()->*capture, buf, n);
    }
    l->WakeToys();
    if (auto src = l->source.Lock()) src->WakeToys();
    Start();
  }
};

static void RegisterCapture(HANDLE pipe, const Ptr<Launch>& launch,
                            StreamCapture Launch::* capture) {
  auto* reader = new CaptureReader(pipe, launch->AcquireWeakPtr(), capture);
  mux::epoll.Post([reader] { reader->Start(); });
}

struct JobWatch {
  std::mutex mutex;
  std::unordered_map<uintptr_t, WeakPtr<Launch>> launches;
  HANDLE port = nullptr;
  uintptr_t next_key = 1;
};

static JobWatch& Jobs() {
  static JobWatch* watch = new JobWatch;
  return *watch;
}

static void JobPortLoop() {
  SetThreadName("JobWatch");
  for (;;) {
    DWORD message = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED info = nullptr;
    GetQueuedCompletionStatus(Jobs().port, &message, &key, &info, INFINITE);
    if (message != JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) continue;
    Ptr<Launch> launch;
    {
      auto lock = std::lock_guard(Jobs().mutex);
      auto it = Jobs().launches.find(key);
      if (it == Jobs().launches.end()) continue;
      launch = it->second.Lock();
      Jobs().launches.erase(it);
    }
    if (!launch) continue;
    Vec<std::move_only_function<void()>> callbacks;
    {
      auto lock = std::lock_guard(launch->mutex);
      launch->exited = true;
      callbacks.swap(launch->on_exit);
    }
    FireExit(*launch, std::move(callbacks));
  }
}

static uintptr_t AssociateJobPort(HANDLE job) {
  auto& jobs = Jobs();
  uintptr_t key;
  {
    auto lock = std::lock_guard(jobs.mutex);
    if (jobs.port == nullptr) {
      jobs.port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
      if (jobs.port == nullptr) return 0;
      std::thread(JobPortLoop).detach();
    }
    key = jobs.next_key++;
  }
  JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc = {.CompletionKey = (PVOID)key,
                                               .CompletionPort = jobs.port};
  if (!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &assoc,
                               sizeof(assoc))) {
    return 0;
  }
  return key;
}

static Str BuildCommandLine(const Vec<Str>& words) {
  Str line;
  for (auto& word : words) {
    if (!line.empty()) line += ' ';
    if (word.find_first_of(" \t\"") == Str::npos) {
      line += word;
      continue;
    }
    line += '"';
    size_t backslashes = 0;
    for (char c : word) {
      if (c == '\\') {
        ++backslashes;
        continue;
      }
      if (c == '"') {
        line.append(backslashes * 2 + 1, '\\');
      } else {
        line.append(backslashes, '\\');
      }
      backslashes = 0;
      line += c;
    }
    line.append(backslashes * 2, '\\');
    line += '"';
  }
  return line;
}

static Ptr<Launch> StartProcess(Vec<Str>& words, const SpawnFds& fds, Status& status) {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    AppendErrorMessage(status) += f("CreateJobObject: {}", win32::GetLastErrorStr());
    return nullptr;
  }
  uintptr_t job_key = AssociateJobPort(job);
  HANDLE err_read = nullptr, err_write = nullptr;
  HANDLE out_read = nullptr, out_write = nullptr;
  HANDLE no_input = nullptr;
  auto give_up = [&]() -> Ptr<Launch> {
    for (HANDLE h : {err_read, err_write, out_read, out_write, no_input, job}) {
      if (h) CloseHandle(h);
    }
    return nullptr;
  };
  if (!MakeCapturePipe(err_read, err_write, status)) return give_up();
  bool capture_out = fds.out == kNoStdio;
  if (capture_out && !MakeCapturePipe(out_read, out_write, status)) return give_up();
  if (fds.in == kNoStdio) {
    auto attributes = Inheritable();
    no_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                           OPEN_EXISTING, 0, nullptr);
    if (no_input == INVALID_HANDLE_VALUE) no_input = nullptr;
  }

  HANDLE child_stdio[3] = {fds.in == kNoStdio ? no_input : (HANDLE)fds.in,
                           capture_out ? out_write : (HANDLE)fds.out, err_write};
  HANDLE inherited[3];
  DWORD inherited_count = 0;
  for (HANDLE h : child_stdio) {
    if (h == nullptr) continue;
    SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    inherited[inherited_count++] = h;
  }

  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  auto attribute_storage = std::make_unique<char[]>(attribute_size);
  auto* attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)attribute_storage.get();
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                 inherited_count * sizeof(HANDLE), nullptr, nullptr)) {
    AppendErrorMessage(status) += f("Preparing the child's handles: {}", win32::GetLastErrorStr());
    return give_up();
  }

  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_stdio[0];
  startup.StartupInfo.hStdOutput = child_stdio[1];
  startup.StartupInfo.hStdError = child_stdio[2];
  startup.lpAttributeList = attributes;

  std::wstring command_line = win32::Utf8ToWide(BuildCommandLine(words));
  command_line.push_back(L'\0');
  PROCESS_INFORMATION process = {};
  BOOL started =
      CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                     CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                     nullptr, &startup.StartupInfo, &process);
  DWORD spawn_error = GetLastError();
  DeleteProcThreadAttributeList(attributes);
  CloseHandle(err_write);
  err_write = nullptr;
  if (capture_out) {
    CloseHandle(out_write);
    out_write = nullptr;
  }
  if (no_input) {
    CloseHandle(no_input);
    no_input = nullptr;
  }
  if (!started) {
    AppendErrorMessage(status) += f("{}: {}", words[0], win32::ErrorStr(spawn_error));
    return give_up();
  }
  bool in_job = AssignProcessToJobObject(job, process.hProcess);
  if (!in_job) {
    ERROR << "Launch " << words[0] << " could not be put in a job: " << win32::GetLastErrorStr();
  }

  auto launch = MAKE_PTR(Launch);
  launch->pid = process.dwProcessId;
  launch->process = process.hProcess;
  launch->job = job;
  launch->job_key = job_key;
  launch->job_tracked = in_job && job_key != 0;
  launch->child_stdin = fds.in;
  if (launch->job_tracked) {
    auto lock = std::lock_guard(Jobs().mutex);
    Jobs().launches[job_key] = launch->AcquireWeakPtr();
  }
  ResumeThread(process.hThread);
  CloseHandle(process.hThread);
  RegisterCapture(err_read, launch, &Launch::err_capture);
  if (capture_out) RegisterCapture(out_read, launch, &Launch::out_capture);
  return launch;
}

#else

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

static Ptr<Launch> StartProcess(Vec<Str>& words, const SpawnFds& fds, Status& status) {
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
  bool capture_out = fds.out == kNoStdio;
  if (capture_out && pipe2(out_pipe, O_CLOEXEC) != 0) {
    AppendErrorMessage(status) += f("pipe2: {}", strerror(errno));
    close(err_pipe[0]);
    close(err_pipe[1]);
    return nullptr;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (fds.in != kNoStdio) posix_spawn_file_actions_adddup2(&actions, fds.in, 0);
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
  launch->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);

  RegisterCapture(FD(err_pipe[0]), launch, &Launch::err_capture);
  if (capture_out) RegisterCapture(FD(out_pipe[0]), launch, &Launch::out_capture);
  return launch;
}

#endif

Ptr<Launch> Launch::Spawn(const Vec<Str>& argv_in, Object* source, ClientWindow* restoring,
                          Status& status, const SpawnFds& fds) {
  Vec<Str> words;
  for (auto& w : argv_in) {
    if (!w.empty()) words.push_back(w);
  }
  if (words.empty()) {
    AppendErrorMessage(status) += "Nothing to run.";
    return nullptr;
  }

  auto launch = StartProcess(words, fds, status);
  if (!launch) return nullptr;

  launch->when = time::SteadyNow();
  launch->argv = std::move(words);
  if (source) launch->source = source->AcquireWeakPtr();
  if (restoring) launch->restoring = restoring->AcquireWeakPtr();
  if (fds.in_pipe) fds.in_pipe->reader = launch->AcquireWeakPtr();
  if (fds.out_pipe) launch->stdout_pipe = fds.out_pipe;

  {
    auto lock = std::lock_guard(registry_mutex);
    std::erase_if(registry, [](WeakPtr<Launch>& w) { return w.IsExpired(); });
    registry.push_back(launch->AcquireWeakPtr());
  }

  Status watch_status;
  mux::WatchProcess(
      (int)launch->pid,
      [weak = launch->AcquireWeakPtr()](int wait_status) {
        if (auto l = weak.Lock()) {
          Vec<std::move_only_function<void()>> callbacks;
          {
            auto lock = std::lock_guard(l->mutex);
            l->wait_status = wait_status;
#if defined(_WIN32)
            if (!l->job_tracked) l->exited = true;
#else
            l->exited = true;
#endif
            if (l->exited) callbacks.swap(l->on_exit);
          }
          FireExit(*l, std::move(callbacks));
        }
      },
      watch_status);
  if (!OK(watch_status)) {
    auto lock = std::lock_guard(launch->mutex);
#if defined(_WIN32)
    if (!launch->job_tracked) launch->exited = true;
#else
    launch->exited = true;
#endif
  }
  return launch;
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
      if (auto l = w.Lock(); l && l->OwnsProcess(client_pid)) return l;
    }
  }
  return nullptr;
}

bool Launch::OwnsProcess(I64 candidate_pid) {
  if (candidate_pid == pid) return true;
#if defined(_WIN32)
  if (job == nullptr) return false;
  HANDLE candidate = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)candidate_pid);
  if (candidate == nullptr) return false;
  BOOL inside = FALSE;
  bool owns = IsProcessInJob(candidate, (HANDLE)job, &inside) && inside;
  CloseHandle(candidate);
  return owns;
#else
  return false;
#endif
}

void Launch::Terminate(bool keep_connected) {
#if defined(_WIN32)
  if (win32_wm::CloseWindowsOf(*this, keep_connected)) return;
  if (job) TerminateJobObject((HANDLE)job, 1);
#else
  (void)keep_connected;
  if (pid > 0) kill((pid_t)pid, SIGTERM);
#endif
}

Launch::~Launch() {
#if defined(_WIN32)
  if (job_key) {
    auto lock = std::lock_guard(Jobs().mutex);
    Jobs().launches.erase(job_key);
  }
  if (!exited && !window_appeared) Terminate();
  if (process) CloseHandle((HANDLE)process);
  if (job) CloseHandle((HANDLE)job);
#else
  if (pidfd >= 0) close(pidfd);
  if (!exited && !window_appeared && pid > 0) kill((pid_t)pid, SIGTERM);
#endif
}

bool MakeStdioPipe(StdioHandle& read_end, StdioHandle& write_end, Status& status) {
#if defined(_WIN32)
  auto attributes = Inheritable();
  HANDLE reading = nullptr, writing = nullptr;
  if (!CreatePipe(&reading, &writing, &attributes, kPipeBuffer)) {
    AppendErrorMessage(status) += f("CreatePipe: {}", win32::GetLastErrorStr());
    return false;
  }
  read_end = reading;
  write_end = writing;
  return true;
#else
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    AppendErrorMessage(status) += f("pipe2: {}", strerror(errno));
    return false;
  }
  read_end = fds[0];
  write_end = fds[1];
  return true;
#endif
}

void CloseStdio(StdioHandle handle) {
  if (handle == kNoStdio) return;
#if defined(_WIN32)
  CloseHandle((HANDLE)handle);
#else
  close(handle);
#endif
}

StdioHandle AdoptFileDescriptor(int fd) {
  if (fd < 0) return kNoStdio;
#if defined(_WIN32)
  HANDLE opened = (HANDLE)_get_osfhandle(fd);
  HANDLE owned = nullptr;
  if (opened != INVALID_HANDLE_VALUE) {
    DuplicateHandle(GetCurrentProcess(), opened, GetCurrentProcess(), &owned, 0, TRUE,
                    DUPLICATE_SAME_ACCESS);
  }
  _close(fd);
  return owned;
#else
  return fd;
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
#if defined(_WIN32)
  bool alive;
  Ptr<Pipe> pipe;
  {
    auto lock = std::lock_guard(mutex);
    alive = !exited;
    if (alive && process) {
      IO_COUNTERS counters = {};
      if (GetProcessIoCounters((HANDLE)process, &counters)) io_wchar = counters.WriteTransferCount;
    }
    stats.bytes = io_wchar;
    pipe = stdout_pipe;
  }
  if (!alive || !pipe) return stats;

  auto peer = pipe->reader.Lock();
  if (!peer) return stats;
  HANDLE peer_process = nullptr;
  StdioHandle peer_stdin = kNoStdio;
  {
    auto lock = std::lock_guard(peer->mutex);
    if (!peer->exited) {
      peer_process = (HANDLE)peer->process;
      peer_stdin = peer->child_stdin;
    }
  }
  if (peer_process == nullptr || peer_stdin == kNoStdio) return stats;
  HANDLE borrowed = nullptr;
  if (DuplicateHandle(peer_process, (HANDLE)peer_stdin, GetCurrentProcess(), &borrowed, 0, FALSE,
                      DUPLICATE_SAME_ACCESS)) {
    DWORD waiting = 0;
    if (PeekNamedPipe(borrowed, nullptr, 0, nullptr, &waiting, nullptr)) stats.fill = waiting;
    DWORD to_child = 0, from_parent = 0;
    if (GetNamedPipeInfo(borrowed, nullptr, &to_child, &from_parent, nullptr)) {
      stats.capacity = from_parent ? from_parent : to_child;
    }
    CloseHandle(borrowed);
  }
#else
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
