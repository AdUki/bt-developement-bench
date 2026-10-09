// The decoder against scripted packet sequences: connection bookkeeping, L2CAP channel mapping,
// AVDTP state and codec decoding, RTP loss/jitter, NOCP latency, credits, windows and rings.

#include <cmath>
#include <string>

#include "../check.h"
#include "codec.h"
#include "decoder.h"
#include "histogram.h"
#include "packets.h"

using namespace hcitest;
using btb::hci::CodecInfo;
using btb::hci::Decoder;
using btb::hci::LatencyHist;
using nlohmann::json;

namespace {

const json* find_conn(const json& stats, int handle) {
  for (const auto& c : stats["conns"]) {
    if (c["handle"] == handle) return &c;
  }
  return nullptr;
}

const json* find_chan(const json& conn, const std::string& name) {
  for (const auto& ch : conn["channels"]) {
    if (ch["name"] == name) return &ch;
  }
  return nullptr;
}

bool has_event(const json& evs, const std::string& kind, const std::string& needle) {
  for (const auto& e : evs) {
    if (e["kind"] == kind && e["text"].get<std::string>().find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void test_histogram() {
  LatencyHist h;
  CHECK_EQ(h.count(), 0u);
  CHECK_NEAR(h.percentile_ms(0.5), 0.0, 1e-9);
  for (int ms = 1; ms <= 100; ++ms) h.add_us(ms * 1000 + 300);  // 1.3 .. 100.3 ms
  CHECK_EQ(h.count(), 100u);
  CHECK_NEAR(h.min_ms(), 1.3, 1e-9);
  CHECK_NEAR(h.max_ms(), 100.3, 1e-9);
  CHECK_NEAR(h.avg_ms(), 50.8, 1e-9);
  // rank 50 → 50.3 ms → bucket [50,51) → 50.5
  CHECK_NEAR(h.percentile_ms(0.50), 50.5, 1e-9);
  CHECK_NEAR(h.percentile_ms(0.95), 95.5, 1e-9);
  // the top bucket's midpoint (100.5) is above the exact max: clamped
  CHECK_NEAR(h.percentile_ms(1.0), 100.3, 1e-9);

  // One sample: every percentile is that sample, not a bucket midpoint.
  LatencyHist one;
  one.add_us(7200);
  CHECK_NEAR(one.percentile_ms(0.5), 7.2, 1e-9);
  CHECK_NEAR(one.percentile_ms(0.95), 7.2, 1e-9);

  // Overflow bucket reports the exact max.
  LatencyHist big;
  big.add_us(1000);
  big.add_us(800000);
  CHECK_EQ(big.counts()[LatencyHist::kBuckets], 1u);
  CHECK_NEAR(big.percentile_ms(0.95), 800.0, 1e-9);
  big.clear();
  CHECK_EQ(big.count(), 0u);
  CHECK_EQ(big.counts()[1], 0u);
}

void test_codecs() {
  CodecInfo c;
  const Bytes sbc = sbc_cap();
  CHECK(btb::hci::decode_media_codec(sbc.data() + 2, sbc.size() - 2, &c));
  CHECK_EQ(c.name, std::string("SBC"));
  CHECK_EQ(c.config,
           std::string("48000 Hz, joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2..53"));
  CHECK_EQ(c.rate, 48000u);
  CHECK(c.sbc && c.rtp);

  // A capability: several bits per field, no single rate.
  const Bytes caps = sbc_cap(0x3f, 0xff, 2, 53);
  CHECK(btb::hci::decode_media_codec(caps.data() + 2, caps.size() - 2, &c));
  CHECK_EQ(c.config, std::string("44100/48000 Hz, mono/dual channel/stereo/joint stereo, "
                                 "4/8/12/16 blocks, 4/8 subbands, SNR/loudness, bitpool 2..53"));
  CHECK_EQ(c.rate, 0u);

  // AAC: MPEG-2 AAC LC, 44100, 2 channels, VBR 320 kbps
  const Bytes aac{0x00, 0x02, 0x80, 0x01, 0x04, 0x84, 0xe2, 0x00};
  CHECK(btb::hci::decode_media_codec(aac.data(), aac.size(), &c));
  CHECK_EQ(c.name, std::string("AAC"));
  CHECK_EQ(c.config, std::string("MPEG-2 AAC LC, 44100 Hz, 2 ch, 320 kbps VBR"));
  CHECK_EQ(c.rate, 44100u);

  // aptX: no RTP
  const Bytes aptx{0x00, 0xff, 0x4f, 0, 0, 0, 0x01, 0x00, 0x12};
  CHECK(btb::hci::decode_media_codec(aptx.data(), aptx.size(), &c));
  CHECK_EQ(c.name, std::string("aptX"));
  CHECK_EQ(c.config, std::string("48000 Hz, stereo"));
  CHECK(!c.rtp);

  // aptX HD: RTP
  const Bytes aptxhd{0x00, 0xff, 0xd7, 0, 0, 0, 0x24, 0x00, 0x22, 0, 0, 0, 0};
  CHECK(btb::hci::decode_media_codec(aptxhd.data(), aptxhd.size(), &c));
  CHECK_EQ(c.name, std::string("aptX HD"));
  CHECK_EQ(c.config, std::string("44100 Hz, stereo"));
  CHECK(c.rtp);

  const Bytes ldac{0x00, 0xff, 0x2d, 0x01, 0, 0, 0xaa, 0x00, 0x04, 0x01};
  CHECK(btb::hci::decode_media_codec(ldac.data(), ldac.size(), &c));
  CHECK_EQ(c.name, std::string("LDAC"));
  CHECK_EQ(c.config, std::string("96000 Hz, stereo"));
  CHECK_EQ(c.rate, 96000u);

  const Bytes other{0x00, 0xff, 0x34, 0x12, 0, 0, 0x01, 0x00};
  CHECK(btb::hci::decode_media_codec(other.data(), other.size(), &c));
  CHECK_EQ(c.name, std::string("vendor 0x00001234:0x0001"));

  CHECK_EQ(std::string(btb::hci::avdtp_signal_name(0x0d)), std::string("DELAY_REPORT"));
}

// RFC 3550 jitter recurrence, for the expected value.
double expected_jitter_ms() {
  struct P {
    int64_t at;
    uint32_t ts;
    bool after_gap;
  };
  std::vector<P> ps;
  uint16_t seq = 100;
  uint32_t rts = 5000;
  for (int i = 0; i < 10; ++i) {
    bool gap = false;
    if (seq == 105) {
      ++seq;
      rts += 896;
      gap = true;
    }
    ps.push_back({1000000 + i * 20000, rts, gap});
    ++seq;
    rts += 896;
  }
  for (int i = 0; i < 3; ++i) {
    ps.push_back({1900000 + i * 1000, rts, false});
    rts += 896;
  }
  double j = 0;
  for (size_t i = 1; i < ps.size(); ++i) {
    if (ps[i].after_gap) continue;  // not consecutive: no jitter sample
    const double d = (ps[i].at - ps[i - 1].at) * 48000.0 / 1e6 -
                     static_cast<double>(static_cast<int32_t>(ps[i].ts - ps[i - 1].ts));
    j += (std::fabs(d) - j) / 16.0;
  }
  return j * 1000.0 / 48000.0;
}

void test_a2dp_session() {
  Decoder d;
  const auto pkts = a2dp_session();
  // Up to the first media packet: second 0 only.
  size_t i = 0;
  for (; i < pkts.size() && pkts[i].ts < kT0 + 1000000; ++i) {
    d.packet(pkts[i].ts, pkts[i].index, pkts[i].opcode, pkts[i].data.data(), pkts[i].data.size());
  }
  CHECK_EQ(d.windows_closed(), 0u);

  const json evs = d.events(0);
  CHECK(has_event(evs, "index", "hci0 added"));
  CHECK(has_event(evs, "conn", "acl handle 11 to AA:BB:CC:DD:EE:FF"));
  CHECK(has_event(evs, "l2cap", "AVDTP signalling open (PSM 0x0019, scid 0x0040, dcid 0x0050)"));
  CHECK(has_event(evs, "avdtp", "RX DISCOVER accept: 1 audio sink"));
  CHECK(has_event(evs, "avdtp", "RX GET_CAPABILITIES accept: SBC 16000/32000/44100/48000 Hz"));
  // Decoded although it came in two ACL fragments.
  CHECK(has_event(evs, "avdtp",
                  "TX SET_CONFIGURATION cmd acp 1 int 3: SBC 48000 Hz, joint stereo, 16 blocks, "
                  "8 subbands, loudness, bitpool 2..53, delay reporting"));
  CHECK(has_event(evs, "avdtp", "RX SET_CONFIGURATION accept: SBC 48000 Hz"));
  CHECK(has_event(evs, "l2cap", "AVDTP media open (PSM 0x0019, scid 0x0041, dcid 0x0051) for SEID 3/1"));
  CHECK(has_event(evs, "avdtp", "RX START accept"));

  // Second 1 (media) and the start of second 2.
  for (; i < pkts.size() && pkts[i].ts < kT0 + 2000000; ++i) {
    d.packet(pkts[i].ts, pkts[i].index, pkts[i].opcode, pkts[i].data.data(), pkts[i].data.size());
  }
  // Window 0 closed when the first media packet arrived.
  CHECK_EQ(d.windows_closed(), 1u);
  {
    const json& s = d.stats();
    CHECK_EQ(s["ts"].get<int64_t>(), (kT0 + 1000000) / 1000);
    CHECK_EQ(s["adapters"].size(), 1u);
    CHECK_EQ(s["adapters"][0]["acl_mtu"], 1021);
    CHECK_EQ(s["adapters"][0]["acl_pkts"], 8);
    const json* c = find_conn(s, kHandle);
    CHECK(c != nullptr);
    if (c) {
      CHECK_EQ((*c)["type"], "acl");
      CHECK_EQ((*c)["peer"], "AA:BB:CC:DD:EE:FF");
      CHECK_EQ((*c)["in_flight"], 0);
      CHECK_EQ((*c)["credits"], 8);
      CHECK_EQ((*c)["lat_ms"]["n"], 8);
      CHECK_EQ((*c)["nocp_unmatched"], 0);
      const json* sig = find_chan(*c, "AVDTP signalling");
      CHECK(sig != nullptr);
      if (sig) {
        CHECK_EQ((*sig)["scid"], kSigLcid);
        CHECK_EQ((*sig)["dcid"], kSigRcid);
        CHECK_EQ((*sig)["psm"], 25);
        CHECK_EQ((*sig)["avdtp"]["streams"].size(), 1u);
        CHECK_EQ((*sig)["avdtp"]["streams"][0]["state"], "streaming");
        CHECK_EQ((*sig)["avdtp"]["streams"][0]["lseid"], 3);
        CHECK_EQ((*sig)["avdtp"]["streams"][0]["rseid"], 1);
      }
      CHECK(find_chan(*c, "L2CAP signalling") != nullptr);
    }
  }

  // Close window 1 (media second).
  d.advance(kT0 + 2000000);
  CHECK_EQ(d.windows_closed(), 2u);
  {
    const json* c = find_conn(d.stats(), kHandle);
    CHECK(c != nullptr);
    if (c) {
      const uint64_t bytes = 13 * (4 + 12 + 1 + 600);
      CHECK_EQ((*c)["tx_bps"].get<uint64_t>(), bytes * 8);
      CHECK_EQ((*c)["tx_pps"], 13);
      CHECK_EQ((*c)["rx_bps"], 0);
      // 3 packets sent at +1.9 s are still in the controller at the window's end.
      CHECK_EQ((*c)["in_flight"], 3);
      CHECK_EQ((*c)["in_flight_max"], 3);
      CHECK_EQ((*c)["credits"], 5);
      CHECK_EQ((*c)["credits_min"], 5);
      CHECK_EQ((*c)["pool"], "acl");
      // NOCP latencies 2..11 ms
      const json& lat = (*c)["lat_ms"];
      CHECK_EQ(lat["n"], 10);
      CHECK_NEAR(lat["min"].get<double>(), 2.0, 1e-9);
      CHECK_NEAR(lat["max"].get<double>(), 11.0, 1e-9);
      CHECK_NEAR(lat["avg"].get<double>(), 6.5, 1e-9);
      CHECK_NEAR(lat["p50"].get<double>(), 6.5, 1e-9);
      CHECK_NEAR(lat["p95"].get<double>(), 11.0, 1e-9);

      const json* media = find_chan(*c, "AVDTP media");
      CHECK(media != nullptr);
      if (media) {
        CHECK_EQ((*media)["scid"], kMediaLcid);
        CHECK_EQ((*media)["dcid"], kMediaRcid);
        CHECK_EQ((*media)["tx_bps"].get<uint64_t>(), bytes * 8);
        CHECK_EQ((*media)["lat_ms"]["n"], 10);
        const json& av = (*media)["avdtp"];
        CHECK_EQ(av["role"], "media");
        CHECK_EQ(av["codec"], "SBC");
        CHECK_EQ(av["config"],
                 "48000 Hz, joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2..53");
        CHECK_EQ(av["state"], "streaming");
        CHECK_EQ(av["rtp_pkts"], 13);
        CHECK_EQ(av["rtp_lost"], 1);
        CHECK_EQ(av["rtp_lost_total"], 1);
        CHECK_NEAR(av["frames_per_packet"].get<double>(), 7.0, 1e-9);
        CHECK_NEAR(av["rtp_jitter_ms"].get<double>(), expected_jitter_ms(), 0.006);
        CHECK(av["rtp_jitter_ms"].get<double>() > 0.0);
      }
    }
  }

  // Second 2: the in-flight packets complete (118..120 ms), then the link drops.
  for (; i < pkts.size(); ++i) {
    d.packet(pkts[i].ts, pkts[i].index, pkts[i].opcode, pkts[i].data.data(), pkts[i].data.size());
  }
  // Since-connect latency: 8 signalling + 10 + 3 media completions.
  json lat;
  CHECK(d.latency(0, kHandle, &lat));
  CHECK_EQ(lat["bucket_ms"], 1);
  CHECK_EQ(lat["counts"].size(), 500u);
  CHECK_EQ(lat["lat_ms"]["n"], 21);
  CHECK_EQ(lat["counts"][118], 1);
  CHECK_EQ(lat["counts"][120], 1);
  CHECK_EQ(lat["overflow"], 0);
  uint64_t sum = 0;
  for (const auto& n : lat["counts"]) sum += n.get<uint64_t>();
  CHECK_EQ(sum, 21u);
  json hist;
  CHECK(d.history(0, kHandle, 300, &hist));
  CHECK_EQ(hist["t"].size(), 2u);
  CHECK_EQ(hist["in_flight"][1], 3);
  CHECK_EQ(hist["credits_min"][1], 5);
  CHECK(hist["lat_p50"][1].is_number());
  CHECK_EQ(hist["tx_bps"][1].get<uint64_t>(), 13u * (4 + 12 + 1 + 600) * 8);

  d.advance(kT0 + 3000000);
  {
    const json* c = find_conn(d.stats(), kHandle);
    CHECK(c != nullptr);  // reported for the window it disconnected in...
    if (c) {
      CHECK_EQ((*c)["connected"], false);
      CHECK_EQ((*c)["in_flight"], 0);
      CHECK_EQ((*c)["lat_ms"]["n"], 3);
      CHECK_NEAR((*c)["lat_ms"]["min"].get<double>(), 118.0, 1e-9);
      CHECK_NEAR((*c)["lat_ms"]["max"].get<double>(), 120.0, 1e-9);
    }
    CHECK(has_event(d.events(0), "disconn",
                    "acl handle 11 to AA:BB:CC:DD:EE:FF disconnected (reason 0x13)"));
  }
  // ...and gone after it; its events stay.
  CHECK(!d.latency(0, kHandle, &lat));
  CHECK(!d.history(0, kHandle, 300, &hist));
  d.advance(kT0 + 4000000);
  CHECK(find_conn(d.stats(), kHandle) == nullptr);
  CHECK(has_event(d.events(0), "conn", "acl handle 11"));

  // events since a sequence number, and the publish queue
  const json all = d.events(0);
  const uint64_t mid = all[all.size() / 2]["seq"].get<uint64_t>();
  CHECK_EQ(d.events(mid).size(), all.size() - all.size() / 2 - 1);
  CHECK_EQ(d.take_new_events().size(), all.size());
  CHECK_EQ(d.take_new_events().size(), 0u);
}

void test_le_att() {
  Decoder d;
  int64_t t = kT0;
  auto pk = [&](uint16_t op, const Bytes& b) { d.packet(t, 0, op, b.data(), b.size()); };
  pk(kMonNewIndex, new_index("hci0"));
  pk(kMonEvent, cmd_complete(kCmdReadBufferSize, Bytes{0, 0xfd, 0x03, 64, 8, 0, 1, 0}));
  // LE Read Buffer Size v2: LE 251 x 6, ISO 0
  pk(kMonEvent, cmd_complete(kCmdLeReadBufferSizeV2, Bytes{0, 251, 0, 6, 0, 0, 0}));
  // LE Enhanced Connection Complete v1: status, handle 64, role peripheral, random peer
  Bytes p{0};
  add16(p, 64);
  p.push_back(1);
  p.push_back(1);
  p = cat(p, kLePeer);
  p.resize(p.size() + 12 + 7, 0);
  pk(kMonEvent, le_meta(kLeEnhConnComplete, p));

  // ATT read request TX / response RX, completed 7.5 ms later
  t += 1000;
  pk(kMonAclTx, acl(64, 0, l2cap(kCidAtt, Bytes{0x0a, 0x03, 0x00})));
  t += 7500;
  pk(kMonEvent, nocp(64, 1));
  t += 1000;
  pk(kMonAclRx, acl(64, 2, l2cap(kCidAtt, Bytes{0x0b, 'h', 'i'})));
  // SMP on CID 6
  pk(kMonAclRx, acl(64, 2, l2cap(kCidSmp, Bytes{0x0b, 0x01})));

  // EATT: ECRED request for two channels; the peer accepts one (0x60) and refuses the other.
  {
    Bytes req;
    add16(req, kPsmEatt);
    add16(req, 64);   // mtu
    add16(req, 64);   // mps
    add16(req, 10);   // credits
    add16(req, 0x0040);
    add16(req, 0x0041);
    pk(kMonAclTx, acl(64, 0, l2cap(kCidLeSignaling, sig(kSigEcredConnReq, 7, req))));
    Bytes rsp;
    add16(rsp, 64);
    add16(rsp, 64);
    add16(rsp, 10);
    add16(rsp, 0x0004);  // some refused
    add16(rsp, 0x0060);
    add16(rsp, 0x0000);
    t += 1000;
    pk(kMonAclRx, acl(64, 2, l2cap(kCidLeSignaling, sig(kSigEcredConnRsp, 7, rsp))));
    // data on the EATT channel: TX uses the peer's CID
    pk(kMonAclTx, acl(64, 0, l2cap(0x0060, Bytes{0, 5, 0x0a, 0x03, 0x00})));
  }
  d.advance(kT0 + 1000000);

  const json evs = d.events(0);
  CHECK(has_event(evs, "conn", "le handle 64 to C1:22:33:44:55:66 (random)"));
  CHECK(has_event(evs, "l2cap", "EATT open (PSM 0x0027, scid 0x0040, dcid 0x0060)"));
  CHECK(has_event(evs, "l2cap", "EATT refused, result 0x0004 (PSM 0x0027, scid 0x0041"));

  const json* c = find_conn(d.stats(), 64);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ((*c)["type"], "le");
  CHECK_EQ((*c)["role"], "peripheral");
  CHECK_EQ((*c)["peer_type"], "random");
  CHECK_EQ((*c)["pool"], "le");
  // ATT was completed; the ECRED request and the EATT data packet are still in flight.
  CHECK_EQ((*c)["credits"], 4);
  CHECK_EQ((*c)["in_flight"], 2);
  CHECK_EQ((*c)["lat_ms"]["n"], 1);
  CHECK_NEAR((*c)["lat_ms"]["p50"].get<double>(), 7.5, 1e-9);
  const json* att = find_chan(*c, "ATT");
  CHECK(att != nullptr);
  if (att) {
    CHECK_EQ((*att)["tx_bps"], (4 + 3) * 8);
    CHECK_EQ((*att)["rx_bps"], (4 + 3) * 8);
    CHECK_NEAR((*att)["lat_ms"]["p50"].get<double>(), 7.5, 1e-9);
  }
  CHECK(find_chan(*c, "SMP") != nullptr);
  const json* eatt = find_chan(*c, "EATT");
  CHECK(eatt != nullptr);
  if (eatt) CHECK_EQ((*eatt)["tx_pps"], 1);
  // The refused channel is reported once (closed) and then removed.
  d.advance(kT0 + 2000000);
  c = find_conn(d.stats(), 64);
  int eatt_n = 0;
  if (c) {
    for (const auto& ch : (*c)["channels"]) eatt_n += ch["name"] == "EATT";
  }
  CHECK_EQ(eatt_n, 1);
}

// A trace that starts mid-connection: links and channels are created from their data.
void test_mid_trace() {
  Decoder d;
  int64_t t = kT0;
  const Bytes a = acl(5, 2, l2cap(0x0047, Bytes{1, 2, 3}));
  d.packet(t, 0, kMonAclRx, a.data(), a.size());
  const Bytes b = acl(5, 0, l2cap(0x0099, Bytes{1, 2, 3, 4}));
  d.packet(t, 0, kMonAclTx, b.data(), b.size());
  // NOCP for more than was seen: clamped, counted as unmatched
  const Bytes n = nocp(5, 3);
  d.packet(t + 1000, 0, kMonEvent, n.data(), n.size());
  d.advance(kT0 + 1000000);
  CHECK(has_event(d.events(0), "conn", "already connected when the trace started"));
  const json* c = find_conn(d.stats(), 5);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ((*c)["in_flight"], 0);
  CHECK_EQ((*c)["nocp_unmatched"], 2);
  CHECK(find_chan(*c, "CID 0x0047") != nullptr);
  CHECK(find_chan(*c, "CID 0x0099") != nullptr);
  CHECK_EQ(d.stats()["adapters"][0]["name"], "hci0");
}

// BR/EDR channel close via Disconnection Response, and refusal.
void test_l2cap_close() {
  Decoder d;
  int64_t t = kT0;
  auto pk = [&](uint16_t op, const Bytes& b) { d.packet(t, 0, op, b.data(), b.size()); };
  pk(kMonEvent, conn_complete(7, kPeer));
  // The peer opens AVCTP to us: RX request with its CID, TX response with ours.
  pk(kMonAclRx, acl(7, 2, conn_req(3, kPsmAvctp, 0x0070)));
  pk(kMonAclTx, acl(7, 0, conn_rsp(3, 0x0045, 0x0070)));
  // Pending, then refused
  pk(kMonAclTx, acl(7, 0, conn_req(4, kPsmRfcomm, 0x0046)));
  pk(kMonAclRx, acl(7, 2, conn_rsp(4, 0x0000, 0x0046, 0x0001)));
  pk(kMonAclRx, acl(7, 2, conn_rsp(4, 0x0000, 0x0046, 0x0004)));
  // data on AVCTP both ways
  pk(kMonAclTx, acl(7, 0, l2cap(0x0070, Bytes{0x10, 0x11, 0x0e})));
  pk(kMonAclRx, acl(7, 2, l2cap(0x0045, Bytes{0x12, 0x11, 0x0e, 0x00})));
  // We disconnect it. The peer's Disconnection Response: DCID is the responder's (the peer's)
  // endpoint 0x0070, SCID ours 0x0045.
  t += 1000;
  pk(kMonAclRx, acl(7, 2, disconn_rsp(5, 0x0070, 0x0045)));
  const json evs = d.events(0);
  CHECK(has_event(evs, "l2cap", "AVCTP open (PSM 0x0017, scid 0x0045, dcid 0x0070)"));
  CHECK(has_event(evs, "l2cap", "RFCOMM refused, result 0x0004"));
  CHECK(has_event(evs, "l2cap", "AVCTP closed (PSM 0x0017, scid 0x0045, dcid 0x0070)"));
  d.advance(kT0 + 1000000);
  const json* c = find_conn(d.stats(), 7);
  CHECK(c != nullptr);
  if (!c) return;
  const json* avctp = find_chan(*c, "AVCTP");
  CHECK(avctp != nullptr);
  if (avctp) {
    CHECK_EQ((*avctp)["open"], false);
    CHECK_EQ((*avctp)["tx_bps"], 7 * 8);
    CHECK_EQ((*avctp)["rx_bps"], 8 * 8);
  }
  d.advance(kT0 + 2000000);
  c = find_conn(d.stats(), 7);
  CHECK(c && find_chan(*c, "AVCTP") == nullptr);
}

void test_history_ring() {
  Decoder d;
  const Bytes cc = conn_complete(3, kPeer);
  d.packet(kT0, 0, kMonEvent, cc.data(), cc.size());
  // 310 one-second windows, window k carries k+1 bytes of ACL payload.
  for (int k = 0; k < 310; ++k) {
    Bytes payload(static_cast<size_t>(k + 1), 0);
    const Bytes a = acl(3, 2, payload);
    d.packet(kT0 + k * 1000000LL + 500000, 0, kMonAclRx, a.data(), a.size());
  }
  d.advance(kT0 + 310 * 1000000LL);
  json h;
  CHECK(d.history(0, 3, 300, &h));
  CHECK_EQ(h["t"].size(), 300u);
  CHECK_EQ(h["t"][299].get<int64_t>(), (kT0 + 310 * 1000000LL) / 1000);
  CHECK_EQ(h["t"][0].get<int64_t>(), (kT0 + 11 * 1000000LL) / 1000);
  CHECK_EQ(h["rx_bps"][299], 310 * 8);
  CHECK_EQ(h["rx_bps"][0], 11 * 8);
  CHECK(h["lat_p50"][0].is_null());  // nothing completed
  CHECK(d.history(0, 3, 10, &h));
  CHECK_EQ(h["t"].size(), 10u);
  CHECK_EQ(h["rx_bps"][9], 310 * 8);

  // A long idle gap closes at most kHistorySize windows and re-anchors.
  d.advance(kT0 + 5000 * 1000000LL + 300000);
  CHECK(d.history(0, 3, 300, &h));
  CHECK_EQ(h["t"].size(), 300u);
  CHECK_EQ(h["rx_bps"][299], 0);
}

void test_event_ring() {
  Decoder d;
  for (int i = 0; i < 520; ++i) d.add_mark(kT0 + i, "m" + std::to_string(i));
  const json all = d.events(0);
  CHECK_EQ(all.size(), 500u);
  CHECK_EQ(all[0]["text"], "m20");
  CHECK_EQ(all[499]["text"], "m519");
  CHECK_EQ(all[499]["seq"], 520);
  CHECK_EQ(d.events(515).size(), 5u);
  CHECK(all[0]["handle"].is_null());

  // The kernel's echo of our own mark is not recorded twice; a mark from a capture is.
  Bytes ul{6, 8};
  for (char ch : std::string("btbench")) ul.push_back(static_cast<uint8_t>(ch));
  ul.push_back(0);
  for (char ch : std::string("m519")) ul.push_back(static_cast<uint8_t>(ch));
  ul.push_back(0);
  d.packet(kT0 + 600, 0xffff, kMonUserLogging, ul.data(), ul.size());
  CHECK_EQ(d.events(519).size(), 1u);
  d.packet(kT0 + 10000000, 0xffff, kMonUserLogging, ul.data(), ul.size());
  CHECK_EQ(d.events(519).size(), 2u);
  // bluetoothd's own log lines are not events
  Bytes bt{6, 12};
  for (char ch : std::string("bluetoothd")) bt.push_back(static_cast<uint8_t>(ch));
  bt.push_back(0);
  bt.push_back(0);
  bt.push_back('x');
  bt.push_back(0);
  d.packet(kT0 + 10000001, 0xffff, kMonUserLogging, bt.data(), bt.size());
  CHECK_EQ(d.events(519).size(), 2u);
}

// SCO without flow control queues nothing; ISO uses NOCP like ACL.
void test_sco_iso() {
  Decoder d;
  int64_t t = kT0;
  auto pk = [&](uint16_t op, const Bytes& b) { d.packet(t, 0, op, b.data(), b.size()); };
  pk(kMonEvent, cmd_complete(kCmdLeReadBufferSizeV2, Bytes{0, 251, 0, 6, 120, 0, 4}));
  Bytes sc{0};
  add16(sc, 0x0101);
  sc = cat(sc, kPeer);
  sc.push_back(0x02);
  sc.resize(sc.size() + 7, 0);
  pk(kMonEvent, evt(kEvtSyncConnComplete, sc));
  for (int i = 0; i < 200; ++i) {
    Bytes s;
    add16(s, 0x0101);
    s.push_back(60);
    s.resize(s.size() + 60, 0);
    pk(kMonScoTx, s);
  }
  // CIS 0x60 on a central's LE Create CIS paired with ACL 0x40
  pk(kMonEvent, le_meta(kLeConnComplete, cat(Bytes{0, 0x40, 0, 0, 0}, cat(kLePeer, Bytes(6, 0)))));
  Bytes cis_cmd;
  add16(cis_cmd, kCmdLeCreateCis);
  cis_cmd.push_back(5);
  cis_cmd.push_back(1);
  add16(cis_cmd, 0x60);
  add16(cis_cmd, 0x40);
  pk(kMonCommand, cis_cmd);
  Bytes est{0};
  add16(est, 0x60);
  est.resize(28, 0);
  pk(kMonEvent, le_meta(kLeCisEstablished, est));
  Bytes iso;
  add16(iso, 0x60 | (2 << 12));
  add16(iso, 104);
  iso.resize(4 + 104, 0);
  pk(kMonIsoTx, iso);
  pk(kMonIsoTx, iso);
  t += 4000;
  pk(kMonEvent, nocp(0x60, 2));
  d.advance(kT0 + 1000000);

  const json* sco = find_conn(d.stats(), 0x0101);
  CHECK(sco != nullptr);
  if (sco) {
    CHECK_EQ((*sco)["type"], "esco");
    CHECK_EQ((*sco)["pool"], "none");
    CHECK_EQ((*sco)["in_flight"], 0);
    CHECK_EQ((*sco)["fifo_overflow"], 0);
    CHECK_EQ((*sco)["tx_bps"], 200 * 60 * 8);
    CHECK((*sco)["credits"].is_null());
  }
  const json* cis = find_conn(d.stats(), 0x60);
  CHECK(cis != nullptr);
  if (cis) {
    CHECK_EQ((*cis)["type"], "cis");
    CHECK_EQ((*cis)["peer"], "C1:22:33:44:55:66");
    CHECK_EQ((*cis)["pool"], "iso");
    CHECK_EQ((*cis)["credits"], 4);
    CHECK_EQ((*cis)["credits_min"], 2);
    CHECK_EQ((*cis)["lat_ms"]["n"], 2);
    CHECK_NEAR((*cis)["lat_ms"]["max"].get<double>(), 4.0, 1e-9);
    CHECK_EQ((*cis)["tx_bps"], 2 * 104 * 8);
  }
  CHECK_EQ(d.stats()["adapters"][0]["iso_pkts"], 4);
}

}  // namespace

int main() {
  test_histogram();
  test_codecs();
  test_a2dp_session();
  test_le_att();
  test_mid_trace();
  test_l2cap_close();
  test_history_ring();
  test_event_ring();
  test_sco_iso();
  return report("test_hci_decode");
}
