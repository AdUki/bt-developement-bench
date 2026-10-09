#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace btb {

// "bluetooth" → "bluetooth.service"; a name with a suffix of its own is left alone. False for
// anything that is not a plausible unit name (it ends up in a journal match).
bool journal_unit_name(const std::string& in, std::string* out);

// The last `lines` entries of a unit: [{ts (ms since the epoch), prio, unit, ident, pid, msg}],
// oldest first. sd-journal, not journalctl: no process per request on the Zero W.
nlohmann::json journal_tail(const std::string& unit, int lines, std::string* err);

// Follows one unit's journal and publishes each new entry on the "journal" topic, but only while
// someone is subscribed: with nobody watching the thread sleeps and the journal stays closed.
class JournalFollower {
 public:
  using HasSubscribers = std::function<bool()>;
  using Publish = std::function<void(const nlohmann::json&)>;
  JournalFollower(HasSubscribers has, Publish pub) : has_(std::move(has)), pub_(std::move(pub)) {}
  ~JournalFollower() { stop(); }

  void start();
  void stop();
  void set_unit(const std::string& unit);
  std::string unit() const;

 private:
  void run();

  HasSubscribers has_;
  Publish pub_;
  mutable std::mutex m_;
  std::string unit_ = "bluetooth.service";
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace btb
