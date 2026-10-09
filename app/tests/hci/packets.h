#pragma once

// Builders for synthetic monitor-channel packets, and a scripted A2DP session the decode and
// replay tests share.

#include <cstdint>
#include <string>
#include <vector>

#include "wire.h"

namespace hcitest {

using Bytes = std::vector<uint8_t>;
using namespace btb::hci;

struct Pkt {
  int64_t ts;
  uint16_t index;
  uint16_t opcode;
  Bytes data;
};

inline void add16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
inline void add16be(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}
inline void add32be(Bytes& b, uint32_t v) {
  add16be(b, static_cast<uint16_t>(v >> 16));
  add16be(b, static_cast<uint16_t>(v));
}
inline Bytes cat(Bytes a, const Bytes& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// AA:BB:CC:DD:EE:FF on the wire (little-endian)
inline const Bytes kPeer = {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa};
inline const Bytes kLePeer = {0x66, 0x55, 0x44, 0x33, 0x22, 0xc1};

inline Bytes evt(uint8_t code, const Bytes& params) {
  Bytes b{code, static_cast<uint8_t>(params.size())};
  return cat(b, params);
}
inline Bytes le_meta(uint8_t sub, const Bytes& params) {
  return evt(kEvtLeMeta, cat(Bytes{sub}, params));
}
inline Bytes cmd_complete(uint16_t opcode, const Bytes& ret) {
  Bytes p{1};
  add16(p, opcode);
  return evt(kEvtCmdComplete, cat(p, ret));
}
inline Bytes conn_complete(uint16_t handle, const Bytes& peer, uint8_t link_type = 1) {
  Bytes p{0};
  add16(p, handle);
  p = cat(p, peer);
  p.push_back(link_type);
  p.push_back(0);
  return evt(kEvtConnComplete, p);
}
inline Bytes disconn_complete(uint16_t handle, uint8_t reason = 0x13) {
  Bytes p{0};
  add16(p, handle);
  p.push_back(reason);
  return evt(kEvtDisconnComplete, p);
}
inline Bytes nocp(uint16_t handle, uint16_t count) {
  Bytes p{1};
  add16(p, handle);
  add16(p, count);
  return evt(kEvtNumCompletedPackets, p);
}
inline Bytes new_index(const char* name) {
  Bytes p{0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  bool end = false;
  for (int i = 0; i < 8; ++i) {
    end = end || !name[i];
    p.push_back(end ? 0 : static_cast<uint8_t>(name[i]));
  }
  return p;
}

// ACL packet: handle + PB flags, length, payload.
inline Bytes acl(uint16_t handle, uint8_t pb, const Bytes& payload) {
  Bytes b;
  add16(b, static_cast<uint16_t>(handle | (pb << 12)));
  add16(b, static_cast<uint16_t>(payload.size()));
  return cat(b, payload);
}
// L2CAP basic frame.
inline Bytes l2cap(uint16_t cid, const Bytes& payload) {
  Bytes b;
  add16(b, static_cast<uint16_t>(payload.size()));
  add16(b, cid);
  return cat(b, payload);
}
inline Bytes sig(uint8_t code, uint8_t ident, const Bytes& params) {
  Bytes b{code, ident};
  add16(b, static_cast<uint16_t>(params.size()));
  return cat(b, params);
}
inline Bytes conn_req(uint8_t ident, uint16_t psm, uint16_t scid) {
  Bytes p;
  add16(p, psm);
  add16(p, scid);
  return l2cap(kCidSignaling, sig(kSigConnReq, ident, p));
}
inline Bytes conn_rsp(uint8_t ident, uint16_t dcid, uint16_t scid, uint16_t result = 0) {
  Bytes p;
  add16(p, dcid);
  add16(p, scid);
  add16(p, result);
  add16(p, 0);
  return l2cap(kCidSignaling, sig(kSigConnRsp, ident, p));
}
inline Bytes disconn_rsp(uint8_t ident, uint16_t dcid, uint16_t scid) {
  Bytes p;
  add16(p, dcid);
  add16(p, scid);
  return l2cap(kCidSignaling, sig(kSigDisconnRsp, ident, p));
}

// AVDTP single-packet message.
inline Bytes avdtp(uint8_t label, uint8_t mtype, uint8_t signal, const Bytes& params) {
  Bytes b{static_cast<uint8_t>((label << 4) | mtype), signal};
  return cat(b, params);
}
// SBC Media Codec capability: 48 kHz joint stereo, 16 blocks, 8 subbands, loudness, 2..53.
inline Bytes sbc_cap(uint8_t b0 = 0x11, uint8_t b1 = 0x15, uint8_t minbp = 2, uint8_t maxbp = 53) {
  return Bytes{0x07, 0x06, 0x00, 0x00, b0, b1, minbp, maxbp};
}

inline Bytes rtp(uint16_t seq, uint32_t ts, uint8_t frames, size_t payload = 600) {
  Bytes b{0x80, 0x60};
  add16be(b, seq);
  add32be(b, ts);
  add32be(b, 0x11223344);
  b.push_back(frames);
  b.resize(b.size() + payload, 0x9c);
  return b;
}

constexpr int64_t kT0 = 1700000000LL * 1000000;  // a whole second, µs
constexpr uint16_t kHandle = 11;
constexpr uint16_t kSigLcid = 0x0040, kSigRcid = 0x0050;
constexpr uint16_t kMediaLcid = 0x0041, kMediaRcid = 0x0051;

// A complete A2DP source session, as seen by btmon on the source:
//   second 0: controller init, ACL connect, AVDTP signalling channel, DISCOVER,
//             GET_CAPABILITIES, SET_CONFIGURATION (fragmented over two ACL packets), OPEN,
//             media channel, START
//   second 1: 10 RTP media packets (seq 100..110 with 105 missing), each completed by NOCP
//             2..11 ms later (latencies 2,3,...,11 ms), then 3 more packets left in flight
//   second 2: one NOCP (count 3) for the packets in flight, then the disconnection
inline std::vector<Pkt> a2dp_session() {
  std::vector<Pkt> v;
  int64_t t = kT0;
  auto ev = [&](const Bytes& d) { v.push_back({t, 0, kMonEvent, d}); };
  auto tx = [&](const Bytes& d) { v.push_back({t, 0, kMonAclTx, d}); };
  auto rx = [&](const Bytes& d) { v.push_back({t, 0, kMonAclRx, d}); };
  auto step = [&](int64_t us) { t += us; };

  v.push_back({t, 0, kMonNewIndex, new_index("hci0")});
  v.push_back({t, 0, kMonOpenIndex, {}});
  // Read Buffer Size: ACL 1021 x 8, SCO 64 x 1
  ev(cmd_complete(kCmdReadBufferSize, Bytes{0, 0xfd, 0x03, 64, 8, 0, 1, 0}));
  step(1000);
  ev(conn_complete(kHandle, kPeer));
  step(1000);

  // Signalling channel: we request, the peer accepts.
  tx(acl(kHandle, 0, conn_req(1, kPsmAvdtp, kSigLcid)));
  step(1000);
  rx(acl(kHandle, 2, conn_rsp(1, kSigRcid, kSigLcid)));
  step(1000);

  // DISCOVER / accept: SEP 1, audio sink, not in use
  tx(acl(kHandle, 0, l2cap(kSigRcid, avdtp(0, 0, 0x01, {}))));
  step(1000);
  rx(acl(kHandle, 2, l2cap(kSigLcid, avdtp(0, 2, 0x01, Bytes{1 << 2, 0x08}))));
  step(1000);

  // GET_CAPABILITIES acp 1 / accept: media transport, SBC (everything), delay reporting
  tx(acl(kHandle, 0, l2cap(kSigRcid, avdtp(1, 0, 0x02, Bytes{1 << 2}))));
  step(1000);
  rx(acl(kHandle, 2,
         l2cap(kSigLcid, avdtp(1, 2, 0x02,
                               cat(cat(Bytes{0x01, 0x00}, sbc_cap(0xff, 0xff, 2, 53)),
                                   Bytes{0x08, 0x00})))));
  step(1000);

  // SET_CONFIGURATION acp 1 int 3, split over a start and a continuation fragment.
  {
    const Bytes body = l2cap(
        kSigRcid, avdtp(2, 0, 0x03,
                        cat(cat(Bytes{1 << 2, 3 << 2, 0x01, 0x00}, sbc_cap()), Bytes{0x08, 0x00})));
    const Bytes first(body.begin(), body.begin() + 9);
    const Bytes rest(body.begin() + 9, body.end());
    tx(acl(kHandle, 0, first));
    step(200);
    tx(acl(kHandle, 1, rest));
  }
  step(1000);
  rx(acl(kHandle, 2, l2cap(kSigLcid, avdtp(2, 2, 0x03, {}))));
  step(1000);

  // OPEN, then the transport channel
  tx(acl(kHandle, 0, l2cap(kSigRcid, avdtp(3, 0, 0x06, Bytes{1 << 2}))));
  step(1000);
  rx(acl(kHandle, 2, l2cap(kSigLcid, avdtp(3, 2, 0x06, {}))));
  step(1000);
  tx(acl(kHandle, 0, conn_req(2, kPsmAvdtp, kMediaLcid)));
  step(1000);
  rx(acl(kHandle, 2, conn_rsp(2, kMediaRcid, kMediaLcid)));
  step(1000);

  // START
  tx(acl(kHandle, 0, l2cap(kSigRcid, avdtp(4, 0, 0x07, Bytes{1 << 2}))));
  step(1000);
  rx(acl(kHandle, 2, l2cap(kSigLcid, avdtp(4, 2, 0x07, {}))));
  // The 8 signalling TX packets above (SET_CONFIGURATION took two) complete in bulk.
  step(1000);
  ev(nocp(kHandle, 8));

  // Second 1: media. 7 SBC frames of 128 samples = 896 samples = 18.666 ms at 48 kHz.
  t = kT0 + 1000000;
  uint16_t seq = 100;
  uint32_t rts = 5000;
  for (int i = 0; i < 10; ++i) {
    if (seq == 105) {  // lost before reaching HCI: a gap in the sequence
      ++seq;
      rts += 896;
    }
    const int64_t sent = kT0 + 1000000 + i * 20000;
    t = sent;
    tx(acl(kHandle, 0, l2cap(kMediaRcid, rtp(seq, rts, 7))));
    t = sent + (2 + i) * 1000;
    ev(nocp(kHandle, 1));
    ++seq;
    rts += 896;
  }
  // Three more, not completed within this second.
  for (int i = 0; i < 3; ++i) {
    t = kT0 + 1000000 + 900000 + i * 1000;
    tx(acl(kHandle, 0, l2cap(kMediaRcid, rtp(seq, rts, 7))));
    ++seq;
    rts += 896;
  }

  // Second 2: all three complete at +2.02 s, sent at +1.900/1.901/1.902 s: 120, 119, 118 ms.
  t = kT0 + 2000000 + 20000;
  ev(nocp(kHandle, 3));
  t = kT0 + 2000000 + 500000;
  ev(disconn_complete(kHandle));
  return v;
}

}  // namespace hcitest
