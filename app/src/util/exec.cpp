#include "util/exec.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>

#include "util/strings.h"

namespace btb {

namespace {

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool executable(const std::string& p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
}

// Everything after fork() in the child must be async-signal-safe (another thread may hold malloc's
// lock at the moment of the fork), so argv is built before and the child only dup2s and execs.
void child_exec(const std::vector<char*>& av, int out_w, int err_w) {
  setpgid(0, 0);
  const int devnull = open("/dev/null", O_RDONLY);
  if (devnull >= 0) dup2(devnull, STDIN_FILENO);
  dup2(out_w, STDOUT_FILENO);
  dup2(err_w, STDERR_FILENO);
  // The daemon's own descriptors (the HCI monitor socket, the bus, listening sockets) must not
  // leak into a tool it runs; most are CLOEXEC already, this covers the rest.
  const long max = sysconf(_SC_OPEN_MAX);
  for (int fd = 3; fd < (max > 0 && max < 4096 ? max : 4096); ++fd) close(fd);
  // A child inherits the daemon's ignored SIGPIPE; tools expect the default.
  signal(SIGPIPE, SIG_DFL);
  execv(av[0], av.data());
  const char msg[] = "exec failed\n";
  (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
  _exit(127);
}

}  // namespace

std::string ExecResult::reason() const {
  if (!started) return err.empty() ? std::string("could not run it") : trim(err);
  if (timed_out) return "timed out";
  const std::vector<std::string> lines = split(trim(err), '\n');
  if (!lines.empty()) return trim(lines.back());
  const std::vector<std::string> olines = split(trim(out), '\n');
  if (status != 0 && !olines.empty()) return trim(olines.back());
  return "exit status " + std::to_string(status);
}

std::string find_exec(const std::string& name, const std::string& dirs) {
  if (name.empty()) return {};
  if (name.find('/') != std::string::npos) return executable(name) ? name : std::string{};
  const char* p = getenv("PATH");
  std::string all = dirs;
  if (p) all += std::string(all.empty() ? "" : ":") + p;
  else all += std::string(all.empty() ? "" : ":") + "/usr/sbin:/usr/bin:/sbin:/bin";
  for (const std::string& d : split(all, ':')) {
    const std::string full = d + "/" + name;
    if (executable(full)) return full;
  }
  return {};
}

bool spawn(const std::vector<std::string>& argv, Spawned* out, std::string* err) {
  if (argv.empty()) {
    if (err) *err = "empty command";
    return false;
  }
  const std::string path = find_exec(argv[0]);
  if (path.empty()) {
    if (err) *err = argv[0] + ": not found";
    return false;
  }
  int o[2], e[2];
  if (pipe2(o, O_CLOEXEC) != 0) {
    if (err) *err = strerror(errno);
    return false;
  }
  if (pipe2(e, O_CLOEXEC) != 0) {
    if (err) *err = strerror(errno);
    close(o[0]);
    close(o[1]);
    return false;
  }
  std::vector<std::string> args = argv;
  args[0] = path;
  std::vector<char*> av;
  for (std::string& a : args) av.push_back(a.data());
  av.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    if (err) *err = std::string("fork: ") + strerror(errno);
    close(o[0]); close(o[1]); close(e[0]); close(e[1]);
    return false;
  }
  if (pid == 0) child_exec(av, o[1], e[1]);
  // Also from here, so a kill_group() racing the child's own setpgid() still finds the group.
  setpgid(pid, pid);
  close(o[1]);
  close(e[1]);
  out->pid = pid;
  out->out_fd = o[0];
  out->err_fd = e[0];
  return true;
}

void kill_group(pid_t pid, int grace_ms) {
  if (pid <= 0) return;
  kill(-pid, SIGTERM);
  const int64_t until = now_ms() + grace_ms;
  while (now_ms() < until) {
    // Still there? kill(…, 0) on a zombie succeeds, so ask waitpid without reaping.
    siginfo_t si{};
    const int rc = waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT);
    if (rc == 0 && si.si_pid == pid) return;
    // Reaped by its owner meanwhile: its group id may be anyone's soon, so leave it be.
    if (rc < 0 && errno == ECHILD) return;
    usleep(20000);
  }
  kill(-pid, SIGKILL);
}

ExecResult run_cmd(const std::vector<std::string>& argv, int timeout_ms, size_t max_bytes) {
  ExecResult r;
  Spawned s;
  std::string err;
  if (!spawn(argv, &s, &err)) {
    r.err = err;
    return r;
  }
  r.started = true;
  const int64_t deadline = now_ms() + timeout_ms;
  bool out_open = true, err_open = true;
  char buf[4096];
  while (out_open || err_open) {
    const int64_t left = deadline - now_ms();
    if (left <= 0) {
      r.timed_out = true;
      kill_group(s.pid, 500);
      break;
    }
    pollfd p[2] = {{out_open ? s.out_fd : -1, POLLIN, 0}, {err_open ? s.err_fd : -1, POLLIN, 0}};
    const int n = poll(p, 2, static_cast<int>(left));
    if (n < 0 && errno != EINTR) break;
    for (int i = 0; i < 2; ++i) {
      if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const ssize_t got = read(p[i].fd, buf, sizeof(buf));
      if (got <= 0) {
        (i == 0 ? out_open : err_open) = false;
        continue;
      }
      std::string& dst = i == 0 ? r.out : r.err;
      if (dst.size() < max_bytes) dst.append(buf, std::min(static_cast<size_t>(got), max_bytes - dst.size()));
    }
  }
  close(s.out_fd);
  close(s.err_fd);
  // The pipes closing is not the child exiting (it may have closed them itself), so this wait is
  // bounded too.
  int status = 0;
  for (;;) {
    const pid_t w = waitpid(s.pid, &status, r.timed_out ? 0 : WNOHANG);
    if (w == s.pid) break;
    if (w < 0 && errno != EINTR) break;
    if (now_ms() > deadline + 200) {
      r.timed_out = true;
      kill_group(s.pid, 500);
      waitpid(s.pid, &status, 0);
      break;
    }
    usleep(5000);
  }
  if (WIFEXITED(status)) r.status = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) r.status = 128 + WTERMSIG(status);
  return r;
}

}  // namespace btb
