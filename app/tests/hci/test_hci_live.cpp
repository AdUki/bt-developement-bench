// The live packet view: filtering fed packets exactly as the capture view filters the same
// packets in a file, channel tracking seeded mid-stream from the Decoder, the ring and its detail
// lookup, the per-stream drop/skip accounting and latency hold, and the routes over a paced replay.

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <httplib.h>
#include <sstream>
#include <string>
#include <thread>

#include "../check.h"
#include "btsnoop.h"
#include "decoder.h"
#include "hci_monitor.h"
#include "live.h"
#include "packet_view.h"
#include "packets.h"

using namespace hcitest;
using nlohmann::json;
using btb::hci::Decoder;
using btb::hci::Filter;
using btb::hci::LiveView;
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

Filter parse(const std::string& s) {
  Filter f;
  std::string err;
  CHECK(Filter::parse(s, &f, &err));
  return f;
}

// A drain's output split into its status lines and packet lines.
struct Batch {
  std::vector<json> status, pkts;
};
Batch split(const std::string& out) {
  Batch b;
  std::istringstream in(out);
  std::string line;
  while (std::getline(in, line)) {
    const json j = json::parse(line, nullptr, false);
    CHECK(!j.is_discarded());
    if (j.is_discarded()) continue;
    if (j.contains("status")) b.status.push_back(j["status"]);
    else b.pkts.push_back(j);
  }
  return b;
}

// Everything the stream has: drains until nothing is left (held packets released by a clock
// two seconds ahead).
Batch drain_all(LiveView& lv, LiveView::Client& c) {
  Batch all;
  for (int i = 0; i < 100; ++i) {
    std::string out;
    lv.drain(c, LiveView::mono_us() + 2000000, &out);
    Batch b = split(out);
    for (auto& s : b.status) all.status.push_back(std::move(s));
    if (b.pkts.empty() && i > 0) break;
    for (auto& p : b.pkts) all.pkts.push_back(std::move(p));
  }
  return all;
}

// The A2DP session plus an LE link with ATT (one error response), a failed command and a mark.
std::vector<Pkt> session() {
  std::vector<Pkt> v = a2dp_session();
  int64_t t = kT0 + 3000000;
  auto at = [&](uint16_t op, const Bytes& d) {
    v.push_back({t, 0, op, d});
    t += 1000;
  };
  at(kMonEvent, cmd_complete(0x200e, Bytes{0x0c}));
  {
    Bytes p{0x00, 64, 0x00, 0x00, 0x01};
    p = cat(p, kLePeer);
    p = cat(p, Bytes{0x18, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00});
    at(kMonEvent, le_meta(0x01, p));
  }
  at(kMonAclTx, acl(64, 0, l2cap(kCidAtt, Bytes{0x0a, 0x03, 0x00})));
  at(kMonAclRx, acl(64, 2, l2cap(kCidAtt, Bytes{0x01, 0x0a, 0x03, 0x00, 0x05})));
  at(kMonAclRx, acl(64, 2, l2cap(kCidAtt, Bytes{0x1b, 0x2a, 0x00, 'h', 'i'})));
  {
    Bytes m{6, 8, 'b', 't', 'b', 'e', 'n', 'c', 'h', 0};
    for (char c : std::string("before the stall")) m.push_back(static_cast<uint8_t>(c));
    m.push_back(0);
    at(kMonUserLogging, m);
  }
  return v;
}

// ---------------------------------------------------------------------------------------------

// The same packets, fed live and written to a file, must match the same filters the same way:
// the live view numbers packets from 1 like a file's frames, so the sets compare directly.
void test_matches_capture_view() {
  const auto pkts = session();
  const std::string dir = tmp_path("cap");
  mkdir(dir.c_str(), 0755);
  write_file(dir + "/hci-20261009-120000.btsnoop", pkts);
  PacketStore store(dir);

  const char* filters[] = {
      "",          "proto:rtp",       "proto:avdtp",       "proto:att",    "is:err",
      "is:mark",   "type:cmd|evt",    "!type:acl",         "dir:rx type:acl", "handle:11 dir:tx",
      "cid:0x51",  "psm:25 !proto:rtp", "is:cont",         "lat>100",      "lat>5 lat<10",
      "len>600",   "\"rtp seq\"",     "proto:att \"error\"", "!\"number of completed\"", "n<=10",
      "t>=1 t<2",  "evt:0x13",        "opcode:0x200e",     "is:lat",
  };
  for (const char* fs : filters) {
    LiveView lv;
    auto c = lv.subscribe(parse(fs), -1);
    CHECK(c != nullptr);
    if (!c) continue;
    CHECK(lv.active());
    CHECK(lv.needs_seed());
    lv.seed({});
    CHECK(!lv.needs_seed());
    for (const auto& p : pkts) lv.push(p.ts, p.index, p.opcode, p.data.data(), p.data.size());
    const Batch live = drain_all(lv, *c);

    PacketStore::ListParams lp;
    lp.filter = fs;
    lp.count = 1000;
    json file;
    int status = 0;
    std::string err;
    CHECK(store.list("hci-20261009-120000.btsnoop", lp, &file, &status, &err));
    CHECK_EQ(file["complete"], true);
    CHECK_EQ(live.pkts.size(), file["packets"].size());
    if (live.pkts.size() != file["packets"].size()) {
      std::printf("  filter: %s\n", fs);
      continue;
    }
    for (size_t i = 0; i < live.pkts.size(); ++i) {
      json a = live.pkts[i];
      const json& b = file["packets"][i];
      CHECK_EQ(a["seq"], b["n"]);
      a.erase("seq");
      // The hand-written line is the capture view's object: same keys, same values.
      if (a != b) {
        std::printf("FAIL filter '%s' packet %zu:\n  live %s\n  file %s\n", fs, i, a.dump().c_str(),
                    b.dump().c_str());
        ++g_failures;
        break;
      }
    }
  }
}

// The live view starts tracking only when someone watches: a viewer who arrives after the A2DP
// channels opened still sees proto:rtp (from the Decoder's seed), and a packet in flight at that
// moment does not shift every later latency by one.
void test_seeded_mid_stream() {
  const auto pkts = a2dp_session();
  // Up to and including the first media packet: its NOCP comes after the viewer arrives.
  size_t split_at = 0;
  for (size_t i = 0; i < pkts.size(); ++i) {
    if (pkts[i].opcode == kMonAclTx && pkts[i].ts >= kT0 + 1000000) {
      split_at = i + 1;
      break;
    }
  }
  CHECK(split_at > 0);

  for (bool seeded : {true, false}) {
    Decoder dec;
    LiveView lv;
    for (size_t i = 0; i < split_at; ++i) {
      const auto& p = pkts[i];
      dec.packet(p.ts, p.index, p.opcode, p.data.data(), p.data.size());
    }
    auto c = lv.subscribe(parse("dir:tx type:acl"), -1);
    CHECK(lv.needs_seed());
    if (seeded) {
      const auto links = dec.links();
      CHECK_EQ(links.size(), 1u);
      if (links.size() == 1) {
        CHECK_EQ(links[0].handle, kHandle);
        CHECK_EQ(links[0].in_flight, 1);
        CHECK_EQ(links[0].chans.size(), 2u);
      }
      lv.seed(links);
    } else {
      lv.seed({});
    }
    for (size_t i = split_at; i < pkts.size(); ++i) {
      const auto& p = pkts[i];
      lv.push(p.ts, p.index, p.opcode, p.data.data(), p.data.size());
      dec.packet(p.ts, p.index, p.opcode, p.data.data(), p.data.size());
    }
    const Batch b = drain_all(lv, *c);
    // 9 more media packets in second 1, 3 at its end.
    CHECK_EQ(b.pkts.size(), 12u);
    if (b.pkts.size() != 12) continue;
    if (seeded) {
      for (const auto& p : b.pkts) {
        CHECK_EQ(p["proto"], "rtp");
        CHECK_EQ(p["psm"], kPsmAvdtp);
        CHECK_EQ(p["cid"], kMediaRcid);
      }
      // The second media packet completes 3 ms after it was sent, the third 4 ms, ...
      CHECK_EQ(b.pkts[0]["lat_ms"], 3.0);
      CHECK_EQ(b.pkts[8]["lat_ms"], 11.0);
      CHECK_EQ(b.pkts[9]["lat_ms"], 120.0);
      CHECK_EQ(b.pkts[11]["lat_ms"], 118.0);
    } else {
      // Unseeded, the media channel is just a CID.
      CHECK_EQ(b.pkts[0]["proto"], "l2cap");
      CHECK(b.pkts[0]["psm"].is_null());
    }
  }

  // A packet in flight when the viewer arrives: its NOCP comes after the next TX, and without
  // the seed's placeholder it would be taken as that one's (1 ms instead of 4).
  for (bool seeded : {true, false}) {
    std::vector<Pkt> v;
    v.push_back({kT0, 0, kMonEvent, conn_complete(kHandle, kPeer)});
    const Bytes media = acl(kHandle, 0, l2cap(kMediaRcid, rtp(1, 0, 7, 100)));
    v.push_back({kT0 + 1000, 0, kMonAclTx, media});   // in flight at the seed
    v.push_back({kT0 + 4000, 0, kMonAclTx, media});   // completes at +8 ms
    v.push_back({kT0 + 5000, 0, kMonEvent, nocp(kHandle, 1)});
    v.push_back({kT0 + 7000, 0, kMonAclTx, media});   // completes at +11 ms
    v.push_back({kT0 + 8000, 0, kMonEvent, nocp(kHandle, 1)});
    v.push_back({kT0 + 11000, 0, kMonEvent, nocp(kHandle, 1)});
    Decoder dec;
    LiveView lv;
    for (size_t i = 0; i < 2; ++i) dec.packet(v[i].ts, v[i].index, v[i].opcode, v[i].data.data(), v[i].data.size());
    auto c = lv.subscribe(parse("dir:tx"), -1);
    lv.seed(seeded ? dec.links() : std::vector<btb::hci::LinkSeed>{});
    for (size_t i = 2; i < v.size(); ++i) lv.push(v[i].ts, v[i].index, v[i].opcode, v[i].data.data(), v[i].data.size());
    const Batch b = drain_all(lv, *c);
    CHECK_EQ(b.pkts.size(), 2u);
    if (b.pkts.size() != 2) continue;
    if (seeded) {
      CHECK_EQ(b.pkts[0]["lat_ms"], 4.0);
      CHECK_EQ(b.pkts[1]["lat_ms"], 4.0);
    } else {
      CHECK_EQ(b.pkts[0]["lat_ms"], 1.0);
    }
  }

  // Channels opened after the viewer arrived are tracked by the live view itself.
  LiveView lv;
  auto c = lv.subscribe(parse("proto:rtp|avdtp"), -1);
  lv.seed({});
  for (const auto& p : pkts) lv.push(p.ts, p.index, p.opcode, p.data.data(), p.data.size());
  const Batch b = drain_all(lv, *c);
  size_t rtp = 0, avdtp = 0;
  for (const auto& p : b.pkts) {
    if (p["proto"] == "rtp") ++rtp;
    if (p["proto"] == "avdtp") ++avdtp;
  }
  CHECK_EQ(rtp, 13u);
  CHECK_EQ(avdtp, 11u);  // 10 signalling messages, SET_CONFIGURATION's continuation included
}

// The ring keeps the last kRing packets, numbered from 1; detail() decodes one of them.
void test_ring_and_detail() {
  LiveView lv;
  CHECK(!lv.active());
  Bytes big = acl(kHandle, 0, l2cap(0x0051, Bytes(3000, 0xab)));
  json d;
  CHECK(!lv.detail(1, &d));  // nothing yet
  auto c = lv.subscribe(parse(""), -1);
  lv.seed({});
  const size_t n = LiveView::kRing + 100;
  for (size_t i = 0; i < n; ++i) {
    const Bytes p = i == n - 2 ? big : acl(kHandle, 0, l2cap(kCidAtt, Bytes{0x0a, 0x03, 0x00}));
    lv.push(kT0 + static_cast<int64_t>(i) * 1000, 0, kMonAclTx, p.data(), p.size());
  }
  lv.push(kT0 + static_cast<int64_t>(n) * 1000, 0, kMonEvent, nocp(kHandle, 1).data(), nocp(kHandle, 1).size());
  CHECK_EQ(lv.last_seq(), n + 1);

  CHECK(!lv.detail(0, &d));
  CHECK(!lv.detail(1, &d));                  // overwritten
  CHECK(!lv.detail(n + 1 - LiveView::kRing, &d));
  CHECK(lv.detail(n + 2 - LiveView::kRing, &d));  // the oldest kept
  CHECK(!lv.detail(n + 2, &d));              // not yet
  CHECK(lv.detail(n + 1, &d));
  CHECK_EQ(d["seq"], n + 1);
  CHECK_EQ(d["n"], n + 1);
  CHECK_EQ(d["type"], "evt");
  CHECK(d["summary"].get<std::string>().find("Number of Completed Packets") == 0);
  CHECK_EQ(d["hex"], "1305010b000100");
  CHECK(d["fields"].is_array() && d["fields"][0]["name"] == "Frame");

  // The NOCP completed the oldest TX packet still in the FIFO (128 deep), which the ring has.
  CHECK(lv.detail(n - 127, &d));
  CHECK(d["lat_ms"].is_number());

  // A packet beyond kMaxBytes is kept cut, and the detail says so.
  CHECK(lv.detail(n - 1, &d));
  CHECK_EQ(d["len"], big.size());
  CHECK_EQ(d["hex"].get<std::string>().size(), 2 * LiveView::kMaxBytes);
  bool kept = false;
  for (const auto& f : d["fields"][0]["children"]) kept = kept || f["name"] == "Kept";
  CHECK(kept);

  // since=: a new stream starts after that sequence number, or at the oldest packet kept.
  auto c2 = lv.subscribe(parse(""), static_cast<int64_t>(n - 5));
  CHECK(c2 != nullptr);
  if (c2) {
    const Batch b = drain_all(lv, *c2);
    CHECK_EQ(b.pkts.size(), 6u);
    CHECK_EQ(b.status.at(0)["dropped"], 0);
  }
  auto c3 = lv.subscribe(parse(""), 0);
  if (c3) {
    std::string out;
    lv.drain(*c3, LiveView::mono_us(), &out);
    const Batch b = split(out);
    // The first batch is the backlog: the whole ring, nothing dropped or skipped.
    CHECK_EQ(b.status.at(0)["dropped"], 0);
    CHECK_EQ(b.status.at(0)["skipped"], 0);
    CHECK_EQ(b.pkts.size(), LiveView::kRing);
    if (!b.pkts.empty()) {
      CHECK_EQ(b.pkts.front()["seq"], n + 2 - LiveView::kRing);
      CHECK_EQ(b.pkts.back()["seq"], n + 1);
    }
  }
  lv.unsubscribe(c);
  if (c2) lv.unsubscribe(c2);
  if (c3) lv.unsubscribe(c3);
  CHECK(!lv.active());
}

// Each stream has its own filter and cursor; one that falls behind loses the oldest packets and
// says how many; too many matches in one batch are skipped, newest kept; a text filter's window
// is bounded too.
void test_streams_and_drops() {
  LiveView lv;
  auto all = lv.subscribe(parse(""), -1);
  auto rx = lv.subscribe(parse("dir:rx"), -1);
  auto text = lv.subscribe(parse("\"handle 0x0003\""), -1);
  auto tail = lv.subscribe(parse("dir:tx"), -1);
  CHECK(lv.subscribe(parse(""), -1) == nullptr);  // kMaxSubscribers
  lv.seed({});

  const Bytes tx = acl(64, 0, l2cap(kCidAtt, Bytes{0x0a, 0x03, 0x00}));  // Read Request 0x0003
  const Bytes rxp = acl(64, 2, l2cap(kCidAtt, Bytes{0x0b, 'x'}));
  int64_t t = kT0;
  auto feed = [&](size_t count) {
    for (size_t i = 0; i < count; ++i) {
      const Bytes& p = i % 2 ? rxp : tx;
      lv.push(t, 0, i % 2 ? kMonAclRx : kMonAclTx, p.data(), p.size());
      t += 100;
    }
  };

  // A small batch: everything gets through, each stream its own part.
  feed(100);
  std::string out;
  lv.drain(*all, LiveView::mono_us(), &out);
  Batch b = split(out);
  CHECK_EQ(b.status.size(), 1u);  // the start line
  CHECK_EQ(b.status.at(0)["filter"], "");
  CHECK_EQ(b.pkts.size(), 100u);
  out.clear();
  lv.drain(*rx, LiveView::mono_us(), &out);
  b = split(out);
  CHECK_EQ(b.pkts.size(), 50u);
  for (const auto& p : b.pkts) CHECK_EQ(p["dir"], "rx");
  out.clear();
  lv.drain(*text, LiveView::mono_us(), &out);
  b = split(out);
  CHECK_EQ(b.pkts.size(), 50u);
  for (const auto& p : b.pkts) CHECK_EQ(p["dir"], "tx");

  // Nothing new: no lines at all (the heartbeat is not due).
  out.clear();
  lv.drain(*all, LiveView::mono_us(), &out);
  CHECK(out.empty());

  // 5000 more: "all" was not drained for a while. The ring kept 2048, of which one batch looks
  // at kMaxScan (both counted as dropped), and sends the newest kMaxSend (the rest skipped).
  feed(5000);
  out.clear();
  lv.drain(*all, LiveView::mono_us(), &out);
  b = split(out);
  CHECK_EQ(b.status.size(), 1u);
  CHECK_EQ(b.status.at(0)["dropped"], 5000 - LiveView::kMaxScan);
  CHECK_EQ(b.status.at(0)["skipped"], LiveView::kMaxScan - LiveView::kMaxSend);
  CHECK_EQ(b.pkts.size(), LiveView::kMaxSend);
  if (!b.pkts.empty()) {
    CHECK_EQ(b.pkts.front()["seq"], 5100 - LiveView::kMaxSend + 1);
    CHECK_EQ(b.pkts.back()["seq"], 5100);
  }
  CHECK_EQ(all->sent, 100 + LiveView::kMaxSend);

  // The text stream decodes at most kMaxDecode candidates; the older ones were never examined.
  out.clear();
  lv.drain(*text, LiveView::mono_us(), &out);
  b = split(out);
  CHECK_EQ(b.status.at(0)["dropped"], 5000 - LiveView::kMaxScan + (LiveView::kMaxScan - LiveView::kMaxDecode));
  CHECK_EQ(b.status.at(0)["skipped"], 0);
  CHECK_EQ(b.pkts.size(), LiveView::kMaxDecode / 2);

  // The counts never go back, and a status line comes only when they change.
  feed(10);
  out.clear();
  lv.drain(*all, LiveView::mono_us(), &out);
  b = split(out);
  CHECK(b.status.empty());
  CHECK_EQ(b.pkts.size(), 10u);

  lv.close_all();
  CHECK(!lv.wait(*all));
}

// A latency filter holds a TX packet until its NOCP (or for kLatHoldUs without one).
void test_latency_hold() {
  LiveView lv;
  auto c = lv.subscribe(parse("lat>1"), -1);
  auto plain = lv.subscribe(parse("dir:tx"), -1);
  lv.seed({});
  CHECK(c->lat_hold);
  CHECK(!plain->lat_hold);
  const Bytes cc = conn_complete(kHandle, kPeer);
  lv.push(kT0, 0, kMonEvent, cc.data(), cc.size());
  const Bytes tx = acl(kHandle, 0, l2cap(0x0051, Bytes(20, 1)));
  lv.push(kT0 + 1000, 0, kMonAclTx, tx.data(), tx.size());  // seq 2: completed after 5 ms
  lv.push(kT0 + 2000, 0, kMonAclTx, tx.data(), tx.size());  // seq 3: never completed

  std::string out;
  lv.drain(*c, LiveView::mono_us(), &out);
  Batch b = split(out);
  CHECK(b.pkts.empty());
  CHECK_EQ(c->cursor, 2u);  // held at the first TX packet
  out.clear();
  lv.drain(*plain, LiveView::mono_us(), &out);
  b = split(out);
  CHECK_EQ(b.pkts.size(), 2u);  // a stream without a latency term is not held
  if (b.pkts.size() == 2) CHECK(b.pkts[0]["lat_ms"].is_null());

  const Bytes done = nocp(kHandle, 1);
  lv.push(kT0 + 6000, 0, kMonEvent, done.data(), done.size());
  out.clear();
  lv.drain(*c, LiveView::mono_us(), &out);
  b = split(out);
  CHECK_EQ(b.pkts.size(), 1u);
  if (!b.pkts.empty()) {
    CHECK_EQ(b.pkts[0]["seq"], 2);
    CHECK_EQ(b.pkts[0]["lat_ms"], 5.0);
  }
  CHECK_EQ(c->cursor, 3u);  // now held at seq 3
  // A second later the hold gives up on it: it is looked at, and has no latency to match.
  out.clear();
  lv.drain(*c, LiveView::mono_us() + LiveView::kLatHoldUs + 1, &out);
  b = split(out);
  CHECK(b.pkts.empty());
  CHECK_EQ(c->cursor, 5u);
}

// The routes over a paced replay of the session: the stream subscribes after the connection and
// the channels were set up, so the seed is what makes proto:rtp match.
void test_routes() {
  const std::string path = tmp_path("live.btsnoop");
  // Two seconds of nothing first, so the stream is surely open before the session starts...
  // except for the connection, which comes first and is seen only by the Decoder.
  std::vector<Pkt> pkts = a2dp_session();
  for (auto& p : pkts) {
    if (p.ts >= kT0 + 1000000) p.ts += 1500000;
  }
  write_file(path, pkts);

  btb::hci::Options o;
  o.replay_file = path;
  o.capture_dir = g_tmp;
  o.systemctl = "/nonexistent/systemctl";
  auto mon = btb::hci::start(o, nullptr);
  httplib::Server svr;
  btb::hci::register_routes(svr, *mon);
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });
  svr.wait_until_ready();

  {
    httplib::Client cli("127.0.0.1", port);
    auto r = cli.Get("/api/hci/live?filter=type:foo");
    CHECK(r && r->status == 400);
    r = cli.Get("/api/hci/live/1");
    CHECK(r && r->status == 404);
  }
  // Let the replay get past the setup (the Decoder sees it, the live view does not: nobody
  // watches yet).
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  std::string got;
  std::atomic<bool> done{false};
  std::thread reader([&] {
    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(5, 0);
    const auto t0 = std::chrono::steady_clock::now();
    cli.Get("/api/hci/live?filter=" + httplib::encode_uri_component("proto:rtp"),
            [&](const char* d, size_t n) {
              got.append(d, n);
              size_t lines = 0;
              for (char ch : got) lines += ch == '\n';
              // 13 media packets and a status line or two.
              return lines < 14 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6);
            });
    done = true;
  });
  reader.join();
  const Batch b = split(got);
  CHECK(!b.status.empty());
  if (!b.status.empty()) CHECK_EQ(b.status[0]["filter"], "proto:rtp");
  CHECK_EQ(b.pkts.size(), 13u);
  if (!b.pkts.empty()) {
    CHECK_EQ(b.pkts[0]["psm"], kPsmAvdtp);
    CHECK_EQ(b.pkts[0]["lat_ms"].is_null() || b.pkts[0]["lat_ms"].is_number(), true);
    httplib::Client cli("127.0.0.1", port);
    const auto r = cli.Get("/api/hci/live/" + std::to_string(b.pkts[0]["seq"].get<uint64_t>()));
    CHECK(r && r->status == 200);
    if (r && r->status == 200) {
      const json d = json::parse(r->body);
      CHECK_EQ(d["proto"], "rtp");
      CHECK_EQ(d["lat_ms"], 2.0);  // known by now
      CHECK(d["summary"].get<std::string>().find("RTP seq 100") == 0);
    }
  }

  mon->stop();
  svr.stop();
  th.join();
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/btb-hci-live-XXXXXX";
  const char* tmp = mkdtemp(tmpl);
  if (!tmp) return 1;
  g_tmp = tmp;

  test_matches_capture_view();
  test_seeded_mid_stream();
  test_ring_and_detail();
  test_streams_and_drops();
  test_latency_hold();
  test_routes();

  if (std::system(("rm -rf '" + g_tmp + "'").c_str()) != 0) std::printf("cannot remove %s\n", g_tmp.c_str());
  return report("test_hci_live");
}
