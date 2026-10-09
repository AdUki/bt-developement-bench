#pragma once

// btbench-logd: the journal, mirrored into one plain-text file per program under /tmp/logs, each
// line in the format the program itself prints when it logs to a terminal — so clog (app/clog)
// picks its colour group from the file name (bluez, pipewire, wireplumber, wpasupplicant) and its
// regexes match, and `clog -t /tmp/logs/bluez.log` reads like the program's own output. journald
// keeps everything as it does; these are a RAM copy (tmpfs), each capped and rotated once.
//
// Only what is needed to rebuild a native line is taken from an entry: the time, the priority,
// the program, its PID/TID, the message, and the source location and topic PipeWire and
// WirePlumber log with.

#include <cstdint>
#include <string>

namespace btb::logd {

struct Entry {
  uint64_t realtime_us = 0;  // __REALTIME_TIMESTAMP
  int priority = 6;          // syslog: 0 emerg .. 7 debug
  std::string transport;     // _TRANSPORT ("kernel", "journal", "stdout", "syslog")
  std::string unit;          // _SYSTEMD_UNIT
  std::string ident;         // SYSLOG_IDENTIFIER, else _COMM
  long pid = 0, tid = 0;
  std::string message;
  std::string code_file, code_func, topic;  // CODE_FILE, CODE_FUNC, TOPIC (PipeWire, WirePlumber)
  long code_line = 0;
};

enum class Format { bluez, pipewire, wireplumber, wpasupplicant, nsdk };

struct Route {
  std::string file;  // "bluez.log"
  Format format;
  bool prefix_ident;  // nsdk lines of a file several programs share: "ident: message"
};

// Which file an entry goes to: the kernel, bluetoothd, PipeWire, WirePlumber, wpa_supplicant,
// btbenchd each to their own, everything else to system.log.
Route route(const Entry& e);
// The entry as the program prints it (no newline). Times in local time.
std::string format(const Entry& e, const Route& r);

}  // namespace btb::logd
