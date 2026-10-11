#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include <systemd/sd-bus.h>

namespace btb {

// One logged message as a line of the log file: time, kind, who to whom, then what — the call
// with its arguments, a reply with the call it answers and how long that took, a signal, an error.
// `e` is a DbusLog entry (see DbusLog::entries).
std::string dbus_log_line(const nlohmann::json& e);

// A message's arguments as compact JSON text of at most `max` bytes, cut with "…" when longer:
// a property dump can be tens of kilobytes, and the log is for reading. *cut says it was cut.
std::string dbus_args_text(const nlohmann::json& args, size_t max, bool* cut);

// BlueZ's D-Bus traffic: every method call, reply, error and signal to or from org.bluez and
// org.bluez.obex on the system bus, as busctl monitor would show it, kept as a ring for the
// console (GET /api/bluetooth/dbus, the "bluez.dbus" topic) and written to a log file beside
// btbench-logd's (bluez-dbus.log, rotated once).
//
// It watches from inside btbenchd: a second bus connection that becomes a monitor
// (org.freedesktop.DBus.Monitoring.BecomeMonitor, root only) and decodes each message with the
// same reader as everything else (read_json), so no busctl process and no text parsing on the
// Zero W. It runs whether or not anyone watches — a log has to hold the minutes before an issue —
// but stays cheap: BlueZ is quiet when nothing happens, and the biggest messages, the object tree
// btbenchd re-reads every few seconds, are counted (N objects) rather than decoded.
class DbusLog {
 public:
  struct Options {
    std::string file;                   // "" writes no file
    size_t max_file_bytes = 2 << 20;    // then file → file.1
    size_t ring = 2000;                 // entries kept for the console
    size_t max_args = 1500;             // bytes of argument text kept per message
  };
  using HasSubscribers = std::function<bool()>;
  using Publish = std::function<void(const nlohmann::json& batch)>;

  DbusLog(Options opt, HasSubscribers has, Publish pub);
  ~DbusLog();
  DbusLog(const DbusLog&) = delete;
  DbusLog& operator=(const DbusLog&) = delete;

  void start();
  void stop();

  // {running, error, seq (the newest entry's), file, entries: [...]} — the entries after `since`,
  // oldest first, at most `limit` (the newest of them). An entry: {seq, ts (ms since the epoch),
  // type: call|return|error|signal, sender, sender_name, dest, dest_name, path, iface, member,
  // sig, args (JSON, or a string when cut), cut, call (what a reply answers), latency_ms,
  // error_name, error}.
  nlohmann::json entries(uint64_t since, size_t limit) const;

 private:
  void run();
  bool open(std::string* err);
  void close_bus();
  void handle(sd_bus_message* m);
  std::string label(const std::string& name);
  void write_line(const std::string& line);
  void flush(bool force);

  Options opt_;
  HasSubscribers has_;
  Publish pub_;

  mutable std::mutex m_;
  std::deque<nlohmann::json> ring_;
  uint64_t seq_ = 0;
  std::string error_;
  bool monitoring_ = false;

  std::thread thread_;
  std::atomic<bool> running_{false};

  // The run thread's own.
  sd_bus* bus_ = nullptr;    // the monitor
  sd_bus* names_ = nullptr;  // an ordinary connection, for who owns a name and which program it is
  std::map<std::string, std::string> labels_;
  struct Call {
    uint64_t at_us;
    std::string member;
  };
  std::map<std::string, Call> calls_;  // "sender/cookie" → the call, until its reply
  std::deque<std::string> call_order_;
  nlohmann::json batch_ = nlohmann::json::array();
  uint64_t last_flush_us_ = 0;
  FILE* file_ = nullptr;
  size_t file_bytes_ = 0;
};

}  // namespace btb
