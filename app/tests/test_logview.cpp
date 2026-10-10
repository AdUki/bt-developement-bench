// The log viewer's reading side (logview.h): btbench-logd's lines parsed back into entries — each
// format checked on what logd itself writes — the filter, and paging a file backwards and forwards.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "check.h"
#include "logd.h"
#include "logview.h"

using btb::LogEntry;

namespace {

constexpr uint64_t kT = 1791626401234567ull;  // 2026-10-10 10:00:01.234567 UTC

// A journal entry as logd formats it, parsed back.
LogEntry round_trip(btb::logd::Entry e) {
  e.realtime_us = kT;
  const btb::logd::Route r = btb::logd::route(e);
  return btb::parse_log_line(btb::logd::format(e, r));
}

void test_formats() {
  btb::logd::Entry e;
  e.unit = "bluetooth.service";
  e.pid = 13077;
  e.priority = 7;
  e.message = "src/adapter.c:adapter_service_insert() /org/bluez/hci0";
  LogEntry x = round_trip(e);
  CHECK_EQ(x.who, std::string("bluetoothd"));
  CHECK_EQ(x.pid, 13077);
  CHECK_EQ(x.prio, 7);
  CHECK_EQ(x.msg, e.message);
  CHECK_EQ(x.ts_ms, static_cast<int64_t>(kT / 1000));

  e = btb::logd::Entry{};
  e.ident = "pipewire";
  e.priority = 4;
  e.message = "spa.alsa: hw:0: snd_pcm_avail after recover: Broken pipe";
  e.code_file = "alsa-pcm.c";
  e.code_line = 2584;
  e.code_func = "alsa_recover";
  x = round_trip(e);
  CHECK_EQ(x.who, std::string("pipewire"));
  CHECK_EQ(x.prio, 4);
  CHECK_EQ(x.topic, std::string("spa.alsa"));
  CHECK(x.msg.find("Broken pipe") != std::string::npos);
  CHECK_EQ(x.ts_ms % 1000, 234);

  e = btb::logd::Entry{};
  e.ident = "wireplumber";
  e.priority = 3;
  e.topic = "m-lua-scripting";
  e.message = "cannot load";
  e.code_file = "config.c";
  e.code_line = 104;
  e.code_func = "load_file";
  x = round_trip(e);
  CHECK_EQ(x.who, std::string("wireplumber"));
  CHECK_EQ(x.prio, 3);
  CHECK_EQ(x.topic, std::string("m-lua-scripting"));
  CHECK(x.msg.find("cannot load") != std::string::npos);

  e = btb::logd::Entry{};
  e.ident = "systemd";
  e.pid = 1;
  e.priority = 6;
  e.message = "Started Session c159 of User root.";
  x = round_trip(e);
  CHECK_EQ(x.who, std::string("systemd"));
  CHECK_EQ(x.prio, 6);
  CHECK_EQ(x.msg, e.message);
  CHECK_EQ(x.ts_ms, static_cast<int64_t>(kT / 1000));

  e = btb::logd::Entry{};
  e.unit = "btbenchd.service";
  e.pid = 778;
  e.tid = 781;
  e.priority = 3;
  e.message = "media: no answer in time";
  x = round_trip(e);
  CHECK_EQ(x.prio, 3);
  CHECK_EQ(x.tid, std::string("778.781"));
  CHECK_EQ(x.pid, 778);

  e = btb::logd::Entry{};
  e.ident = "wpa_supplicant";
  e.message = "dbus: Unregister BSS object";
  x = round_trip(e);
  CHECK_EQ(x.who, std::string("wpa_supplicant"));
  CHECK_EQ(x.msg, e.message);
  CHECK_EQ(x.ts_ms, static_cast<int64_t>(kT / 1000));

  x = btb::parse_log_line("something else entirely");
  CHECK_EQ(x.prio, 6);
  CHECK_EQ(x.ts_ms, 0);
  CHECK_EQ(x.msg, std::string("something else entirely"));
}

void test_filter() {
  btb::LogFilter f;
  std::string err;
  LogEntry e;
  e.who = "bluetoothd";
  e.pid = 12;
  e.prio = 6;
  e.msg = "Device 0C:32:3A:D6:C3:44 is already connected";
  CHECK(f.parse("device !failed", 7, &err));
  CHECK(f.match(e));
  CHECK(f.parse("\"already connected\" bluetoothd[12]", 7, &err));
  CHECK(f.match(e));
  CHECK(f.parse("/0c:32:3a/", 7, &err));  // no capital: case-insensitive
  CHECK(f.match(e));
  CHECK(f.parse("/0c:32:3A/", 7, &err));  // a capital: as written
  CHECK(!f.match(e));
  CHECK(f.parse("!connected", 7, &err));
  CHECK(!f.match(e));
  CHECK(f.parse("", 4, &err));  // warnings and worse
  CHECK(!f.match(e));
  CHECK(!f.parse("/(/", 7, &err));
  CHECK(!err.empty());
}

void test_file(const std::string& dir) {
  const std::string path = dir + "/t.log";
  {
    std::ofstream o(path);
    for (int i = 1; i <= 10; ++i) o << "line " << i << (i % 3 == 0 ? " three" : "") << "\n";
    o << "partial";  // being written: not a line yet
  }
  btb::LogQuery q;
  btb::LogPage p;
  std::string err;
  q.lines = 4;
  CHECK(btb::read_log_file(path, q, &p, &err));
  CHECK_EQ(p.entries.size(), 4u);
  CHECK_EQ(p.entries.front().msg, std::string("line 7"));
  CHECK_EQ(p.entries.back().msg, std::string("line 10"));
  CHECK(!p.before.empty());
  const long long end = p.end;
  // Older: the four before those, then the last two, then nothing.
  q.before = p.before;
  CHECK(btb::read_log_file(path, q, &p, &err));
  CHECK_EQ(p.entries.front().msg, std::string("line 3 three"));
  CHECK_EQ(p.entries.back().msg, std::string("line 6 three"));
  q.before = p.before;
  CHECK(btb::read_log_file(path, q, &p, &err));
  CHECK_EQ(p.entries.size(), 2u);
  CHECK(p.before.empty());
  // A filter, over the whole file.
  q = btb::LogQuery{};
  CHECK(q.filter.parse("three", 7, &err));
  CHECK(btb::read_log_file(path, q, &p, &err));
  CHECK_EQ(p.entries.size(), 3u);
  // Following: the partial line completes, one more comes.
  {
    std::ofstream o(path, std::ios::app);
    o << " now\nline 12\n";
  }
  q = btb::LogQuery{};
  q.after = end;
  CHECK(btb::read_log_file(path, q, &p, &err));
  CHECK_EQ(p.entries.size(), 2u);
  CHECK_EQ(p.entries.front().msg, std::string("partial now"));
  CHECK(!p.reset);
  // Rotated: smaller than where the viewer was.
  {
    std::ofstream o(path, std::ios::trunc);
    o << "fresh\n";
  }
  q.after = p.end;
  CHECK(btb::read_log_file(path, q, &p, &err));
  CHECK(p.reset);
  CHECK_EQ(p.entries.size(), 1u);
}

void test_json() {
  std::vector<LogEntry> v(1);
  v[0].ts_ms = 5;
  v[0].msg = "a \"b\"\n\x01";
  std::string out;
  btb::append_entries_json(v, &out);
  CHECK_EQ(out, std::string("[{\"ts\":5,\"prio\":6,\"msg\":\"a \\\"b\\\"\\n\\u0001\"}]"));
}

}  // namespace

int main() {
  setenv("TZ", "UTC", 1);
  char tmpl[] = "/tmp/btb-logview-XXXXXX";
  const char* dir = mkdtemp(tmpl);
  if (!dir) return 1;
  test_formats();
  test_filter();
  test_file(dir);
  test_json();
  std::system((std::string("rm -rf '") + dir + "'").c_str());
  return report("test_logview");
}
