#include "capture.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace btb::hci {

using nlohmann::json;

namespace {

int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// In the child, between fork and exec: only async-signal-safe calls.
void close_other_fds() {
#ifdef SYS_close_range
  if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
  long max = sysconf(_SC_OPEN_MAX);
  if (max < 0 || max > 4096) max = 4096;
  for (int fd = 3; fd < max; ++fd) close(fd);
}

void sleep_ms(int ms) {
  struct timespec ts {
    0, ms * 1000000L
  };
  nanosleep(&ts, nullptr);
}

}  // namespace

CommandResult run_command(const std::vector<std::string>& argv, int timeout_ms,
                          size_t max_output) {
  CommandResult r;
  if (argv.empty()) {
    r.error = "empty command";
    return r;
  }
  // Everything the child needs is prepared before fork: a multithreaded parent's child may not
  // allocate.
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);

  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) < 0) {
    r.error = std::string("pipe: ") + std::strerror(errno);
    return r;
  }
  const int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);

  const pid_t pid = fork();
  if (pid < 0) {
    r.error = std::string("fork: ") + std::strerror(errno);
    close(pipefd[0]);
    close(pipefd[1]);
    if (devnull >= 0) close(devnull);
    return r;
  }
  if (pid == 0) {
    if (devnull >= 0) dup2(devnull, 0);
    dup2(pipefd[1], 1);
    dup2(pipefd[1], 2);
    close_other_fds();
    execvp(args[0], args.data());
    _exit(127);
  }

  close(pipefd[1]);
  if (devnull >= 0) close(devnull);
  r.started = true;

  const int64_t deadline = mono_ms() + timeout_ms;
  char buf[4096];
  for (;;) {
    const int64_t left = deadline - mono_ms();
    if (left <= 0) {
      r.timed_out = true;
      break;
    }
    pollfd p{pipefd[0], POLLIN, 0};
    const int rc = poll(&p, 1, static_cast<int>(std::min<int64_t>(left, 1000)));
    if (rc < 0 && errno == EINTR) continue;
    if (rc <= 0) continue;
    const ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // EOF: the child (and anything it forked) closed its output
    // Keep draining past the cap so the child never blocks on a full pipe.
    const size_t room = max_output > r.output.size() ? max_output - r.output.size() : 0;
    if (static_cast<size_t>(n) > room) r.truncated = true;
    r.output.append(buf, std::min(static_cast<size_t>(n), room));
  }
  close(pipefd[0]);

  if (r.timed_out) kill(pid, SIGKILL);
  int status = 0;
  for (;;) {
    const pid_t w = waitpid(pid, &status, r.timed_out ? 0 : WNOHANG);
    if (w == pid) break;
    if (w < 0 && errno != EINTR) break;
    if (w == 0) {
      if (mono_ms() >= deadline) {
        r.timed_out = true;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        break;
      }
      sleep_ms(5);
    }
  }
  if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  if (r.exit_code == 127 && r.output.empty()) r.error = argv[0] + ": not found";
  return r;
}

Capture::Capture(std::string dir, std::string unit, std::string systemctl, std::string btmon)
    : dir_(std::move(dir)),
      unit_(std::move(unit)),
      systemctl_(std::move(systemctl)),
      btmon_(std::move(btmon)) {}

bool Capture::valid_name(const std::string& name) {
  static const std::string ext = ".btsnoop";
  if (name.size() <= ext.size() || name.size() > 255) return false;
  if (name[0] == '.') return false;
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
  if (name.find('\0') != std::string::npos) return false;
  return name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
}

bool Capture::exists(const std::string& name) const {
  if (!valid_name(name)) return false;
  struct stat st {};
  return stat(path(name).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<Capture::File> Capture::list() const {
  std::vector<File> out;
  DIR* d = opendir(dir_.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (!valid_name(name)) continue;
    struct stat st {};
    if (stat(path(name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    out.push_back(File{name, static_cast<long long>(st.st_size),
                       static_cast<long long>(st.st_mtim.tv_sec) * 1000 +
                           st.st_mtim.tv_nsec / 1000000});
  }
  closedir(d);
  // Newest first. The names carry the start time, which breaks mtime ties (a rotation writes the
  // last packets to both files within the same instant).
  std::sort(out.begin(), out.end(), [](const File& a, const File& b) {
    if (a.mtime_ms != b.mtime_ms) return a.mtime_ms > b.mtime_ms;
    return a.name > b.name;
  });
  return out;
}

std::string Capture::unit_state() const {
  const CommandResult r = run_command({systemctl_, "is-active", unit_}, 5000, 256);
  if (!r.started || r.exit_code == 127) return "unknown";
  std::string s = r.output;
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s.empty() ? "unknown" : s;
}

json Capture::status() const {
  const std::string state = unit_state();
  const bool running = state == "active" || state == "activating" || state == "reloading";
  const auto files = list();
  json arr = json::array();
  for (size_t i = 0; i < files.size(); ++i) {
    arr.push_back(json{{"name", files[i].name},
                       {"size", files[i].size},
                       {"mtime", files[i].mtime_ms},
                       {"active", running && i == 0}});
  }
  return json{{"running", running},
              {"state", state},
              {"unit", unit_},
              {"dir", dir_},
              {"files", std::move(arr)}};
}

bool Capture::is_active(const std::string& name) const {
  const auto files = list();
  if (files.empty() || files.front().name != name) return false;
  const std::string state = unit_state();
  return state == "active" || state == "activating" || state == "reloading";
}

bool Capture::set_running(bool on, std::string* err) const {
  const CommandResult r = run_command({systemctl_, on ? "start" : "stop", unit_}, 15000, 4096);
  if (r.started && !r.timed_out && r.exit_code == 0) return true;
  if (err) {
    if (!r.started) {
      *err = r.error;
    } else if (r.timed_out) {
      *err = "systemctl timed out";
    } else {
      *err = r.output.empty() ? r.error : r.output;
      while (!err->empty() && err->back() == '\n') err->pop_back();
      if (err->empty()) err->assign("systemctl exited with " + std::to_string(r.exit_code));
    }
  }
  return false;
}

CommandResult Capture::analyze(const std::string& name) const {
  // btmon -a over a full 16 MiB ring file takes a few seconds on the Zero W; 30 s is generous.
  return run_command({btmon_, "-a", path(name)}, 30000, 4 * 1024 * 1024);
}

}  // namespace btb::hci
