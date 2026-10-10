#include "logview.h"

#include <dirent.h>
#include <fcntl.h>
#include <regex.h>
#include <sys/stat.h>
#include <systemd/sd-journal.h>
#include <time.h>
#include <zlib.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace btb {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool digits(const char* p, int n) {
  for (int i = 0; i < n; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(p[i]))) return false;
  }
  return true;
}

int num(const char* p, int n) {
  int v = 0;
  for (int i = 0; i < n; ++i) v = v * 10 + (p[i] - '0');
  return v;
}

// Local time, as btbench-logd writes it, to ms since the epoch. mktime() (a time zone lookup) is
// most of a line's cost on the Zero W, and every line of a day has the same midnight: that is
// worked out once a day. (A DST change within the day shifts that day's later lines by an hour.)
int64_t local_ms(int y, int mo, int d, int h, int mi, int s, int ms) {
  thread_local int cached_day = -1;
  thread_local int64_t midnight_ms = 0;
  const int day = (y * 100 + mo) * 100 + d;
  if (day != cached_day) {
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_isdst = -1;
    midnight_ms = static_cast<int64_t>(mktime(&t)) * 1000;
    cached_day = day;
  }
  return midnight_ms + ((h * 60 + mi) * 60 + s) * 1000LL + ms;
}

// "20220309 12:06:48.280" at p (21 characters).
bool stamp(const char* p, size_t n, int64_t* ts) {
  if (n < 21 || !digits(p, 8) || p[8] != ' ' || !digits(p + 9, 2) || p[11] != ':' || !digits(p + 12, 2) ||
      p[14] != ':' || !digits(p + 15, 2) || p[17] != '.' || !digits(p + 18, 3))
    return false;
  *ts = local_ms(num(p, 4), num(p + 4, 2), num(p + 6, 2), num(p + 9, 2), num(p + 12, 2), num(p + 15, 2),
                 num(p + 18, 3));
  return true;
}

// "10:44:56.111404" (PipeWire, WirePlumber): today, in local time; a time later than now by more
// than a minute is yesterday's (a file read just after midnight).
bool clock_us(const char* p, size_t n, int64_t* ts) {
  if (n < 15 || !digits(p, 2) || p[2] != ':' || !digits(p + 3, 2) || p[5] != ':' || !digits(p + 6, 2) ||
      p[8] != '.' || !digits(p + 9, 6))
    return false;
  thread_local time_t at = 0;
  thread_local struct tm t {};
  const time_t now = time(nullptr);
  if (now != at) {
    localtime_r(&now, &t);
    at = now;
  }
  int64_t v = local_ms(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, num(p, 2), num(p + 3, 2), num(p + 6, 2),
                       num(p + 9, 3));
  if (v > static_cast<int64_t>(now) * 1000 + 60000) v -= 86400000;
  *ts = v;
  return true;
}

int level_prio(const std::string& l) {
  if (l == "DEBUG") return 7;
  if (l == "INFO") return 6;
  if (l == "NOTICE") return 5;
  if (l == "WARN" || l == "WARNING") return 4;
  if (l == "ERROR") return 3;
  if (l == "FATAL" || l == "CRIT") return 2;
  return -1;
}

// The word at s[at..] up to a space; *next is after the space.
std::string word(const std::string& s, size_t at, size_t* next) {
  const size_t sp = s.find(' ', at);
  *next = sp == std::string::npos ? s.size() : sp + 1;
  return s.substr(at, (sp == std::string::npos ? s.size() : sp) - at);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Lines of btbench-logd's files

LogEntry parse_log_line(const std::string& l) {
  LogEntry e;
  const char* p = l.c_str();
  const size_t n = l.size();
  size_t at = 0;

  // bluez: "bluetoothd[13077]: 20220309 12:06:48.280 DEBUG src/adapter.c:... message"
  if (l.rfind("bluetoothd[", 0) == 0) {
    const size_t close = l.find("]: ");
    if (close != std::string::npos && stamp(p + close + 3, n - close - 3, &e.ts_ms)) {
      e.who = "bluetoothd";
      e.pid = std::atoi(p + 11);
      const std::string lvl = word(l, close + 3 + 22, &at);
      const int pr = level_prio(lvl);
      if (pr >= 0) {
        e.prio = pr;
        e.msg = l.substr(at);
        return e;
      }
    }
  }
  // PipeWire: "[I][10:44:56.111404] pw.conf      | [          conf.c:  423 conf_load()] message"
  if (n > 21 && p[0] == '[' && p[2] == ']' && p[3] == '[' && clock_us(p + 4, n - 4, &e.ts_ms) && p[19] == ']') {
    static const char lv[] = "TDIWE";
    static const int pr[] = {7, 7, 6, 4, 3};
    const char* f = std::strchr(lv, p[1]);
    e.prio = f && *f ? pr[f - lv] : 6;
    e.who = "pipewire";
    const size_t bar = l.find(" | ", 21);
    if (bar != std::string::npos) {
      e.topic = l.substr(21, bar - 21);
      while (!e.topic.empty() && e.topic.back() == ' ') e.topic.pop_back();
      e.msg = l.substr(bar + 3);
    } else {
      e.msg = l.substr(21);
    }
    return e;
  }
  // WirePlumber: "I 16:45:19.152172    m-lua-scripting config.c:104:load_file: message"
  if (n > 17 && std::strchr("DIMWEC", p[0]) && p[0] && p[1] == ' ' && clock_us(p + 2, n - 2, &e.ts_ms) && p[17] == ' ') {
    static const char lv[] = "DIMWEC";
    static const int pr[] = {7, 6, 5, 4, 3, 2};
    e.prio = pr[std::strchr(lv, p[0]) - lv];
    e.who = "wireplumber";
    size_t t = 18;
    while (t < n && p[t] == ' ') ++t;
    e.topic = word(l, t, &at);
    e.msg = l.substr(at);
    return e;
  }
  // nsdk (btbenchd, the kernel, system.log): "20220403 11:35:59.169 [778.781] INFO Networking: message"
  if (stamp(p, n, &e.ts_ms) && n > 23 && p[21] == ' ' && p[22] == '[') {
    const size_t close = l.find("] ", 23);
    if (close != std::string::npos) {
      e.tid = l.substr(23, close - 23);
      e.pid = std::atoi(e.tid.c_str());
      const std::string lvl = word(l, close + 2, &at);
      const int pr = level_prio(lvl);
      if (pr >= 0) {
        e.prio = pr;
        // system.log: "ident: " first, a word without spaces (systemd, sshd, systemd-logind[235]).
        const size_t colon = l.find(": ", at);
        if (colon != std::string::npos && colon > at && colon - at < 40 &&
            l.find(' ', at) >= colon) {
          e.who = l.substr(at, colon - at);
          e.msg = l.substr(colon + 2);
        } else {
          e.msg = l.substr(at);
        }
        return e;
      }
    }
  }
  // wpa_supplicant: "1649400051.070940: dbus: message"
  {
    size_t d = 0;
    while (d < n && std::isdigit(static_cast<unsigned char>(p[d]))) ++d;
    if (d >= 9 && d + 8 < n && p[d] == '.' && digits(p + d + 1, 6) && p[d + 7] == ':' && p[d + 8] == ' ') {
      e.ts_ms = std::atoll(p) * 1000 + num(p + d + 1, 3);
      e.who = "wpa_supplicant";
      e.msg = l.substr(d + 9);
      return e;
    }
  }
  e = LogEntry{};
  e.msg = l;
  return e;
}

// ---------------------------------------------------------------------------------------------
// The filter

bool LogFilter::parse(const std::string& src, int max_prio, std::string* err) {
  terms_.clear();
  max_prio_ = std::clamp(max_prio, 0, 7);
  size_t i = 0;
  while (i < src.size()) {
    while (i < src.size() && src[i] == ' ') ++i;
    if (i >= src.size()) break;
    Term t;
    if (src[i] == '!') {
      t.neg = true;
      ++i;
    }
    if (i < src.size() && src[i] == '"') {
      const size_t e = src.find('"', i + 1);
      t.word = lower(src.substr(i + 1, (e == std::string::npos ? src.size() : e) - i - 1));
      i = e == std::string::npos ? src.size() : e + 1;
    } else if (i < src.size() && src[i] == '/') {
      // Up to the next unescaped '/', then its flags.
      size_t e = i + 1;
      while (e < src.size() && src[e] != '/') e += src[e] == '\\' ? 2 : 1;
      const std::string body = src.substr(i + 1, std::min(e, src.size()) - i - 1);
      size_t f = std::min(e + 1, src.size());
      std::string flags;
      while (f < src.size() && std::isalpha(static_cast<unsigned char>(src[f]))) flags += src[f++];
      i = f;
      bool icase = flags.find('i') != std::string::npos;
      if (!icase) icase = std::none_of(body.begin(), body.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
      // The page's regexes are JavaScript's; glibc's extended ones know \w and \b but not these.
      std::string ere;
      for (size_t k = 0; k < body.size(); ++k) {
        if (body[k] == '\\' && k + 1 < body.size()) {
          const char c = body[++k];
          if (c == 'd') ere += "[0-9]";
          else if (c == 'D') ere += "[^0-9]";
          else if (c == 's') ere += "[[:space:]]";
          else if (c == 'S') ere += "[^[:space:]]";
          else ere += std::string("\\") + c;
        } else if (body.compare(k, 3, "(?:") == 0) {
          ere += '(';
          k += 2;
        } else {
          ere += body[k];
        }
      }
      auto* re = new regex_t;
      const int rc = regcomp(re, ere.c_str(), REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0));
      if (rc != 0) {
        char msg[128];
        regerror(rc, re, msg, sizeof msg);
        delete re;
        *err = "bad regex /" + body + "/: " + msg;
        return false;
      }
      t.re = std::shared_ptr<void>(re, [](void* p) {
        regfree(static_cast<regex_t*>(p));
        delete static_cast<regex_t*>(p);
      });
    } else {
      const size_t e = src.find(' ', i);
      t.word = lower(src.substr(i, (e == std::string::npos ? src.size() : e) - i));
      i = e == std::string::npos ? src.size() : e;
    }
    if (t.re || !t.word.empty()) terms_.push_back(std::move(t));
  }
  return true;
}

bool LogFilter::match(const LogEntry& e) const {
  if (e.prio > max_prio_) return false;
  if (terms_.empty()) return true;
  // What the page shows of a line: the program, its pid, the topic and the message.
  std::string text = e.who;
  if (e.pid) text += "[" + std::to_string(e.pid) + "]";
  if (!text.empty()) text += ": ";
  if (!e.topic.empty()) text += e.topic + " ";
  text += e.msg;
  std::string low;
  for (const Term& t : terms_) {
    bool hit;
    if (t.re) {
      hit = regexec(static_cast<const regex_t*>(t.re.get()), text.c_str(), 0, nullptr, 0) == 0;
    } else {
      if (low.empty()) low = lower(text);
      hit = low.find(t.word) != std::string::npos;
    }
    if (hit == t.neg) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// JSON

void append_json_string(const std::string& s, std::string* out) {
  static const char* hex = "0123456789abcdef";
  out->push_back('"');
  // Runs of plain characters are appended at once; only the few that need it are escaped.
  size_t run = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c != '"' && c != '\\' && c != 0x7f) continue;
    out->append(s, run, i - run);
    run = i + 1;
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        *out += "\\u00";
        out->push_back(hex[c >> 4]);
        out->push_back(hex[c & 15]);
    }
  }
  out->append(s, run, std::string::npos);
  out->push_back('"');
}

void append_entries_json(const std::vector<LogEntry>& entries, std::string* out) {
  // By hand, and without temporaries: on the Zero W this is a good part of a 20000-line answer.
  size_t need = 2;
  for (const LogEntry& e : entries) need += 48 + e.who.size() + e.tid.size() + e.topic.size() + e.msg.size() * 11 / 10;
  out->reserve(out->size() + need);
  char num[32];
  auto put_num = [&](const char* key, long long v) {
    *out += key;
    const int n = std::snprintf(num, sizeof num, "%lld", v);
    out->append(num, static_cast<size_t>(n));
  };
  auto put_str = [&](const char* key, const std::string& v) {
    if (v.empty()) return;
    *out += key;
    append_json_string(v, out);
  };
  out->push_back('[');
  bool first = true;
  for (const LogEntry& e : entries) {
    if (!first) out->push_back(',');
    first = false;
    put_num("{\"ts\":", e.ts_ms);
    put_num(",\"prio\":", e.prio);
    put_str(",\"who\":", e.who);
    if (e.pid) put_num(",\"pid\":", e.pid);
    put_str(",\"tid\":", e.tid);
    put_str(",\"topic\":", e.topic);
    *out += ",\"msg\":";
    append_json_string(e.msg, out);
    out->push_back('}');
  }
  out->push_back(']');
}

bool gzip_fast(const std::string& in, std::string* out) {
  z_stream z{};
  if (deflateInit2(&z, 1, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
  out->resize(deflateBound(&z, in.size()));
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  z.avail_in = static_cast<uInt>(in.size());
  z.next_out = reinterpret_cast<Bytef*>(out->data());
  z.avail_out = static_cast<uInt>(out->size());
  const int r = deflate(&z, Z_FINISH);
  out->resize(z.total_out);
  deflateEnd(&z);
  return r == Z_STREAM_END;
}

// ---------------------------------------------------------------------------------------------
// Files: btbench-logd caps each at 2 MiB (in RAM), so a file is read whole and its lines walked
// from the end (or from `before`) backwards

bool read_log_file(const std::string& path, const LogQuery& q, LogPage* out, std::string* err) {
  *out = LogPage{};
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *err = "cannot open: " + std::string(std::strerror(errno));
    return false;
  }
  struct stat st {};
  fstat(fd, &st);
  constexpr long long kMax = 64LL << 20;  // a file put there by hand: its last 64 MiB
  const long long size = st.st_size;
  long long base = 0;
  if (q.after >= 0) {
    base = q.after;
    // Smaller than where the viewer stopped: rotated (btbench-logd renames it to .1 and starts
    // afresh).
    if (base > size) base = 0, out->reset = true;
  }
  if (size - base > kMax) base = size - kMax, out->reset = q.after >= 0;
  std::string text(static_cast<size_t>(size - base), '\0');
  ssize_t got = 0;
  while (got < static_cast<ssize_t>(text.size())) {
    const ssize_t r = pread(fd, text.data() + got, text.size() - got, base + got);
    if (r <= 0) break;
    got += r;
  }
  close(fd);
  text.resize(static_cast<size_t>(got));
  // A last line still being written is not a line yet: everything is up to the last '\n'.
  size_t end = text.rfind('\n');
  end = end == std::string::npos ? 0 : end + 1;
  out->end = base + static_cast<long long>(end);
  // Starting mid-file (cut to its last 64 MiB, or rotated): from the first whole line.
  size_t start = 0;
  if (base > 0 && (q.after < 0 || out->reset)) {
    const size_t nl = text.find('\n');
    start = nl == std::string::npos || nl >= end ? end : nl + 1;
  }

  if (q.after >= 0) {
    // Following: what was appended, oldest first.
    for (size_t at = start; at < end;) {
      const size_t nl = text.find('\n', at);
      if (nl > at) {
        ++out->scanned;
        LogEntry e = parse_log_line(text.substr(at, nl - at));
        if (q.filter.match(e)) out->entries.push_back(std::move(e));
      }
      at = nl + 1;
    }
    return true;
  }

  // Backwards from the end, or from `before` (a byte offset where a line starts).
  size_t e = end;
  if (!q.before.empty()) {
    const long long b = std::atoll(q.before.c_str()) - base;
    e = b <= static_cast<long long>(start) ? start : std::min(end, static_cast<size_t>(b));
  }
  std::vector<LogEntry> found;  // newest first
  size_t oldest = e;           // where the walk stopped
  while (e > start && static_cast<int>(found.size()) < q.lines) {
    // The line ending at e-1 ('\n') starts after the '\n' before it.
    const size_t nl = e >= 2 ? text.rfind('\n', e - 2) : std::string::npos;
    const size_t s = nl == std::string::npos || nl < start ? start : nl + 1;
    if (e - 1 > s) {
      ++out->scanned;
      LogEntry entry = parse_log_line(text.substr(s, e - 1 - s));
      if (q.filter.match(entry)) found.push_back(std::move(entry));
    }
    oldest = s;
    e = s;
  }
  out->before = oldest > start ? std::to_string(base + static_cast<long long>(oldest)) : std::string();
  out->entries.reserve(found.size());
  for (auto it = found.rbegin(); it != found.rend(); ++it) out->entries.push_back(std::move(*it));
  return true;
}

// ---------------------------------------------------------------------------------------------
// The journal

namespace {

// The journal files without an '@' in their name: the ones being written (system.journal,
// user-1000.journal). Archived files are "system@...journal" and "...journal~". With a journal in
// RAM (Storage=volatile, the image's), /run's are the ones being written, and a system.journal
// left in /var/log/journal from before is history like the archives.
std::vector<std::string> active_files() {
  std::vector<std::string> files;
  for (const char* root : {"/run/log/journal", "/var/log/journal"}) {
    if (!files.empty()) break;
    DIR* d = opendir(root);
    if (!d) continue;
    while (dirent* m = readdir(d)) {
      if (m->d_name[0] == '.') continue;
      const std::string dir = std::string(root) + "/" + m->d_name;
      DIR* dd = opendir(dir.c_str());
      if (!dd) continue;
      while (dirent* f = readdir(dd)) {
        const std::string n = f->d_name;
        if (n.size() > 8 && n.compare(n.size() - 8, 8, ".journal") == 0 && n.find('@') == std::string::npos)
          files.push_back(dir + "/" + n);
      }
      closedir(dd);
    }
    closedir(d);
  }
  return files;
}

// The fields the viewer shows, in one pass over the entry (one lookup per field is a hash and a
// search each, five times an entry).
void read_fields(sd_journal* j, LogEntry* e) {
  const void* data = nullptr;
  size_t len = 0;
  std::string comm;
  e->prio = 6;
  SD_JOURNAL_FOREACH_DATA(j, data, len) {
    const char* d = static_cast<const char*>(data);
    auto is = [&](const char* k, size_t n) { return len > n && std::memcmp(d, k, n) == 0; };
    if (is("MESSAGE=", 8)) e->msg.assign(d + 8, len - 8);
    else if (is("PRIORITY=", 9)) e->prio = d[9] - '0';
    else if (is("SYSLOG_IDENTIFIER=", 18)) e->who.assign(d + 18, len - 18);
    else if (is("_PID=", 5)) e->pid = std::atoi(std::string(d + 5, len - 5).c_str());
    else if (is("_COMM=", 6)) comm.assign(d + 6, len - 6);
  }
  if (e->who.empty()) e->who = std::move(comm);
}

void add_matches(sd_journal* j, const std::string& unit) {
  if (unit == "kernel") {
    sd_journal_add_match(j, "_TRANSPORT=kernel", 0);
  } else {
    // The unit's own messages, and systemd's about it (Started, Stopped, Main process exited).
    sd_journal_add_match(j, ("_SYSTEMD_UNIT=" + unit).c_str(), 0);
    sd_journal_add_disjunction(j);
    sd_journal_add_match(j, ("UNIT=" + unit).c_str(), 0);
    sd_journal_add_match(j, "_PID=1", 0);
  }
}

}  // namespace

bool read_journal(const std::string& unit, const LogQuery& q, LogPage* out, std::string* err) {
  *out = LogPage{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(q.budget_ms);
  std::vector<LogEntry> found;  // newest first
  std::string resume = q.before;  // the cursor to continue older than
  bool exhausted = false;
  // Pass 0: the active files only. Pass 1, only if more is wanted: every file, from where pass 0
  // stopped.
  for (int pass = 0; pass < 2 && static_cast<int>(found.size()) < q.lines && !exhausted; ++pass) {
    sd_journal* j = nullptr;
    int r;
    if (pass == 0) {
      std::vector<std::string> files = active_files();
      std::vector<const char*> paths;
      for (const auto& f : files) paths.push_back(f.c_str());
      paths.push_back(nullptr);
      r = files.empty() ? -ENOENT : sd_journal_open_files(&j, paths.data(), 0);
      if (r < 0) continue;  // straight to every file
    } else {
      r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
      if (r < 0) {
        *err = std::string("cannot open the journal: ") + std::strerror(-r);
        return false;
      }
    }
    add_matches(j, unit);
    bool have = false;
    if (resume.empty()) {
      sd_journal_seek_tail(j);
    } else {
      sd_journal_seek_cursor(j, resume.c_str());
      // On the cursor's entry, if this set of files has it: then step past it, older.
      if (sd_journal_previous(j) > 0) {
        have = true;
        if (sd_journal_test_cursor(j, resume.c_str()) > 0) have = false;
      }
    }
    bool hit_start = false;
    for (;;) {
      if (!have) {
        const int n = sd_journal_previous(j);
        if (n <= 0) {
          hit_start = true;
          break;
        }
      }
      have = false;
      if ((++out->scanned & 255) == 0 && std::chrono::steady_clock::now() > deadline) {
        out->limited = true;
        break;
      }
      LogEntry e;
      uint64_t us = 0;
      sd_journal_get_realtime_usec(j, &us);
      e.ts_ms = static_cast<int64_t>(us / 1000);
      read_fields(j, &e);
      if (q.filter.match(e)) {
        found.push_back(std::move(e));
        if (static_cast<int>(found.size()) >= q.lines) break;
      }
    }
    // Where this walk stopped (the entry it is on): the cursor the next one continues from. Only
    // here, not per entry: making one costs about as much as reading the entry.
    char* c = nullptr;
    if (!hit_start && sd_journal_get_cursor(j, &c) >= 0 && c) {
      resume = c;
      free(c);
    } else if (hit_start && sd_journal_seek_head(j) >= 0 && sd_journal_next(j) > 0 &&
               sd_journal_get_cursor(j, &c) >= 0 && c) {
      resume = c;  // the oldest entry of these files: pass 1 continues older than it
      free(c);
    }
    sd_journal_close(j);
    if (out->limited) break;
    // Every file read to its start: there is nothing older.
    if (pass == 1 && hit_start) exhausted = true;
  }
  out->before = exhausted ? std::string() : resume;
  out->entries.reserve(found.size());
  for (auto it = found.rbegin(); it != found.rend(); ++it) out->entries.push_back(std::move(*it));
  return true;
}

}  // namespace btb
