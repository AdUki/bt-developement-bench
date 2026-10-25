// The whole module through its public API: a btsnoop file written here is replayed by a Monitor
// (fast and paced), and the HTTP routes are exercised on a loopback httplib server.

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <httplib.h>
#include <string>
#include <thread>

#include "../check.h"
#include "btsnoop.h"
#include "hci_monitor.h"
#include "packets.h"

using namespace hcitest;
using nlohmann::json;

namespace {

std::string g_tmp;

std::string tmp_path(const std::string& name) { return g_tmp + "/" + name; }

void write_monitor_file(const std::string& path, const std::vector<Pkt>& pkts) {
  btb::hci::BtsnoopWriter w;
  CHECK(w.open(path));
  for (const auto& p : pkts) CHECK(w.write(p.ts, p.index, p.opcode, p.data.data(), p.data.size()));
  w.close();
}

void write_script(const std::string& path, const std::string& body) {
  std::ofstream(path) << "#!/bin/sh\n" << body << "\n";
  chmod(path.c_str(), 0755);
}

bool has_event(const json& evs, const std::string& kind, const std::string& needle) {
  for (const auto& e : evs) {
    if (e["kind"] == kind && e["text"].get<std::string>().find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void test_reader_roundtrip() {
  const std::string path = tmp_path("rt.btsnoop");
  const auto pkts = a2dp_session();
  write_monitor_file(path, pkts);
  btb::hci::BtsnoopReader r;
  std::string err;
  CHECK(r.open(path, &err));
  CHECK_EQ(r.datalink(), btb::hci::kBtsnoopMonitor);
  btb::hci::SnoopPacket p;
  size_t n = 0;
  while (r.read(&p)) {
    if (n < pkts.size()) {
      CHECK_EQ(p.ts_us, pkts[n].ts);
      CHECK_EQ(p.opcode, pkts[n].opcode);
      CHECK_EQ(p.len, pkts[n].data.size());
    }
    ++n;
  }
  CHECK_EQ(n, pkts.size());

  // A cut-off last record (btmon still writing) reads as end of file.
  {
    std::ifstream in(path, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::ofstream(tmp_path("cut.btsnoop"), std::ios::binary) << all.substr(0, all.size() - 5);
  }
  btb::hci::BtsnoopReader cut;
  CHECK(cut.open(tmp_path("cut.btsnoop"), &err));
  n = 0;
  while (cut.read(&p)) ++n;
  CHECK_EQ(n, pkts.size() - 1);

  btb::hci::BtsnoopReader bad;
  std::ofstream(tmp_path("bad.btsnoop")) << "not a capture at all";
  CHECK(!bad.open(tmp_path("bad.btsnoop"), &err));
  CHECK(err.find("not a btsnoop file") != std::string::npos);
}

// The session without its final disconnection, so the link is still there when the replay ends.
std::vector<Pkt> session_connected() {
  auto v = a2dp_session();
  v.pop_back();
  return v;
}

// The timestamp as btmon writes it (src/shared/btsnoop.c): µs since 2000 plus 0x00E03AB44A676000.
// A reader using that constant against the Unix epoch puts every capture 30 years early.
void test_epoch() {
  const std::string path = tmp_path("epoch.btsnoop");
  const int64_t unix_us = 1791612221476915LL;  // 2026-10-10 06:03:41.476915 UTC
  const uint8_t note[] = {'x', 0};
  btb::hci::BtsnoopWriter w;
  CHECK(w.open(path));
  CHECK(w.write(unix_us, 0, 0x0d, note, sizeof note));
  w.close();
  uint8_t rec[40] = {};
  FILE* f = std::fopen(path.c_str(), "rb");
  CHECK(f && std::fread(rec, 1, sizeof rec, f) == sizeof rec);
  if (f) std::fclose(f);
  uint64_t raw = 0;
  for (int i = 0; i < 8; ++i) raw = raw << 8 | rec[16 + 16 + i];
  CHECK_EQ(raw, static_cast<uint64_t>((unix_us - 946684800LL * 1000000) + 0x00E03AB44A676000LL));
  btb::hci::BtsnoopReader r;
  std::string err;
  CHECK(r.open(path, &err));
  btb::hci::SnoopPacket p;
  CHECK(r.read(&p));
  CHECK_EQ(p.ts_us, unix_us);
}

void test_replay_fast() {
  const std::string path = tmp_path("a2dp.btsnoop");
  write_monitor_file(path, session_connected());

  std::atomic<int> n_stats{0}, n_events{0};
  btb::hci::Options o;
  o.replay_file = path;
  o.replay_fast = true;
  o.replay_record = false;  // the default capture directory is this PC's
  auto mon = btb::hci::start(o, [&](const std::string& topic, const json&) {
    if (topic == "hci.stats") ++n_stats;
    if (topic == "hci.event") ++n_events;
  });
  CHECK(mon->available());
  CHECK(mon->wait_replay_done(5000));

  const json evs = mon->events(0);
  CHECK(has_event(evs, "avdtp", "TX SET_CONFIGURATION cmd acp 1 int 3: SBC 48000 Hz, joint stereo"));
  CHECK(has_event(evs, "avdtp", "RX START accept"));
  json lat;
  CHECK(mon->latency(0, kHandle, &lat));
  CHECK_EQ(lat["lat_ms"]["n"], 21);
  json hist;
  CHECK(mon->history(0, kHandle, 300, &hist));
  CHECK_EQ(hist["t"].size(), 3u);
  CHECK_EQ(hist["in_flight"][1], 3);
  const json s = mon->stats();
  CHECK_EQ(s["source"]["kind"], "replay");
  CHECK_EQ(s["source"]["done"], true);
  CHECK_EQ(s["source"]["packets"].get<size_t>(), session_connected().size());
  // The end of the file closed the last, partial window (+2.000 .. +2.020 s).
  CHECK_EQ(s["window_ms"], 20);
  CHECK(n_events.load() >= static_cast<int>(evs.size()));
  CHECK(n_stats.load() >= 1);
}

// The same session as an H4 (datalink 1002) file, the format Android and vendor stacks write.
void test_replay_h4() {
  const std::string path = tmp_path("a2dp-h4.btsnoop");
  btb::hci::BtsnoopWriter w;
  CHECK(w.open(path, btb::hci::kBtsnoopUart));
  for (const auto& p : session_connected()) {
    uint8_t type = 0;
    bool rx = false;
    switch (p.opcode) {
      case kMonEvent: type = 0x04; rx = true; break;
      case kMonAclTx: type = 0x02; break;
      case kMonAclRx: type = 0x02; rx = true; break;
      default: continue;  // index/open opcodes have no H4 equivalent
    }
    CHECK(w.write_h4(p.ts, type, rx, p.data.data(), p.data.size()));
  }
  w.close();

  btb::hci::Options o;
  o.replay_file = path;
  o.replay_fast = true;
  o.replay_record = false;  // the default capture directory is this PC's
  auto mon = btb::hci::start(o, nullptr);
  CHECK(mon->available());
  CHECK(mon->wait_replay_done(5000));
  json lat;
  CHECK(mon->latency(0, kHandle, &lat));
  CHECK_EQ(lat["lat_ms"]["n"], 21);
  CHECK(has_event(mon->events(0), "l2cap", "AVDTP media open"));
}

// Paced replay of a short file while a live pcap client is attached.
void test_paced_pcap() {
  // Ten ACL packets 150 ms apart on an unknown link; the client needs any three of them.
  std::vector<Pkt> pkts;
  for (int i = 0; i < 10; ++i) {
    pkts.push_back({kT0 + i * 150000, 0, kMonAclRx, acl(9, 2, l2cap(0x0040, Bytes{1, 2, 3}))});
  }
  const std::string path = tmp_path("paced.btsnoop");
  write_monitor_file(path, pkts);

  btb::hci::Options o;
  o.replay_file = path;
  o.replay_record = false;  // the default capture directory is this PC's
  auto mon = btb::hci::start(o, nullptr);
  httplib::Server svr;
  btb::hci::register_routes(svr, *mon);
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });
  svr.wait_until_ready();

  std::string body;
  size_t records = 0;
  httplib::Client cli("127.0.0.1", port);
  cli.set_read_timeout(5, 0);
  auto res = cli.Get("/api/hci/live.pcap", [&](const char* data, size_t len) {
    body.append(data, len);
    // header (24) + records of 16 + 4 + 7 bytes
    records = body.size() >= 24 ? (body.size() - 24) / 27 : 0;
    return records < 3;  // enough: hang up
  });
  CHECK(body.size() >= 24 + 3 * 27);
  if (body.size() >= 24) {
    uint32_t magic, link;
    std::memcpy(&magic, body.data(), 4);
    std::memcpy(&link, body.data() + 20, 4);
    CHECK_EQ(magic, 0xa1b2c3d4u);
    CHECK_EQ(link, 254u);
  }
  CHECK(mon->wait_replay_done(5000));
  svr.stop();
  th.join();
}

void test_routes() {
  const std::string path = tmp_path("routes.btsnoop");
  write_monitor_file(path, session_connected());
  const std::string capdir = tmp_path("cap");
  mkdir(capdir.c_str(), 0755);
  std::ofstream(capdir + "/hci-20261009-120000.btsnoop") << "old";
  sleep(1);  // distinct mtimes (1 s resolution on some filesystems)
  std::ofstream(capdir + "/hci-20261009-130000.btsnoop") << "newest";
  std::ofstream(capdir + "/notes.txt") << "ignored";
  write_script(tmp_path("systemctl"), "[ \"$1\" = is-active ] && { echo active; exit 0; }\n"
                                      "echo \"$1 $2\" >> " + tmp_path("systemctl.log"));
  write_script(tmp_path("btmon"), "echo \"analyzed $2\"");

  btb::hci::Options o;
  o.replay_file = path;
  o.replay_fast = true;
  o.replay_record = false;  // the ring's files are counted below
  o.capture_dir = capdir;
  o.capture_unit = "test-btsnoop.service";
  o.systemctl = tmp_path("systemctl");
  o.btmon = tmp_path("btmon");
  auto mon = btb::hci::start(o, nullptr);
  CHECK(mon->wait_replay_done(5000));

  httplib::Server svr;
  btb::hci::register_routes(svr, *mon);
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });
  svr.wait_until_ready();
  httplib::Client cli("127.0.0.1", port);

  auto r = cli.Get("/api/hci/stats");
  CHECK(r && r->status == 200);
  if (r) {
    const json s = json::parse(r->body);
    CHECK(s.contains("adapters") && s.contains("conns") && s.contains("ts"));
    CHECK_EQ(s["window_ms"].is_number(), true);
  }
  r = cli.Get("/api/hci/events?since=0");
  CHECK(r && r->status == 200 && json::parse(r->body).size() > 5);
  r = cli.Get("/api/hci/latency?index=0&handle=11");
  CHECK(r && r->status == 200 && json::parse(r->body)["counts"].size() == 500);
  r = cli.Get("/api/hci/history?handle=11&seconds=2");
  CHECK(r && r->status == 200 && json::parse(r->body)["t"].size() == 2);
  r = cli.Get("/api/hci/history?handle=99");
  CHECK(r && r->status == 404);
  r = cli.Get("/api/hci/history");
  CHECK(r && r->status == 400);

  r = cli.Post("/api/hci/mark", R"({"text":"before the stall"})", "application/json");
  CHECK(r && r->status == 200);
  if (r) {
    const json m = json::parse(r->body);
    CHECK_EQ(m["logged"], false);  // replay: no kernel logging channel
    const uint64_t seq = m["seq"].get<uint64_t>();
    auto e = cli.Get(("/api/hci/events?since=" + std::to_string(seq - 1)).c_str());
    CHECK(e && json::parse(e->body).size() == 1 &&
          json::parse(e->body)[0]["text"] == "before the stall");
  }
  r = cli.Post("/api/hci/mark", "{}", "application/json");
  CHECK(r && r->status == 400);

  // capture ring
  r = cli.Get("/api/capture");
  CHECK(r && r->status == 200);
  if (r) {
    const json c = json::parse(r->body);
    CHECK_EQ(c["running"], true);
    CHECK_EQ(c["unit"], "test-btsnoop.service");
    CHECK_EQ(c["files"].size(), 2u);
    if (c["files"].size() == 2) {
      CHECK_EQ(c["files"][0]["name"], "hci-20261009-130000.btsnoop");
      CHECK_EQ(c["files"][0]["active"], true);
      CHECK_EQ(c["files"][1]["active"], false);
      CHECK_EQ(c["files"][1]["size"], 3);
    }
  }
  r = cli.Get("/api/capture/files/hci-20261009-120000.btsnoop");
  CHECK(r && r->status == 200 && r->body == "old");
  r = cli.Get("/api/capture/files/notes.txt");
  CHECK(r && r->status == 400);
  r = cli.Get("/api/capture/files/..%2Fetc%2Fpasswd.btsnoop");
  CHECK(r && (r->status == 400 || r->status == 404));
  r = cli.Get("/api/capture/files/hci-20261009-120000.btsnoop/analyze");
  CHECK(r && r->status == 200);
  if (r) CHECK(r->body.find("analyzed " + capdir + "/hci-20261009-120000.btsnoop") == 0);
  r = cli.Delete("/api/capture/files/hci-20261009-130000.btsnoop");
  CHECK(r && r->status == 409);  // being written
  r = cli.Delete("/api/capture/files/hci-20261009-120000.btsnoop");
  CHECK(r && r->status == 200);
  r = cli.Delete("/api/capture/files/hci-20261009-120000.btsnoop");
  CHECK(r && r->status == 404);
  r = cli.Put("/api/capture", R"({"running":false})", "application/json");
  CHECK(r && r->status == 200);
  {
    std::ifstream log(tmp_path("systemctl.log"));
    std::string line;
    std::getline(log, line);
    CHECK_EQ(line, std::string("stop test-btsnoop.service"));
  }
  r = cli.Put("/api/capture", R"({"running":"yes"})", "application/json");
  CHECK(r && r->status == 400);

  svr.stop();
  th.join();
}

// A replay is recorded into the capture directory under a ring file's name, marked as being
// written until it ends: the packet view's live end follows a replay as it follows btmon's file.
void test_replay_record() {
  // Ten ACL packets 150 ms apart, as in test_paced_pcap.
  std::vector<Pkt> pkts;
  for (int i = 0; i < 10; ++i) {
    pkts.push_back({kT0 + i * 150000, 0, kMonAclRx, acl(9, 2, l2cap(0x0040, Bytes{1, 2, 3}))});
  }
  const std::string path = tmp_path("record.btsnoop");
  write_monitor_file(path, pkts);
  const std::string capdir = tmp_path("record-cap");
  mkdir(capdir.c_str(), 0755);

  btb::hci::Options o;
  o.replay_file = path;
  o.capture_dir = capdir;
  o.systemctl = "/nonexistent/systemctl";
  auto mon = btb::hci::start(o, nullptr);
  httplib::Server svr;
  btb::hci::register_routes(svr, *mon);
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });
  svr.wait_until_ready();
  httplib::Client cli("127.0.0.1", port);
  cli.set_read_timeout(10, 0);

  // While it replays: one file, named like the ring's, active although no unit runs.
  auto r = cli.Get("/api/capture");
  CHECK(r && r->status == 200);
  std::string name;
  if (r) {
    const json c = json::parse(r->body);
    CHECK_EQ(c["running"], false);
    CHECK_EQ(c["files"].size(), 1u);
    if (c["files"].size() == 1) {
      name = c["files"][0]["name"].get<std::string>();
      CHECK(name.rfind("hci-", 0) == 0 && name.size() == std::string("hci-20261009-120000.btsnoop").size());
      CHECK_EQ(c["files"][0]["active"], true);
    }
  }
  // A tail request from the start answers with what has been replayed so far, then one from
  // there waits for the next packets and gets them as they are recorded.
  r = cli.Get("/api/capture/files/" + name + "/packets?count=1000&wait=3000");
  CHECK(r && r->status == 200);
  size_t have = 0;
  if (r) {
    const json j = json::parse(r->body);
    have = j["packets"].size();
    CHECK(have >= 1 && have < 10);
    CHECK_EQ(j["complete"], true);
  }
  r = cli.Get("/api/capture/files/" + name + "/packets?start=" + std::to_string(have) + "&count=1000&wait=3000");
  CHECK(r && r->status == 200);
  if (r) {
    const json j = json::parse(r->body);
    CHECK(!j["packets"].empty());
    CHECK_EQ(j["start"], have);
    if (!j["packets"].empty()) CHECK_EQ(j["packets"][0]["n"], have + 1);
  }
  r = cli.Delete("/api/capture/files/" + name);
  CHECK(r && r->status == 409);  // being written

  CHECK(mon->wait_replay_done(5000));
  // Over: the recording stays, complete, and is no longer marked active.
  r = cli.Get("/api/capture");
  if (r) {
    const json c = json::parse(r->body);
    CHECK_EQ(c["files"].size(), 1u);
    if (c["files"].size() == 1) CHECK_EQ(c["files"][0]["active"], false);
  }
  r = cli.Get("/api/capture/files/" + name + "/packets?count=1000");
  CHECK(r && r->status == 200);
  if (r) {
    const json j = json::parse(r->body);
    CHECK_EQ(j["total"], 10);
    CHECK_EQ(j["complete"], true);
  }
  svr.stop();
  th.join();
}

void test_unavailable() {
  btb::hci::Options o;
  o.replay_file = tmp_path("does-not-exist.btsnoop");
  o.capture_dir = tmp_path("nocap");
  o.systemctl = "/nonexistent/systemctl";
  auto mon = btb::hci::start(o, nullptr);
  CHECK(!mon->available());
  CHECK(mon->error().find("does-not-exist") != std::string::npos);

  httplib::Server svr;
  btb::hci::register_routes(svr, *mon);
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });
  svr.wait_until_ready();
  httplib::Client cli("127.0.0.1", port);
  auto r = cli.Get("/api/hci/stats");
  CHECK(r && r->status == 503);
  if (r) CHECK(json::parse(r->body)["error"].get<std::string>().find("unavailable") != std::string::npos);
  r = cli.Get("/api/hci/live.pcap");
  CHECK(r && r->status == 503);
  // Capture management does not depend on the monitor source.
  r = cli.Get("/api/capture");
  CHECK(r && r->status == 200);
  if (r) {
    const json c = json::parse(r->body);
    CHECK_EQ(c["running"], false);
    CHECK_EQ(c["state"], "unknown");
    CHECK_EQ(c["files"].size(), 0u);
  }
  svr.stop();
  th.join();
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/btb-hci-test-XXXXXX";
  const char* tmp = mkdtemp(tmpl);
  if (!tmp) return 1;
  g_tmp = tmp;

  test_reader_roundtrip();
  test_epoch();
  test_replay_fast();
  test_replay_h4();
  test_paced_pcap();
  test_routes();
  test_replay_record();
  test_unavailable();

  if (std::system(("rm -rf '" + g_tmp + "'").c_str()) != 0) std::printf("cannot remove %s\n", g_tmp.c_str());
  return report("test_hci_replay");
}
