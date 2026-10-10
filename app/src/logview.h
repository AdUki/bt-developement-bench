#pragma once

// The log viewer's reading side (GET /api/system/logs/view, docs/api.md "Logs"): the journal and
// btbench-logd's files in /tmp/logs, both as the same entries, read backwards from the end (or
// from where the viewer stopped), filtered on the board so a search covers the whole log and not
// only what a page holds, and written as JSON by hand (thousands of small objects through
// nlohmann::json cost the Zero W more than reading them).
//
// The journal is read from its active files first: opening and interleaving the archived ones is
// most of the cost (2.9 s for 1000 lines on a Zero W, against 0.2 s for the active file alone), and
// the archives are only opened when the active files run out before the request is answered.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace btb {

struct LogEntry {
  int64_t ts_ms = 0;  // 0: the line carries no time (a file line in an unknown format)
  int prio = 6;       // syslog: 0 emerg .. 7 debug
  std::string who;    // the program
  int pid = 0;
  std::string tid;    // "778.781" (nsdk's [pid.tid]), when the format has it
  std::string topic;  // PipeWire/WirePlumber's topic ("spa.alsa", "m-lua-scripting")
  std::string msg;
};

// The viewer's filter, as the page writes it: words that must all appear (case-insensitive),
// !word that must not, /regex/ (its own flags; case-insensitive unless it has a capital), "a
// phrase", and the lowest priority shown.
class LogFilter {
 public:
  // false and *err for a regex that does not compile.
  bool parse(const std::string& src, int max_prio, std::string* err);
  bool empty() const { return terms_.empty() && max_prio_ >= 7; }
  bool match(const LogEntry& e) const;

 private:
  struct Term {
    bool neg = false;
    std::string word;          // lower case; empty for a regex
    std::shared_ptr<void> re;  // a compiled POSIX regex_t (glibc's: many times std::regex's speed)
  };
  std::vector<Term> terms_;
  int max_prio_ = 7;
};

// One line of a btbench-logd file, in the format of the program it is from (app/logd/format.cpp):
// bluez, PipeWire, WirePlumber, wpa_supplicant, and the nsdk format of the rest (system.log has
// "ident: " in front of the message). A line in none of them is a message of priority 6.
LogEntry parse_log_line(const std::string& line);

// JSON for the page: [{"ts", "prio", "who", "pid", "tid", "topic", "msg"}, ...] with empty fields
// left out, appended to *out.
void append_entries_json(const std::vector<LogEntry>& entries, std::string* out);
void append_json_string(const std::string& s, std::string* out);
// gzip at level 1 (fast); false if zlib fails.
bool gzip_fast(const std::string& in, std::string* out);

struct LogQuery {
  int lines = 1000;        // at most this many entries
  std::string before;      // journal: a cursor; file: a byte offset. Entries older than it.
  long long after = -1;    // file: entries after this byte offset (following), oldest first
  LogFilter filter;
  int budget_ms = 1500;    // the journal: stop after this long (a rare match in a long journal)
};

struct LogPage {
  std::vector<LogEntry> entries;  // oldest first
  std::string before;    // where to ask for older ones from; empty when there is nothing older
  long long end = -1;    // file: the byte offset to follow from
  bool reset = false;    // file: rotated since `after`; these entries start afresh
  long long scanned = 0;  // entries (journal) or lines (file) looked at
  bool limited = false;  // stopped by the time budget before `lines` matched: ask again from `before`
};

// A file in `dir` (a bare name). false and *err if it cannot be read.
bool read_log_file(const std::string& path, const LogQuery& q, LogPage* out, std::string* err);
// A unit's journal ("kernel": the kernel's). false and *err if the journal cannot be opened.
bool read_journal(const std::string& unit, const LogQuery& q, LogPage* out, std::string* err);

}  // namespace btb
