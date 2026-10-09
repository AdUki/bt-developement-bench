#include "journal.h"

#include <string.h>
#include <systemd/sd-journal.h>

#include <cctype>
#include <chrono>

#include "util/log.h"

using json = nlohmann::json;

namespace btb {

namespace {

std::string field(sd_journal* j, const char* name) {
  const void* data = nullptr;
  size_t len = 0;
  if (sd_journal_get_data(j, name, &data, &len) < 0) return {};
  const char* s = static_cast<const char*>(data);
  const size_t n = strlen(name);
  // Data comes back as "NAME=value".
  if (len <= n + 1) return {};
  return std::string(s + n + 1, len - n - 1);
}

json entry(sd_journal* j) {
  uint64_t us = 0;
  sd_journal_get_realtime_usec(j, &us);
  const std::string prio = field(j, "PRIORITY");
  const std::string pid = field(j, "_PID");
  return json{{"ts", us / 1000},
              {"prio", prio.empty() ? 6 : std::atoi(prio.c_str())},
              {"unit", field(j, "_SYSTEMD_UNIT")},
              {"ident", field(j, "SYSLOG_IDENTIFIER")},
              {"pid", pid.empty() ? 0 : std::atoi(pid.c_str())},
              {"msg", field(j, "MESSAGE")}};
}

// The unit's own messages, and systemd's about it (Started, Stopped, Main process exited), which
// are what a developer restarting bluetoothd wants to see next to its output.
bool open_unit(const std::string& unit, sd_journal** out, std::string* err) {
  sd_journal* j = nullptr;
  int r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
  if (r < 0) {
    if (err) *err = std::string("cannot open the journal: ") + strerror(-r);
    return false;
  }
  sd_journal_add_match(j, ("_SYSTEMD_UNIT=" + unit).c_str(), 0);
  sd_journal_add_disjunction(j);
  sd_journal_add_match(j, ("UNIT=" + unit).c_str(), 0);
  sd_journal_add_match(j, "_PID=1", 0);
  *out = j;
  return true;
}

}  // namespace

bool journal_unit_name(const std::string& in, std::string* out) {
  if (in.empty() || in.size() > 128) return false;
  for (char c : in) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.' && c != '@' &&
        c != ':')
      return false;
  }
  *out = in.find('.') == std::string::npos ? in + ".service" : in;
  return true;
}

json journal_tail(const std::string& unit, int lines, std::string* err) {
  json out = json::array();
  sd_journal* j = nullptr;
  if (!open_unit(unit, &j, err)) return out;
  sd_journal_seek_tail(j);
  int back = 0;
  while (back < lines && sd_journal_previous(j) > 0) ++back;
  // Now at the oldest of them; walk forward.
  for (int i = 0; i < back; ++i) {
    out.push_back(entry(j));
    if (sd_journal_next(j) <= 0) break;
  }
  sd_journal_close(j);
  return out;
}

void JournalFollower::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void JournalFollower::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void JournalFollower::set_unit(const std::string& unit) {
  std::lock_guard<std::mutex> lk(m_);
  unit_ = unit;
}

std::string JournalFollower::unit() const {
  std::lock_guard<std::mutex> lk(m_);
  return unit_;
}

void JournalFollower::run() {
  sd_journal* j = nullptr;
  std::string open_for;
  std::string logged;
  while (running_.load()) {
    const std::string want = unit();
    if (!has_()) {
      if (j) {
        sd_journal_close(j);
        j = nullptr;
        open_for.clear();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    if (j && open_for != want) {
      sd_journal_close(j);
      j = nullptr;
    }
    if (!j) {
      std::string err;
      if (!open_unit(want, &j, &err)) {
        if (err != logged) LOG_WARN("journal: {}", err);
        logged = err;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
      // From now on: the history is GET /api/system/journal's.
      sd_journal_seek_tail(j);
      sd_journal_previous(j);
      open_for = want;
    }
    // Bounded, so a subscriber leaving or a unit change is noticed within half a second.
    sd_journal_wait(j, 500000);
    int n = 0;
    while (running_.load() && sd_journal_next(j) > 0) {
      pub_(entry(j));
      // A burst (bluetoothd -d at full tilt) is capped per pass; the rest follows on the next.
      if (++n >= 200) break;
    }
  }
  if (j) sd_journal_close(j);
}

}  // namespace btb
