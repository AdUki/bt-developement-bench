#include "audio/pwtop.h"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "util/exec.h"
#include "util/log.h"
#include "util/prio.h"

using json = nlohmann::json;

namespace btb::audio {

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// "12.3us", "1.2ms", "1.0s" → µs; "---" and "+++" (not measured) → -1.
double time_us(const std::string& t) {
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str()) return -1;
  const std::string unit = end;
  if (unit == "us") return v;
  if (unit == "ms") return v * 1000.0;
  if (unit == "s") return v * 1e6;
  return -1;
}

double share(const std::string& t) {
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  return end == t.c_str() || *end ? -1 : v;
}

json num_or_null(double v, double scale = 1.0) {
  return v < 0 ? json(nullptr) : json(std::round(v * scale * 1000.0) / 1000.0);
}

}  // namespace

bool parse_pwtop_line(const std::string& line, PwTopNode* out) {
  // "%s %4.1u %6.1u %6.1u %s %s %s %s  %3.1u %16.16s %s%s": nine fields, then the format in a
  // column of 16, a space, and the name (" + " in front of a follower's).
  std::istringstream in(line);
  std::string st, id, quant, rate, wait, busy, wq, bq, err;
  if (!(in >> st >> id >> quant >> rate >> wait >> busy >> wq >> bq >> err)) return false;
  if (st.size() != 1) return false;
  char* end = nullptr;
  const unsigned long idv = std::strtoul(id.c_str(), &end, 10);
  if (*end || id.empty()) return false;
  PwTopNode n;
  n.state = st[0];
  n.id = static_cast<uint32_t>(idv);
  n.quant = static_cast<uint32_t>(std::strtoul(quant.c_str(), nullptr, 10));
  n.rate = static_cast<uint32_t>(std::strtoul(rate.c_str(), nullptr, 10));
  n.wait_us = time_us(wait);
  n.busy_us = time_us(busy);
  n.wait_q = share(wq);
  n.busy_q = share(bq);
  n.err = static_cast<uint32_t>(std::strtoul(err.c_str(), nullptr, 10));
  // Where the ERR field ends in the line: the format column starts one space after it.
  const std::streamoff at = in.tellg();
  if (at < 0) return false;
  const size_t p = static_cast<size_t>(at) + 1;
  if (line.size() < p + 17) return false;
  std::string fmt = line.substr(p, 16);
  const size_t a = fmt.find_first_not_of(' ');
  n.format = a == std::string::npos ? "" : fmt.substr(a);
  std::string name = line.substr(p + 17);
  // " + " a follower, " = " (pw-top 1.6) an asynchronous one.
  if (name.rfind(" + ", 0) == 0 || name.rfind(" = ", 0) == 0) {
    n.follower = true;
    name.erase(0, 3);
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
  n.name = name;
  *out = std::move(n);
  return true;
}

json pwtop_json(const std::vector<PwTopNode>& rows, int64_t ts_ms, std::map<uint32_t, uint32_t>* prev) {
  json drivers = json::array();
  std::map<uint32_t, uint32_t> next;
  uint32_t xruns_total = 0;
  for (const PwTopNode& n : rows) {
    // xruns since the last table: pw-top counts from its own start, and a node that is new (or
    // was cleared) starts from what it shows now.
    const auto it = prev->find(n.id);
    const uint32_t xruns = it != prev->end() && n.err >= it->second ? n.err - it->second : 0;
    next[n.id] = n.err;
    xruns_total += xruns;
    const bool active = n.state == 'R' || n.state == 'I' || n.state == 'T' || n.state == 't';
    json j{{"id", n.id},
           {"state", std::string(1, n.state)},
           {"name", n.name},
           {"format", n.format.empty() ? json(nullptr) : json(n.format)},
           {"wait_ms", num_or_null(n.wait_us, 0.001)},
           {"busy_ms", num_or_null(n.busy_us, 0.001)},
           {"wait_q", num_or_null(n.wait_q)},
           {"busy_q", num_or_null(n.busy_q)},
           {"err", n.err},
           {"xruns", xruns}};
    if (!n.follower) {
      // A driver's QUANT/RATE are its quantum and rate: the period, and so the latency it adds.
      j["quantum"] = active && n.quant ? json(n.quant) : json(nullptr);
      j["rate"] = active && n.rate ? json(n.rate) : json(nullptr);
      j["period_ms"] = active && n.quant && n.rate ? json(std::round(n.quant * 1e6 / n.rate) / 1000.0) : json(nullptr);
      j["followers"] = json::array();
      drivers.push_back(std::move(j));
    } else if (!drivers.empty()) {
      // A follower's QUANT/RATE are the latency it reports, as a fraction of a second.
      j["latency_ms"] = active && n.quant && n.rate ? json(std::round(n.quant * 1e6 / n.rate) / 1000.0) : json(nullptr);
      drivers.back()["followers"].push_back(std::move(j));
    }
  }
  *prev = std::move(next);
  return json{{"ts", ts_ms}, {"drivers", std::move(drivers)}, {"xruns", xruns_total}};
}

void PwTop::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void PwTop::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

json PwTop::last() const {
  std::lock_guard<std::mutex> lk(m_);
  if (!last_at_ms_ || now_ms() - last_at_ms_ > 5000) {
    return json{{"running", false}, {"error", error_.empty() ? json(nullptr) : json(error_)}};
  }
  json j = last_;
  j["running"] = true;
  return j;
}

void PwTop::run() {
  set_thread_nice(kBackgroundNice);
  std::string logged;
  while (running_.load()) {
    if (!has_()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    std::string err;
    if (!run_once(&err)) {
      if (err != logged) LOG_WARN("pw-top: {}", err);
      logged = err;
      {
        std::lock_guard<std::mutex> lk(m_);
        error_ = err;
      }
      // Not there (a PC without pipewire-tools), or PipeWire is down: look again later.
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }
}

bool PwTop::run_once(std::string* err) {
  // pw-top's stdout into a pipe is block-buffered: a table would arrive in 4 KiB pieces, seconds
  // late. stdbuf (coreutils) makes it line-buffered when it is there.
  static const bool have_stdbuf = !find_exec("stdbuf").empty();
  std::vector<std::string> argv{"pw-top", "-b"};
  if (have_stdbuf) argv.insert(argv.begin(), {"stdbuf", "-oL"});
  Spawned sp;
  if (!spawn(argv, &sp, err)) return false;
  std::map<uint32_t, uint32_t> prev;
  std::vector<PwTopNode> rows;
  std::string buf;
  int tables = 0;
  bool header = false;
  auto flush = [&]() {
    if (!header) return;
    header = false;
    json t = pwtop_json(rows, now_ms(), &prev);
    rows.clear();
    // The first table's xruns are pw-top's whole history: not "now".
    if (tables++ == 0) return;
    {
      std::lock_guard<std::mutex> lk(m_);
      last_ = t;
      last_at_ms_ = now_ms();
      error_.clear();
    }
    pub_(t);
  };
  while (running_.load() && has_()) {
    pollfd p[2] = {{sp.out_fd, POLLIN, 0}, {sp.err_fd, POLLIN, 0}};
    // pw-top writes a table at once, once a second: a pause of 200 ms means it is complete.
    const int rc = poll(p, 2, 200);
    if (rc < 0 && errno != EINTR) break;
    if (rc == 0) {
      flush();
      continue;
    }
    if (p[1].revents & POLLIN) {
      char tmp[512];
      const ssize_t n = read(sp.err_fd, tmp, sizeof tmp);
      if (n > 0) *err = std::string(tmp, static_cast<size_t>(n));
    }
    if (p[0].revents & (POLLIN | POLLHUP)) {
      char tmp[4096];
      const ssize_t n = read(sp.out_fd, tmp, sizeof tmp);
      if (n <= 0) break;  // pw-top ended
      buf.append(tmp, static_cast<size_t>(n));
      for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
        const std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (line.rfind("S   ID", 0) == 0) {
          flush();
          header = true;
          continue;
        }
        PwTopNode node;
        if (header && parse_pwtop_line(line, &node)) rows.push_back(std::move(node));
      }
    }
  }
  kill_group(sp.pid, 500);
  int status = 0;
  waitpid(sp.pid, &status, 0);
  for (int fd : {sp.out_fd, sp.err_fd}) close(fd);
  // Ended by itself before giving any table: it could not connect, or is missing.
  if (tables == 0 && running_.load() && has_()) {
    while (!err->empty() && (err->back() == '\n' || err->back() == ' ')) err->pop_back();
    if (err->empty()) *err = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? "pw-top not found" : "pw-top ended";
    return false;
  }
  return true;
}

}  // namespace btb::audio
