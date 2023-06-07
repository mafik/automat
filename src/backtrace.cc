#include "backtrace.hh"

#include <signal.h>

#include <memory>

#if defined(__linux__)

#include <sys/prctl.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstring>

/**
 * Based on public domain code by Jaco Kroon.
 *
 * TODO: use the stack trace printing code in log.cc
 */

bool PrintBacktrace() {
  char pid_buf[30];
  sprintf(pid_buf, "%d", getpid());
  char name_buf[512];
  name_buf[readlink("/proc/self/exe", name_buf, 511)] = 0;
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
  int child_pid = fork();
  if (!child_pid) {
    dup2(2, 1);  // redirect output to stderr - edit: unnecessary?
    execl("/usr/bin/gdb", "gdb", "--batch", "-n", "-ex", "thread", "-ex", "bt", name_buf, pid_buf,
          NULL);
    abort();  // If gdb failed to start
  } else {
    waitpid(child_pid, NULL, 0);
  }
  return true;
}

static void signal_segv_unix(int signum, siginfo_t* info, void* ptr) {
  static const char* si_codes[3] = {"", "SEGV_MAPERR", "SEGV_ACCERR"};

  printf(R"(Segmentation Fault!
  siginfo_t.si_signo = %d
  siginfo_t.si_errno = %d
  siginfo_t.si_code  = %d (%s)
  siginfo_t.si_addr  = %p
GDB Stack trace:
)",
         signum, info->si_errno, info->si_code, si_codes[info->si_code], info->si_addr);

  PrintBacktrace();

  _exit(-1);
}

void EnableBacktraceOnSIGSEGV() {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = signal_segv_unix;
  action.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSEGV, &action, NULL) < 0) perror("sigaction");
}

#elif defined(_WIN32)

#include <Windows.h>

bool PrintBacktrace() {
  int pid = GetCurrentProcessId();
  char file_name[MAX_PATH];
  GetModuleFileName(NULL, file_name, MAX_PATH);
  char args[512];
  snprintf(args, sizeof(args),
           "gdb --batch -n -iex \"set print thread-events off\" -ex \"info "
           "threads\" -ex \"thread 1\" -ex bt \"%s\" %d",
           file_name, pid);
  STARTUPINFO si = {};
  PROCESS_INFORMATION pi = {};
  if (!CreateProcess(nullptr, args, nullptr, nullptr, 0, 0, nullptr, nullptr, &si, &pi)) {
    char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(),
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, (sizeof(buf) / sizeof(wchar_t)),
                   NULL);
    printf("  CreateProcess failed with error code %lu: %s", GetLastError(), buf);
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  unsigned long exitCode;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return exitCode == 0;
}

static void signal_segv_win32(int signum) {
  printf("Program accessed invalid memory and will shut down.\n");
  printf("Attempting to get stack trace to help in fixing this problem...\n");

  if (!PrintBacktrace()) {
    printf(
        "Most likely the GDB debugger was not found.\n  It can be "
        "downloaded from https://github.com/ssbssa/gdb/releases.\n  It "
        "should also be added to system PATH variable.");
  }

  _exit(-1);
}

void EnableBacktraceOnSIGSEGV() {
  if (signal(SIGSEGV, &signal_segv_win32) == SIG_ERR) perror("sigaction");
}

#endif
