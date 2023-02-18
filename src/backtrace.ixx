export module backtrace;

import "fmt/format.h";
import <memory>;
import <sys/wait.h>;
import <sys/prctl.h>;

/**
 * Based on public domain code by Jaco Kroon.
 *
 * TODO: use the stack trace printing code in log.cc
 */

static void print_trace() {
    char pid_buf[30];
    sprintf(pid_buf, "%d", getpid());
    char name_buf[512];
    name_buf[readlink("/proc/self/exe", name_buf, 511)]=0;
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    int child_pid = fork();
    if (!child_pid) {
        dup2(2,1); // redirect output to stderr - edit: unnecessary?
        execl("/usr/bin/gdb", "gdb", "--batch", "-n", "-ex", "thread", "-ex", "bt", name_buf, pid_buf, NULL);
        abort(); // If gdb failed to start
    } else {
        waitpid(child_pid,NULL,0);
    }
}

static void signal_segv(int signum, siginfo_t *info, void *ptr) {
  static const char *si_codes[3] = {"", "SEGV_MAPERR", "SEGV_ACCERR"};

  fmt::print(R"(Segmentation Fault!
  siginfo_t.si_signo = {}
  siginfo_t.si_errno = {}
  siginfo_t.si_code  = {} ({})
  siginfo_t.si_addr  = {}
GDB Stack trace:
)", signum, info->si_errno, info->si_code, si_codes[info->si_code], info->si_addr);

  print_trace();

  _exit(-1);
}

export void EnableBacktraceOnSIGSEGV() {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = signal_segv;
  action.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSEGV, &action, NULL) < 0)
    perror("sigaction");
}
