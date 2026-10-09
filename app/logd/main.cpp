// btbench-logd (logd.h): follows the journal and writes /tmp/logs/<program>.log.
#include <CLI11.hpp>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <systemd/sd-journal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "logd.h"

namespace {

volatile sig_atomic_t g_stop = 0;

std::string field(sd_journal* j, const char* name) {
  const void* d = nullptr;
  size_t n = 0;
  if (sd_journal_get_data(j, name, &d, &n) < 0) return {};
  const size_t k = std::strlen(name) + 1;  // "NAME="
  return n > k ? std::string(static_cast<const char*>(d) + k, n - k) : std::string{};
}

long num(const std::string& s) { return s.empty() ? 0 : std::strtol(s.c_str(), nullptr, 10); }

// One output file: appended to, and when it passes the cap renamed to <name>.1 (the one before is
// dropped) and started again — the newest lines are what a person reading /tmp/logs wants.
struct Out {
  std::string path;
  FILE* f = nullptr;
  long size = 0;
};

class Writer {
 public:
  Writer(std::string dir, long cap) : dir_(std::move(dir)), cap_(cap) {}
  ~Writer() {
    for (auto& [n, o] : outs_)
      if (o.f) std::fclose(o.f);
  }

  void line(const std::string& name, const std::string& text) {
    Out& o = outs_[name];
    if (!o.f) open(name, &o, true);
    if (!o.f) return;
    std::fwrite(text.data(), 1, text.size(), o.f);
    std::fputc('\n', o.f);
    o.size += static_cast<long>(text.size()) + 1;
    if (o.size > cap_) {
      std::fclose(o.f);
      std::rename(o.path.c_str(), (o.path + ".1").c_str());
      open(name, &o, true);
    }
  }

  // Written out whenever the journal goes quiet: lines reach the files within a moment, and a
  // burst (BT_DBG on a streaming link) costs one write per buffer, not one per line.
  void flush() {
    for (auto& [n, o] : outs_)
      if (o.f) std::fflush(o.f);
  }

 private:
  void open(const std::string& name, Out* o, bool truncate) {
    o->path = dir_ + "/" + name;
    o->f = std::fopen(o->path.c_str(), truncate ? "w" : "a");
    o->size = 0;
    if (!o->f) std::fprintf(stderr, "btbench-logd: cannot write %s: %s\n", o->path.c_str(), std::strerror(errno));
    else setvbuf(o->f, nullptr, _IOFBF, 64 * 1024);
  }

  const std::string dir_;
  const long cap_;
  std::map<std::string, Out> outs_;
};

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"btbench-logd: the journal as one native-format text file per program (for clog)"};
  std::string dir = "/tmp/logs";
  long cap_kib = 2048;
  unsigned backlog = 3000;
  std::vector<std::string> links;
  app.add_option("--dir", dir, "Where the files go (default /tmp/logs)");
  app.add_option("--max-kib", cap_kib, "A file's size before it is rotated to <name>.1 (default 2048)");
  app.add_option("--backlog", backlog, "Journal entries written at start, before following (default 3000)");
  app.add_option("--link", links, "NAME=TARGET: a symbolic link in the directory (btsnoop=/data/btsnoop)");
  CLI11_PARSE(app, argc, argv);

  mkdir(dir.c_str(), 0755);
  for (const std::string& l : links) {
    const auto eq = l.find('=');
    if (eq == std::string::npos) continue;
    const std::string at = dir + "/" + l.substr(0, eq);
    unlink(at.c_str());
    if (symlink(l.substr(eq + 1).c_str(), at.c_str()) < 0)
      std::fprintf(stderr, "btbench-logd: cannot link %s: %s\n", at.c_str(), std::strerror(errno));
  }

  signal(SIGTERM, [](int) { g_stop = 1; });
  signal(SIGINT, [](int) { g_stop = 1; });

  sd_journal* j = nullptr;
  int r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
  if (r < 0) {
    std::fprintf(stderr, "btbench-logd: cannot open the journal: %s\n", std::strerror(-r));
    return 1;
  }
  // The files are started over: the backlog fills them with what led up to now.
  sd_journal_seek_tail(j);
  sd_journal_previous_skip(j, backlog + 1);

  Writer w(dir, cap_kib * 1024);
  while (!g_stop) {
    r = sd_journal_next(j);
    if (r < 0) {
      std::fprintf(stderr, "btbench-logd: reading the journal: %s\n", std::strerror(-r));
      break;
    }
    if (r == 0) {
      w.flush();
      // Wakes on new entries (and once in a while to see the stop flag).
      sd_journal_wait(j, 2000000);
      continue;
    }
    btb::logd::Entry e;
    uint64_t t = 0;
    sd_journal_get_realtime_usec(j, &t);
    e.realtime_us = t;
    e.priority = static_cast<int>(num(field(j, "PRIORITY")));
    if (field(j, "PRIORITY").empty()) e.priority = 6;
    e.transport = field(j, "_TRANSPORT");
    e.unit = field(j, "_SYSTEMD_UNIT");
    e.ident = field(j, "SYSLOG_IDENTIFIER");
    if (e.ident.empty()) e.ident = field(j, "_COMM");
    e.pid = num(field(j, "_PID"));
    e.tid = num(field(j, "TID"));
    e.message = field(j, "MESSAGE");
    e.code_file = field(j, "CODE_FILE");
    e.code_line = num(field(j, "CODE_LINE"));
    e.code_func = field(j, "CODE_FUNC");
    e.topic = field(j, "TOPIC");
    // A message with newlines is one line per line, each formatted alike.
    const btb::logd::Route route = btb::logd::route(e);
    size_t at = 0;
    const std::string whole = e.message;
    do {
      const size_t nl = whole.find('\n', at);
      e.message = whole.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      w.line(route.file, btb::logd::format(e, route));
      at = nl == std::string::npos ? whole.size() : nl + 1;
    } while (at < whole.size());
  }
  w.flush();
  sd_journal_close(j);
  return 0;
}
