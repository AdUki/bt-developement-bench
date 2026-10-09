#pragma once

#include <sys/types.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace btb {

// The tools the jobs runner starts: BlueZ's test tools, the PipeWire/BlueALSA inspectors, iperf3.
// Nothing that takes a shell, and the arguments go to execv as they are.
bool job_allowed(const std::string& cmd);
const std::vector<std::string>& job_allowlist();

// Runs allowlisted tools in the background and streams what they print on the WebSocket topic
// "job.<id>": {"stream":"out|err","line":"..."} per line, then {"exit":N,"killed":bool} (128+N
// for a signal). The output is also kept (the last 64 KiB) for GET /api/jobs/<id>, so a
// console that opened late, or `bench`, can read it.
class Jobs {
 public:
  using Publish = std::function<void(const std::string& topic, const nlohmann::json& data)>;
  explicit Jobs(Publish pub);
  ~Jobs();

  static constexpr size_t kMaxRunning = 4;
  static constexpr size_t kKeepFinished = 20;
  static constexpr size_t kMaxOutput = 64 * 1024;

  // {id, cmd, args, running, exit, started_ms}
  bool start(const std::string& cmd, const std::vector<std::string>& args, unsigned timeout_s,
             nlohmann::json* out, std::string* err);
  nlohmann::json list() const;
  // Includes "output": [{"stream","line"}...]; null when there is no such job.
  nlohmann::json get(int id) const;
  bool kill(int id, std::string* err);
  void stop_all();

 private:
  struct Job;
  void run(Job* j);
  nlohmann::json brief(const Job& j) const;

  Publish pub_;
  mutable std::mutex m_;
  std::map<int, std::unique_ptr<Job>> jobs_;
  int next_id_ = 1;
};

}  // namespace btb
