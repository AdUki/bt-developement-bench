#include "jobs.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include "util/exec.h"
#include "util/log.h"

using json = nlohmann::json;

namespace btb {

namespace {

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

const std::vector<std::string>& job_allowlist() {
  static const std::vector<std::string> a = {
      "l2test",   "l2ping",   "isotest",  "rctest",      "scotest",     "btgatt-client",
      "btmgmt",   "bluetoothctl", "avinfo", "iperf3",    "wpctl",       "pw-dump",
      "pw-cli",   "bluealsactl", "bluealsa-cli", "hcitool", "hciconfig"};
  return a;
}

bool job_allowed(const std::string& cmd) {
  const auto& a = job_allowlist();
  return std::find(a.begin(), a.end(), cmd) != a.end();
}

struct Jobs::Job {
  int id = 0;
  std::string cmd;
  std::vector<std::string> args;
  unsigned timeout_s = 0;
  int64_t started_ms = 0;
  int64_t finished_ms = 0;
  // Under Jobs::m_.
  bool running = true;
  bool killed = false;
  int exit = -1;
  std::deque<std::pair<std::string, std::string>> lines;  // (stream, line)
  size_t bytes = 0;
  Spawned proc;
  std::thread thread;
};

Jobs::Jobs(Publish pub) : pub_(std::move(pub)) {}
Jobs::~Jobs() { stop_all(); }

json Jobs::brief(const Job& j) const {
  return json{{"id", j.id},
              {"cmd", j.cmd},
              {"args", j.args},
              {"running", j.running},
              {"exit", j.running || j.exit < 0 ? json(nullptr) : json(j.exit)},
              {"killed", j.killed},
              {"started_ms", j.started_ms},
              {"finished_ms", j.finished_ms ? json(j.finished_ms) : json(nullptr)},
              {"topic", "job." + std::to_string(j.id)}};
}

bool Jobs::start(const std::string& cmd, const std::vector<std::string>& args, unsigned timeout_s,
                 json* out, std::string* err) {
  if (!job_allowed(cmd)) {
    *err = cmd + " is not one of the tools the runner starts";
    return false;
  }
  for (const std::string& a : args) {
    if (a.size() > 1024 || a.find('\0') != std::string::npos) {
      *err = "an argument is too long";
      return false;
    }
  }
  std::lock_guard<std::mutex> lk(m_);
  size_t running = 0;
  for (const auto& kv : jobs_) running += kv.second->running;
  if (running >= kMaxRunning) {
    *err = "already " + std::to_string(running) + " jobs running; stop one first";
    return false;
  }
  auto j = std::make_unique<Job>();
  j->cmd = cmd;
  j->args = args;
  j->timeout_s = timeout_s;
  std::vector<std::string> argv{cmd};
  argv.insert(argv.end(), args.begin(), args.end());
  std::string why;
  if (!spawn(argv, &j->proc, &why)) {
    *err = why;
    return false;
  }
  j->id = next_id_++;
  j->started_ms = wall_ms();
  LOG_INFO("jobs: {} started: {}", j->id, cmd);
  Job* raw = j.get();
  *out = brief(*raw);
  jobs_[raw->id] = std::move(j);
  raw->thread = std::thread([this, raw] { run(raw); });

  // Forget the oldest finished ones beyond what is kept. Their threads have ended (finished means
  // run() returned), so the join is immediate.
  std::vector<int> finished;
  for (const auto& kv : jobs_)
    if (!kv.second->running) finished.push_back(kv.first);
  while (finished.size() > kKeepFinished) {
    auto it = jobs_.find(finished.front());
    if (it->second->thread.joinable()) it->second->thread.join();
    jobs_.erase(it);
    finished.erase(finished.begin());
  }
  return true;
}

void Jobs::run(Job* j) {
  const std::string topic = "job." + std::to_string(j->id);
  std::string partial[2];
  bool open[2] = {true, true};
  const int fds[2] = {j->proc.out_fd, j->proc.err_fd};
  const char* names[2] = {"out", "err"};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(j->timeout_s);
  char buf[4096];

  auto emit = [&](int s, const std::string& line) {
    {
      std::lock_guard<std::mutex> lk(m_);
      j->lines.emplace_back(names[s], line);
      j->bytes += line.size() + 1;
      while (j->bytes > kMaxOutput && !j->lines.empty()) {
        j->bytes -= j->lines.front().second.size() + 1;
        j->lines.pop_front();
      }
    }
    pub_(topic, json{{"stream", names[s]}, {"line", line}});
  };

  while (open[0] || open[1]) {
    if (j->timeout_s && std::chrono::steady_clock::now() >= deadline) {
      {
        std::lock_guard<std::mutex> lk(m_);
        j->killed = true;
      }
      kill_group(j->proc.pid, 1000);
      break;
    }
    pollfd p[2] = {{open[0] ? fds[0] : -1, POLLIN, 0}, {open[1] ? fds[1] : -1, POLLIN, 0}};
    if (poll(p, 2, 500) < 0 && errno != EINTR) break;
    for (int s = 0; s < 2; ++s) {
      if (!(p[s].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const ssize_t n = read(fds[s], buf, sizeof(buf));
      if (n <= 0) {
        open[s] = false;
        if (!partial[s].empty()) emit(s, partial[s]);
        partial[s].clear();
        continue;
      }
      partial[s].append(buf, static_cast<size_t>(n));
      size_t nl;
      while ((nl = partial[s].find('\n')) != std::string::npos) {
        std::string line = partial[s].substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        emit(s, line);
        partial[s].erase(0, nl + 1);
      }
      // A tool that prints progress without newlines (l2test's counters) is flushed in pieces.
      if (partial[s].size() > 2048) {
        emit(s, partial[s]);
        partial[s].clear();
      }
    }
  }
  close(fds[0]);
  close(fds[1]);
  int status = 0;
  waitpid(j->proc.pid, &status, 0);
  json done;
  {
    std::lock_guard<std::mutex> lk(m_);
    j->running = false;
    j->finished_ms = wall_ms();
    if (WIFEXITED(status)) j->exit = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) j->exit = 128 + WTERMSIG(status);
    done = json{{"exit", j->exit}, {"killed", j->killed}};
  }
  LOG_INFO("jobs: {} ({}) ended: {}", j->id, j->cmd, j->exit);
  pub_(topic, done);
}

json Jobs::list() const {
  json a = json::array();
  std::lock_guard<std::mutex> lk(m_);
  for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it) a.push_back(brief(*it->second));
  return a;
}

json Jobs::get(int id) const {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = jobs_.find(id);
  if (it == jobs_.end()) return nullptr;
  json j = brief(*it->second);
  json out = json::array();
  for (const auto& l : it->second->lines) out.push_back(json{{"stream", l.first}, {"line", l.second}});
  j["output"] = out;
  return j;
}

bool Jobs::kill(int id, std::string* err) {
  pid_t pid = -1;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      *err = "no such job";
      return false;
    }
    if (!it->second->running) return true;
    it->second->killed = true;
    pid = it->second->proc.pid;
  }
  // Not waited for here: the job's own thread reaps it and publishes the exit.
  ::kill(-pid, SIGTERM);
  std::thread([pid] { kill_group(pid, 1500); }).detach();
  return true;
}

void Jobs::stop_all() {
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& kv : jobs_) {
      if (kv.second->running) ::kill(-kv.second->proc.pid, SIGKILL);
      if (kv.second->thread.joinable()) threads.push_back(std::move(kv.second->thread));
    }
  }
  for (std::thread& t : threads) t.join();
}

}  // namespace btb
