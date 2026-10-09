// The packet view of capture files: the index (and its incremental growth while btmon writes),
// the filter language, summaries and the detail decode of the common packet kinds, the graph's
// buckets, and the routes on a loopback server.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <httplib.h>
#include <string>
#include <thread>

#include "../check.h"
#include "btsnoop.h"
#include "hci_monitor.h"
#include "monitor_impl.h"
#include "packet_view.h"
#include "packets.h"  // the test builders

using namespace hcitest;
using nlohmann::json;
using btb::hci::Filter;
using btb::hci::PacketIndex;
using btb::hci::PacketStore;

namespace {

std::string g_tmp;
std::string tmp_path(const std::string& name) { return g_tmp + "/" + name; }

void write_file(const std::string& path, const std::vector<Pkt>& pkts) {
  btb::hci::BtsnoopWriter w;
  CHECK(w.open(path));
  for (const auto& p : pkts) CHECK(w.write(p.ts, p.index, p.opcode, p.data.data(), p.data.size()));
  w.close();
}

// The A2DP session plus the other packet kinds the summaries are checked on: commands, an LE
// link with ATT and SMP, advertising, a btbench mark.
std::vector<Pkt> mixed_session() {
  std::vector<Pkt> v = a2dp_session();
  int64_t t = kT0 + 3000000;
  auto at = [&](uint16_t op, const Bytes& d) {
    v.push_back({t, 0, op, d});
    t += 1000;
  };
  // Disconnect handle 11 reason 0x13, its Command Status.
  at(kMonCommand, Bytes{0x06, 0x04, 0x03, 11, 0x00, 0x13});
  at(kMonEvent, evt(0x0f, Bytes{0x00, 0x01, 0x06, 0x04}));
  // A failed Command Complete: LE Create Connection Cancel, Command Disallowed.
  at(kMonEvent, cmd_complete(0x200e, Bytes{0x0c}));
  // LE advertising report with a name.
  {
    Bytes ad = {0x02, 0x01, 0x06, 0x05, 0x09, 'B', 'e', 'a', 'n'};
    Bytes r{0x01, 0x00, 0x01};
    r = cat(r, kLePeer);
    r.push_back(static_cast<uint8_t>(ad.size()));
    r = cat(r, ad);
    r.push_back(static_cast<uint8_t>(-60));
    at(kMonEvent, le_meta(0x02, r));
  }
  // LE connection on handle 64, ATT and SMP on it.
  {
    Bytes p{0x00, 64, 0x00, 0x00, 0x01};
    p = cat(p, kLePeer);
    p = cat(p, Bytes{0x18, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00});
    at(kMonEvent, le_meta(0x01, p));
  }
  at(kMonAclTx, acl(64, 0, l2cap(kCidAtt, Bytes{0x0a, 0x03, 0x00})));           // Read Request 0x0003
  at(kMonAclRx, acl(64, 2, l2cap(kCidAtt, Bytes{0x01, 0x0a, 0x03, 0x00, 0x05})));  // Error: insuff. auth
  at(kMonAclRx, acl(64, 2, l2cap(kCidAtt, Bytes{0x1b, 0x2a, 0x00, 'h', 'i'})));   // Notification
  at(kMonAclTx, acl(64, 0, l2cap(kCidSmp, Bytes{0x05, 0x08})));                    // Pairing Failed
  // A btbench mark on the logging channel.
  {
    Bytes m{6, 8, 'b', 't', 'b', 'e', 'n', 'c', 'h', 0};
    for (char c : std::string("before the stall")) m.push_back(static_cast<uint8_t>(c));
    m.push_back(0);
    at(kMonUserLogging, m);
  }
  return v;
}

size_t count_matches(PacketIndex& idx, const std::string& src) {
  Filter f;
  std::string err;
  if (!Filter::parse(src, &f, &err)) {
    std::printf("filter '%s': %s\n", src.c_str(), err.c_str());
    return static_cast<size_t>(-1);
  }
  return idx.query(f, INT64_MAX)->matches.size();
}

size_t find_summary(PacketIndex& idx, const std::string& needle) {
  for (size_t i = 0; i < idx.size(); ++i) {
    if (idx.summary(i).find(needle) != std::string::npos) return i;
  }
  return static_cast<size_t>(-1);
}

void test_index() {
  const std::string path = tmp_path("a2dp.btsnoop");
  const auto pkts = mixed_session();
  write_file(path, pkts);
  PacketIndex idx;
  std::string err;
  std::lock_guard<std::mutex> lk(idx.mu);
  CHECK(idx.open(path, &err));
  CHECK(idx.extend(INT64_MAX));
  CHECK_EQ(idx.size(), pkts.size());
  CHECK_EQ(idx.t0_us(), kT0);

  // Channel tracking: the signalling channel's data is AVDTP, the second PSM 25 channel RTP.
  size_t rtp = 0, avdtp = 0, cont = 0;
  for (size_t i = 0; i < idx.size(); ++i) {
    const auto& e = idx.at(i);
    if (e.proto == btb::hci::kProtoRtp) {
      ++rtp;
      CHECK_EQ(e.cid, kMediaRcid);
      CHECK_EQ(e.psm, kPsmAvdtp);
    }
    if (e.proto == btb::hci::kProtoAvdtp) ++avdtp;
    if (e.flags & btb::hci::kFlagCont) {
      ++cont;
      CHECK_EQ(e.proto, btb::hci::kProtoAvdtp);  // SET_CONFIGURATION's continuation
    }
  }
  CHECK_EQ(rtp, 13u);
  CHECK_EQ(avdtp, 11u);  // 5 commands (one in two fragments) + 5 accepts
  CHECK_EQ(cont, 1u);

  // NOCP latency, the same accounting as the live monitor: first media packet 2 ms, the three
  // sent at +1.900 s complete at +2.020 s.
  std::vector<uint32_t> lats;
  for (size_t i = 0; i < idx.size(); ++i) {
    if (idx.at(i).proto == btb::hci::kProtoRtp) lats.push_back(idx.at(i).lat_us);
  }
  CHECK_EQ(lats.size(), 13u);
  if (lats.size() == 13) {
    CHECK_EQ(lats[0], 2000u);
    CHECK_EQ(lats[9], 11000u);
    CHECK_EQ(lats[10], 120000u);
    CHECK_EQ(lats[12], 118000u);
  }

  // Connections, for the graph's list.
  bool acl = false, le = false;
  for (const auto& c : idx.conns()) {
    if (c.handle == kHandle) {
      acl = true;
      CHECK_EQ(std::string(c.type), "acl");
      CHECK_EQ(c.peer, "AA:BB:CC:DD:EE:FF");
      CHECK_EQ(c.tx_pkts, 13u + 8u);
    }
    if (c.handle == 64) {
      le = true;
      CHECK_EQ(std::string(c.type), "le");
    }
  }
  CHECK(acl && le);

  // Commands and events carry the handle they are about.
  const size_t disc = find_summary(idx, "Disconnect handle 11");
  CHECK(disc != static_cast<size_t>(-1));
  if (disc != static_cast<size_t>(-1)) CHECK_EQ(idx.at(disc).handle, kHandle);
}

void test_filter() {
  Filter f;
  std::string err;
  CHECK(Filter::parse("handle:11 !proto:rtp type:acl|evt len>4 t>=0.5 t<2.5 \"read request\"", &f, &err));
  CHECK_EQ(f.terms.size(), 7u);
  CHECK(f.has_text);
  CHECK_EQ(f.terms.back().kind, Filter::Term::kText);  // text sorts last
  CHECK_EQ(f.terms.back().text, std::string("read request"));
  CHECK_NEAR(f.t_lo, 0.5, 1e-9);
  CHECK_NEAR(f.t_hi, 2.5, 1e-9);
  CHECK(Filter::parse("cid:0x40|0x41 psm:25", &f, &err));
  CHECK_EQ(f.terms[0].values.size(), 2u);
  CHECK_EQ(f.terms[0].values[1], 0x41u);
  CHECK(Filter::parse("AA:BB:CC:DD:EE:FF", &f, &err));  // an address is text, not a field
  CHECK_EQ(f.terms[0].kind, Filter::Term::kText);
  CHECK(Filter::parse("", &f, &err));
  CHECK(f.terms.empty());

  CHECK(!Filter::parse("type:foo", &f, &err));
  CHECK(err.find("cmd|evt") != std::string::npos);
  CHECK(!Filter::parse("len>abc", &f, &err));
  CHECK(!Filter::parse("handle:", &f, &err));
  CHECK(!Filter::parse("is:weird", &f, &err));

  const std::string path = tmp_path("filter.btsnoop");
  write_file(path, mixed_session());
  PacketIndex idx;
  std::lock_guard<std::mutex> lk(idx.mu);
  CHECK(idx.open(path, &err));
  idx.extend(INT64_MAX);
  CHECK_EQ(count_matches(idx, "proto:rtp"), 13u);
  CHECK_EQ(count_matches(idx, "proto:rtp lat>100"), 3u);
  CHECK_EQ(count_matches(idx, "proto:rtp !lat>100"), 10u);
  CHECK_EQ(count_matches(idx, "is:lat"), 13u + 8u);  // media + the 8 signalling packets
  CHECK_EQ(count_matches(idx, "proto:rtp t>=1 t<1.5"), 10u);
  CHECK_EQ(count_matches(idx, "proto:avdtp|rtp"), 24u);
  CHECK_EQ(count_matches(idx, "dir:rx type:acl handle:11"), 7u);
  CHECK_EQ(count_matches(idx, "cid:1 handle:11"), 4u);
  CHECK_EQ(count_matches(idx, "type:cmd"), 1u);
  CHECK_EQ(count_matches(idx, "opcode:0x0406"), 2u);  // the command and its Command Status
  CHECK_EQ(count_matches(idx, "evt:0x13"), 12u);
  CHECK_EQ(count_matches(idx, "subevt:2"), 1u);
  CHECK_EQ(count_matches(idx, "is:err"), 3u);  // failed Command Complete, ATT error, SMP failed
  CHECK_EQ(count_matches(idx, "is:mark"), 1u);
  CHECK_EQ(count_matches(idx, "type:log"), 1u);
  CHECK_EQ(count_matches(idx, "proto:att"), 3u);
  CHECK_EQ(count_matches(idx, "n<=3"), 3u);
  CHECK_EQ(count_matches(idx, "set_configuration"), 2u);  // text: command and accept
  CHECK_EQ(count_matches(idx, "proto:avdtp !set_configuration"), 9u);
  CHECK_EQ(count_matches(idx, "\"read request\" handle:64"), 2u);  // the request and the error
  CHECK_EQ(count_matches(idx, "bean"), 1u);
}

void test_summaries_and_detail() {
  const std::string path = tmp_path("sum.btsnoop");
  write_file(path, mixed_session());
  PacketIndex idx;
  std::string err;
  std::lock_guard<std::mutex> lk(idx.mu);
  CHECK(idx.open(path, &err));
  idx.extend(INT64_MAX);

  CHECK(find_summary(idx, "Read Buffer Size complete ACL 1021 x 8") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Connection Complete handle 11 AA:BB:CC:DD:EE:FF (ACL)") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Connection Request psm 0x0019 (AVDTP) scid 0x0040") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Connection Response dcid 0x0050 scid 0x0040 success") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "SET_CONFIGURATION cmd acp 1 int 3") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "SET_CONFIGURATION accept") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "DISCOVER accept: 1 audio sink") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "GET_CAPABILITIES accept: SBC") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "RTP seq 100 ts 5000, SBC 7 frames") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Number of Completed Packets handle 11: 3") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Disconnection Complete handle 11 reason 0x13") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Disconnect handle 11 reason 0x13") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "LE Create Connection Cancel complete status 0x0c (Command Disallowed)") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "LE Advertising Report ADV_IND C1:22:33:44:55:66 (random) -60 dBm \"Bean\"") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "LE Connection Complete handle 64 C1:22:33:44:55:66 (random) central") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Read Request handle 0x0003") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Error Response: Read Request handle 0x0003 Insufficient Authentication") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Handle Value Notification handle 0x002a 6869 \"hi\"") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "Pairing Failed: Unspecified Reason") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "btbench: before the stall") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "New Index hci0 06:05:04:03:02:01 (UART)") != static_cast<size_t>(-1));
  CHECK(find_summary(idx, "continuation, ") != static_cast<size_t>(-1));

  // The detail of a media packet: Frame, HCI ACL, L2CAP, RTP and SBC layers, with byte ranges
  // that stay inside the packet.
  const size_t i = find_summary(idx, "RTP seq 100 ");
  CHECK(i != static_cast<size_t>(-1));
  if (i == static_cast<size_t>(-1)) return;
  size_t len = 0;
  const uint8_t* d = idx.read(i, &len);
  CHECK(d != nullptr);
  btb::hci::Dissection ds;
  btb::hci::dissect(idx.at(i), d, len, true, &ds);
  std::vector<std::string> layers;
  bool ranges_ok = true;
  std::function<void(const json&)> walk = [&](const json& arr) {
    for (const auto& f : arr) {
      if (f.contains("off") && f["off"].get<size_t>() + f["len"].get<size_t>() > len) ranges_ok = false;
      if (f.contains("children")) walk(f["children"]);
    }
  };
  for (const auto& f : ds.fields) layers.push_back(f["name"].get<std::string>());
  walk(ds.fields);
  CHECK(ranges_ok);
  CHECK_EQ(layers.size(), 4u);
  if (layers.size() == 4) {
    CHECK_EQ(layers[0], std::string("HCI ACL"));
    CHECK_EQ(layers[1], std::string("L2CAP"));
    CHECK_EQ(layers[2], std::string("RTP"));
    CHECK_EQ(layers[3], std::string("SBC"));
  }
  CHECK(ds.fields.dump().find("\"Sequence number\"") != std::string::npos);
}

// btmon keeps writing the newest file: the index picks up from where it stopped, including a
// record that was cut in half the first time, and latencies whose NOCP arrived later.
void test_incremental() {
  const auto pkts = a2dp_session();
  const std::string full = tmp_path("full.btsnoop");
  write_file(full, pkts);
  std::ifstream in(full, std::ios::binary);
  const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // Cut inside the record of the first media packet's NOCP.
  size_t first_media = 0;
  for (size_t i = 0; i < pkts.size(); ++i) {
    if (pkts[i].ts >= kT0 + 1000000) {
      first_media = i;
      break;
    }
  }
  size_t off = 16;
  for (size_t i = 0; i <= first_media; ++i) off += 24 + pkts[i].data.size();
  const size_t cut = off + 10;  // inside the next record's header
  const std::string grow = tmp_path("grow.btsnoop");
  std::ofstream(grow, std::ios::binary) << all.substr(0, cut);

  PacketIndex idx;
  std::string err;
  std::lock_guard<std::mutex> lk(idx.mu);
  CHECK(idx.open(grow, &err));
  CHECK(idx.extend(INT64_MAX));
  CHECK_EQ(idx.size(), first_media + 1);
  CHECK_EQ(idx.at(first_media).lat_us, btb::hci::kNoLat);  // its NOCP is not in the file yet
  Filter f;
  CHECK(Filter::parse("proto:rtp", &f, &err));
  CHECK_EQ(idx.query(f, INT64_MAX)->matches.size(), 1u);

  {
    std::ofstream app(grow, std::ios::binary | std::ios::app);
    app << all.substr(cut);
  }
  CHECK(idx.still_valid());
  CHECK(idx.extend(INT64_MAX));
  CHECK_EQ(idx.size(), pkts.size());
  CHECK_EQ(idx.at(first_media).lat_us, 2000u);
  // The cached query continues from where it was.
  const auto* q = idx.query(f, INT64_MAX);
  CHECK_EQ(q->matches.size(), 13u);
  CHECK_EQ(q->scanned, pkts.size());

  // A file replaced under the same name (a new inode) is not the one indexed.
  const std::string tmp = grow + ".new";
  std::ofstream(tmp, std::ios::binary) << all;
  CHECK(std::rename(tmp.c_str(), grow.c_str()) == 0);
  CHECK(!idx.still_valid());
}

// The same session as H4 (datalink 1002): directions come from the flags, the index is the same.
void test_h4() {
  const std::string path = tmp_path("h4.btsnoop");
  btb::hci::BtsnoopWriter w;
  CHECK(w.open(path, btb::hci::kBtsnoopUart));
  size_t n = 0;
  for (const auto& p : a2dp_session()) {
    uint8_t type = 0;
    bool rx = false;
    switch (p.opcode) {
      case kMonEvent: type = 0x04; rx = true; break;
      case kMonAclTx: type = 0x02; break;
      case kMonAclRx: type = 0x02; rx = true; break;
      default: continue;
    }
    CHECK(w.write_h4(p.ts, type, rx, p.data.data(), p.data.size()));
    ++n;
  }
  w.close();
  PacketIndex idx;
  std::string err;
  std::lock_guard<std::mutex> lk(idx.mu);
  CHECK(idx.open(path, &err));
  CHECK_EQ(idx.datalink(), btb::hci::kBtsnoopUart);
  idx.extend(INT64_MAX);
  CHECK_EQ(idx.size(), n);
  CHECK_EQ(count_matches(idx, "proto:rtp lat>0"), 13u);
  CHECK(find_summary(idx, "RTP seq 100 ts 5000") != static_cast<size_t>(-1));
}

void test_graph() {
  const std::string dir = tmp_path("graphdir");
  mkdir(dir.c_str(), 0755);
  write_file(dir + "/hci-1.btsnoop", a2dp_session());
  PacketStore store(dir);
  PacketStore::GraphParams p;
  p.bucket_ms = 1000;
  json g;
  int status = 0;
  std::string err;
  CHECK(store.graph("hci-1.btsnoop", p, &g, &status, &err));
  CHECK_EQ(g["bucket_ms"], 1000);
  CHECK_EQ(g["buckets"], 3);
  CHECK_EQ(g["complete"], true);
  const json& c = g["conn"];
  CHECK_EQ(c["handle"], kHandle);  // the busiest link is picked
  // Second 1: 13 media packets of 4 + 13 + 600 bytes of payload after the ACL header.
  CHECK_EQ(c["tx_bps"][1], 13u * (4 + 13 + 600) * 8);
  CHECK_NEAR(c["lat_p50"][1].get<double>(), 8.5, 0.01);  // 2..11, 118..120 ms: the 7th, in [8, 9)
  CHECK(c["lat_p50"][2].is_null());  // completions land in the window the TX was in
  CHECK_EQ(g["filtered"]["pkts"][0].get<int>() + g["filtered"]["pkts"][1].get<int>() +
               g["filtered"]["pkts"][2].get<int>(),
           static_cast<int>(a2dp_session().size()));

  // The filter's t range zooms the graph, and the automatic bucket keeps it under 2000 points.
  p = PacketStore::GraphParams{};
  p.filter = "proto:rtp t>=1 t<2";
  CHECK(store.graph("hci-1.btsnoop", p, &g, &status, &err));
  CHECK_EQ(g["from"], 1.0);
  CHECK_EQ(g["to"], 2.0);
  CHECK_EQ(g["bucket_ms"], 1);
  CHECK_EQ(g["buckets"], 1000);
  CHECK_EQ(g["matches"], 13);

  CHECK(!store.graph("nope.btsnoop", p, &g, &status, &err));
  CHECK_EQ(status, 404);
  p.filter = "type:nonsense";
  CHECK(!store.graph("hci-1.btsnoop", p, &g, &status, &err));
  CHECK_EQ(status, 400);
}

json get_json(httplib::Client& cli, const std::string& path, int want = 200) {
  auto r = cli.Get(path.c_str());
  CHECK(r);
  if (!r) return json();
  if (r->status != want) std::printf("GET %s: %d %s\n", path.c_str(), r->status, r->body.c_str());
  CHECK_EQ(r->status, want);
  return json::parse(r->body, nullptr, false);
}

void test_routes() {
  const std::string dir = tmp_path("cap");
  mkdir(dir.c_str(), 0755);
  write_file(dir + "/hci-20261009-120000.btsnoop", mixed_session());
  std::ofstream(dir + "/junk.btsnoop") << "not a capture at all";

  btb::hci::Options o;
  o.replay_file = tmp_path("none.btsnoop");  // no source: the capture routes still work
  o.capture_dir = dir;
  o.systemctl = "/nonexistent/systemctl";
  auto mon = btb::hci::start(o, nullptr);
  httplib::Server svr;
  btb::hci::register_routes(svr, *mon);
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });
  svr.wait_until_ready();
  httplib::Client cli("127.0.0.1", port);
  const std::string base = "/api/capture/files/hci-20261009-120000.btsnoop";
  const size_t total = mixed_session().size();

  json j = get_json(cli, base + "/packets?count=5");
  CHECK_EQ(j["total"], total);
  CHECK_EQ(j["packets_in_file"], total);
  CHECK_EQ(j["complete"], true);
  CHECK(j["next"].is_null());
  CHECK_EQ(j["packets"].size(), 5u);
  CHECK_EQ(j["packets"][0]["n"], 1);
  CHECK_EQ(j["packets"][0]["type"], "index");
  CHECK_EQ(j["packets"][0]["t"], 0.0);

  j = get_json(cli, base + "/packets?filter=" + httplib::encode_uri_component("proto:rtp lat>100") + "&start=1&count=10");
  CHECK_EQ(j["total"], 3);
  CHECK_EQ(j["start"], 1);
  CHECK_EQ(j["packets"].size(), 2u);
  if (j["packets"].size() == 2) {
    const json& p = j["packets"][0];
    CHECK_EQ(p["proto"], "rtp");
    CHECK_EQ(p["dir"], "tx");
    CHECK_EQ(p["handle"], kHandle);
    CHECK_EQ(p["cid"], kMediaRcid);
    CHECK_EQ(p["psm"], kPsmAvdtp);
    CHECK_EQ(p["lat_ms"], 119.0);
    CHECK(p["summary"].get<std::string>().find("RTP seq") == 0);
  }
  // Jumping to a time and to a frame.
  j = get_json(cli, base + "/packets?filter=proto:rtp&at_t=1.5&count=1");
  CHECK_EQ(j["start"], 10);  // media at +1.000 .. +1.180, then +1.900: the 11th
  j = get_json(cli, base + "/packets?at_n=30&count=1");
  CHECK_EQ(j["start"], 29);
  CHECK_EQ(j["packets"][0]["n"], 30);

  // One packet in detail.
  j = get_json(cli, base + "/packets/" + std::to_string(j["packets"][0]["n"].get<int>()));
  CHECK(j.contains("fields") && j["fields"].is_array());
  CHECK_EQ(j["fields"][0]["name"], "Frame");
  CHECK_EQ(j["hex"].get<std::string>().size(), 2 * j["len"].get<size_t>());
  get_json(cli, base + "/packets/0", 404);
  get_json(cli, base + "/packets/99999", 404);

  // Errors.
  j = get_json(cli, base + "/packets?filter=" + httplib::encode_uri_component("type:bogus"), 400);
  CHECK(j["error"].get<std::string>().find("unknown type") != std::string::npos);
  get_json(cli, "/api/capture/files/missing.btsnoop/packets", 404);
  get_json(cli, "/api/capture/files/notes.txt/packets", 400);
  get_json(cli, "/api/capture/files/junk.btsnoop/packets", 415);
  get_json(cli, base + "/graph?conn=x", 400);

  j = get_json(cli, base + "/graph?conn=0:64");
  CHECK_EQ(j["conn"]["handle"], 64);
  CHECK(j["conns"].size() >= 2u);

  // A text filter with no time to spare: partial answers with a resume point, until complete.
  mon->impl().packets->set_budget_ms(0);
  {
    // A file big enough that one request cannot scan it all within the minimum progress.
    std::vector<Pkt> many;
    for (int i = 0; i < 5000; ++i) {
      many.push_back({kT0 + i * 1000, 0, kMonAclRx, acl(9, 2, l2cap(kCidAtt, Bytes{0x1b, 0x2a, 0x00, 1}))});
    }
    write_file(dir + "/big.btsnoop", many);
  }
  int rounds = 0;
  for (;;) {
    j = get_json(cli, "/api/capture/files/big.btsnoop/packets?filter=notification&count=1");
    ++rounds;
    if (j["complete"].get<bool>() || rounds > 100) break;
    CHECK(j["next"].is_number());
  }
  CHECK(rounds > 1);
  CHECK_EQ(j["total"], 5000);

  svr.stop();
  th.join();
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/btb-hci-packets-XXXXXX";
  const char* tmp = mkdtemp(tmpl);
  if (!tmp) return 1;
  g_tmp = tmp;

  test_index();
  test_filter();
  test_summaries_and_detail();
  test_incremental();
  test_h4();
  test_graph();
  test_routes();

  if (std::system(("rm -rf '" + g_tmp + "'").c_str()) != 0) std::printf("cannot remove %s\n", g_tmp.c_str());
  return report("test_hci_packets");
}
