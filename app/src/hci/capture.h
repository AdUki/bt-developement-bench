#pragma once

// The btmon capture ring in /tmp/btsnoop (RAM), written by btbench-btsnoop.service (not by us): list,
// download, delete, start/stop the service, and run `btmon -a` over a file.

#include <cstddef>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb::hci {

// btmon -a's output without the lines its missing plotter adds ("sh: gnuplot: command not found").
std::string drop_plot_errors(const std::string& out);


struct CommandResult {
  bool started = false;  // false: fork/exec failed (error says why)
  bool timed_out = false;
  int exit_code = -1;  // 127: the program was not found
  bool truncated = false;
  std::string output;  // stdout and stderr, interleaved, capped
  std::string error;
};

// fork/exec with a timeout (SIGKILL when it expires) and an output cap; no shell. The child gets
// /dev/null as stdin and none of our descriptors (sockets, the HTTP listener).
CommandResult run_command(const std::vector<std::string>& argv, int timeout_ms,
                          size_t max_output);

class Capture {
 public:
  Capture(std::string dir, std::string unit, std::string systemctl, std::string btmon);

  // {"running","state","unit","dir","files":[{"name","size","mtime","active"}]} — newest first.
  nlohmann::json status() const;
  bool set_running(bool on, std::string* err) const;

  // A bare file name in the capture dir: no path separators, no leading dot, ends ".btsnoop".
  static bool valid_name(const std::string& name);
  std::string path(const std::string& name) const { return dir_ + "/" + name; }
  bool exists(const std::string& name) const;
  // The file being written right now: the newest one while the service runs, or the one the
  // monitor itself writes (a replay). Asks systemctl, so not for every request.
  bool is_active(const std::string& name) const;
  // The newest file in the directory (the one btmon writes, while it runs), or "" without any.
  // A directory listing only: cheap enough for a request's answer.
  std::string newest() const;

  // The monitor records what it replays into the directory as a ring file of its own, so the
  // packet view has a live end to follow; this names that file ("" once the replay ended).
  void set_writing(const std::string& name);
  std::string writing() const;

  CommandResult analyze(const std::string& name) const;

  const std::string& dir() const { return dir_; }
  const std::string& unit() const { return unit_; }

 private:
  struct File {
    std::string name;
    long long size;
    long long mtime_ms;
  };
  std::vector<File> list() const;  // newest first
  std::string unit_state() const;  // `systemctl is-active` output: active, inactive, failed, ...

  std::string dir_;
  std::string unit_;
  std::string systemctl_;
  std::string btmon_;
  mutable std::mutex mu_;
  std::string writing_;
};

}  // namespace btb::hci
