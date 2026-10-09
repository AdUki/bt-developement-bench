#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace btb {

struct ExecResult {
  bool started = false;    // false: not found, or fork/exec failed (see `out`/`err` for why)
  bool timed_out = false;  // killed at the deadline
  int status = -1;         // the exit code; 128+N for a signal
  std::string out;
  std::string err;
  bool ok() const { return started && !timed_out && status == 0; }
  // The one line to show for a failure: the command's own last stderr line when it wrote one.
  std::string reason() const;
};

// Runs argv[0] (looked up in PATH) with stdin on /dev/null, collects stdout and stderr (each capped
// at `max_bytes`, the rest discarded), and kills its whole process group at `timeout_ms`. Every
// caller holds an HTTP worker or a module thread while this runs, so the timeout is never
// optional: a script stuck on a dead Wi-Fi driver must not take a thread with it for good.
ExecResult run_cmd(const std::vector<std::string>& argv, int timeout_ms, size_t max_bytes = 1 << 20);

// The full path of an executable: `name` itself when it has a slash, else the first match in
// `dirs` (colon-separated) and then $PATH. "" when there is none.
std::string find_exec(const std::string& name, const std::string& dirs = "");

// A child with its stdout and stderr on pipes, in a process group of its own so that a kill
// reaches whatever it started. For the jobs runner, which streams the output as it comes.
struct Spawned {
  pid_t pid = -1;
  int out_fd = -1;
  int err_fd = -1;
};
bool spawn(const std::vector<std::string>& argv, Spawned* out, std::string* err);
// SIGTERM to the group, then SIGKILL after `grace_ms` if it is still there. Does not reap.
void kill_group(pid_t pid, int grace_ms);

}  // namespace btb
