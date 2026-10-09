#include "decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "wire.h"

namespace btb::hci {

using nlohmann::json;

namespace {

enum LinkType { kAcl, kLe, kSco, kEsco, kCis, kBis };
const char* link_name(int t) {
  switch (t) {
    case kLe: return "le";
    case kSco: return "sco";
    case kEsco: return "esco";
    case kCis: return "cis";
    case kBis: return "bis";
    default: return "acl";
  }
}

// Which controller buffer pool a link's TX packets take credits from. LE shares the ACL pool when
// the controller reports no LE buffers; SCO is only flow-controlled (NOCP for SCO) when the host
// enabled Synchronous Flow Control — otherwise its completions never come and nothing is queued.
enum Pool { kPoolAcl, kPoolLe, kPoolSco, kPoolIso, kPoolNone };
const char* pool_name(int p) {
  switch (p) {
    case kPoolAcl: return "acl";
    case kPoolLe: return "le";
    case kPoolSco: return "sco";
    case kPoolIso: return "iso";
    default: return "none";
  }
}

enum Role { kRoleOther, kRoleSig, kRoleAvdtpSig, kRoleAvdtpMedia };

enum Dir { kTx = 0, kRx = 1 };

const char* peer_type_name(int t) {
  switch (t) {
    case 0: return "public";
    case 1: return "random";
    case 2: return "public-id";
    case 3: return "random-id";
    default: return nullptr;
  }
}

std::string hex4(uint16_t v) {
  char s[8];
  std::snprintf(s, sizeof(s), "0x%04x", v);
  return s;
}

std::string hex2(uint8_t v) {
  char s[6];
  std::snprintf(s, sizeof(s), "0x%02x", v);
  return s;
}

std::string psm_name(uint16_t psm, bool le) {
  if (le) {
    switch (psm) {
      case kPsmEatt: return "EATT";
      case 0x0025: return "OTS";
      default: return "LE PSM " + hex4(psm);
    }
  }
  switch (psm) {
    case kPsmSdp: return "SDP";
    case kPsmRfcomm: return "RFCOMM";
    case kPsmBnep: return "BNEP";
    case kPsmHidCtrl: return "HID control";
    case kPsmHidIntr: return "HID interrupt";
    case kPsmAvctp: return "AVCTP";
    case kPsmAvdtp: return "AVDTP";
    case kPsmAvctpBrowsing: return "AVCTP browsing";
    case kPsmAtt: return "ATT";
    case kPsmEatt: return "EATT";
    default: return "PSM " + hex4(psm);
  }
}

std::string fixed_cid_name(uint16_t cid) {
  switch (cid) {
    case kCidSignaling: return "L2CAP signalling";
    case kCidConnectionless: return "connectionless";
    case kCidAtt: return "ATT";
    case kCidLeSignaling: return "LE signalling";
    case kCidSmp: return "SMP";
    case kCidSmpBredr: return "SMP (BR/EDR)";
    default: return "CID " + hex4(cid);
  }
}

double round2(double v) { return std::round(v * 100.0) / 100.0; }

json lat_json(const LatencyHist& h) {
  return json{{"n", h.count()},
              {"min", round2(h.min_ms())},
              {"avg", round2(h.avg_ms())},
              {"p50", round2(h.percentile_ms(0.50))},
              {"p95", round2(h.percentile_ms(0.95))},
              {"max", round2(h.max_ms())}};
}

struct Counters {
  uint64_t bytes = 0;
  uint32_t pkts = 0;
  void add(uint32_t b) {
    bytes += b;
    ++pkts;
  }
  void clear() { *this = Counters{}; }
};

// RTP receive statistics as RFC 3550 §6.4.1/A.8 define them, per direction of one media channel.
struct Rtp {
  bool init = false;
  uint32_t ssrc = 0;
  uint8_t pt = 0;
  uint16_t last_seq = 0;
  uint32_t last_ts = 0;
  int64_t last_arrival_us = 0;
  double jitter = 0;  // RTP clock units
  uint32_t pkts_total = 0;
  uint32_t lost_total = 0;
  uint32_t pkts_win = 0;
  uint32_t lost_win = 0;
  uint32_t frames_sum_win = 0;
  uint32_t frames_pkts_win = 0;
  uint8_t frames_last = 0;
};

struct Stream {
  uint8_t lseid = 0;  // our SEP
  uint8_t rseid = 0;  // the peer's SEP
  bool has_codec = false;
  CodecInfo codec;
  const char* state = "configured";
  uint32_t media_uid = 0;
  int delay_tenths_ms = -1;
};

struct AvdtpPending {
  bool valid = false;
  uint8_t signal = 0;
  uint8_t nseid = 0;
  uint8_t seids[4] = {};
  uint8_t int_seid = 0;
  bool has_codec = false;
  CodecInfo codec;
};

struct SigPending {
  bool valid = false;
  bool tx = false;
  uint8_t ident = 0;
  uint8_t n = 0;
  uint32_t uids[5] = {};
};

// Per-direction L2CAP PDU in progress on an ACL link: which channel the continuation fragments
// belong to, and — for signalling channels only — the bytes collected so far.
struct Frag {
  uint32_t uid = 0;
  uint32_t remaining = 0;
  bool collect = false;
  uint32_t have = 0;
  uint32_t want = 0;
  std::array<uint8_t, kSigBuf> buf;
};

struct TxEntry {
  int64_t ts_us;
  uint32_t uid;  // channel the packet belonged to, 0 = none
};

struct HistEntry {
  int64_t t_ms;
  float tx_bps;
  float rx_bps;
  float p50;  // NaN: no completions in that window
  float p95;
  uint16_t in_flight;  // the window's maximum
  int32_t credits_min;  // -1: unknown
};

}  // namespace

struct Decoder::Channel {
  uint32_t uid = 0;
  uint16_t lcid = 0;  // ours: what RX packets carry
  uint16_t rcid = 0;  // the peer's: what TX packets carry
  uint16_t psm = 0;
  bool le = false;
  bool fixed = false;
  bool open = false;
  bool closed = false;
  int role = kRoleOther;
  std::string name;
  Counters tx, rx;
  LatencyHist lat;
  Rtp rtp[2];
};

struct Decoder::Conn {
  uint16_t index = 0;
  uint16_t handle = 0;
  int type = kAcl;
  int pool = kPoolAcl;
  uint8_t peer[6] = {};
  bool peer_known = false;
  int peer_type = -1;
  int role = -1;  // LE: 0 central, 1 peripheral
  bool lazy = false;  // first seen by its data, not its connection event
  bool connected = true;
  int64_t since_us = 0;
  int big = -1;

  Counters tx, rx;
  LatencyHist lat;        // this window
  LatencyHist lat_total;  // since connect
  uint32_t in_flight = 0;
  uint32_t in_flight_max = 0;
  std::array<TxEntry, kTxFifo> fifo;
  size_t fifo_head = 0;
  size_t fifo_n = 0;
  uint32_t fifo_overflow = 0;
  uint32_t nocp_unmatched = 0;

  std::vector<std::unique_ptr<Channel>> chans;
  Frag frag[2];
  SigPending sigpend[8];
  size_t sigpend_next = 0;

  struct {
    uint32_t sig_uid = 0;
    std::vector<Stream> streams;
    AvdtpPending pend[2][16];
    int awaiting_lseid = -1;  // stream just OPENed: the next AVDTP channel is its transport
  } avdtp;

  std::array<HistEntry, kHistorySize> hist;
  size_t hist_head = 0;
  size_t hist_n = 0;

  Channel* find_uid(uint32_t uid) {
    if (!uid) return nullptr;
    for (auto& ch : chans) {
      if (ch->uid == uid) return ch.get();
    }
    return nullptr;
  }
  Stream* stream_by_lseid(int lseid) {
    for (auto& s : avdtp.streams) {
      if (s.lseid == lseid) return &s;
    }
    return nullptr;
  }
  // A command's ACP SEID names the receiver's SEP: the peer's when we sent it, ours otherwise.
  Stream* stream_for_cmd(bool cmd_tx, uint8_t acp) {
    for (auto& s : avdtp.streams) {
      if ((cmd_tx ? s.rseid : s.lseid) == acp) return &s;
    }
    return nullptr;
  }
};

struct Decoder::Adapter {
  uint16_t index = 0;
  std::string name;
  uint8_t addr[6] = {};
  bool addr_known = false;
  int bus = -1;
  int manufacturer = -1;
  bool up = false;
  uint16_t acl_mtu = 0, acl_pkts = 0, sco_mtu = 0, sco_pkts = 0;
  uint16_t le_mtu = 0, le_pkts = 0, iso_mtu = 0, iso_pkts = 0;
  bool sco_flow = false;
  int pending_sco_flow = -1;
  uint32_t outstanding[4] = {};
  uint32_t outstanding_max[4] = {};
  std::vector<std::pair<uint16_t, uint16_t>> cis_links;  // (cis handle, acl handle)
  std::vector<std::unique_ptr<Conn>> conns;

  int pool_total(int pool) const {
    switch (pool) {
      case kPoolAcl: return acl_pkts;
      case kPoolLe: return le_pkts;
      case kPoolSco: return sco_pkts;
      case kPoolIso: return iso_pkts;
      default: return 0;
    }
  }
  int pool_for(int type) const {
    switch (type) {
      case kAcl: return kPoolAcl;
      case kLe: return le_pkts ? kPoolLe : kPoolAcl;
      case kSco:
      case kEsco: return sco_flow ? kPoolSco : kPoolNone;
      default: return kPoolIso;
    }
  }
};

Decoder::Decoder() {
  events_.reserve(kEventRing);
  stats_ = json{{"ts", 0}, {"window_ms", 1000}, {"adapters", json::array()},
                {"conns", json::array()}};
}

Decoder::~Decoder() = default;

// ---------------------------------------------------------------------------------------------
// Events

Event& Decoder::emit(int64_t ts_us, int index, int handle, const char* kind, std::string text) {
  Event e;
  e.seq = next_seq_++;
  e.ts_ms = ts_us / 1000;
  e.index = index;
  e.handle = handle;
  e.kind = kind;
  e.text = std::move(text);
  if (events_.size() < kEventRing) {
    events_.push_back(std::move(e));
    return events_.back();
  }
  Event& slot = events_[events_head_];
  slot = std::move(e);
  events_head_ = (events_head_ + 1) % kEventRing;
  return slot;
}

static json event_json(const Event& e) {
  json j{{"seq", e.seq},
         {"ts", e.ts_ms},
         {"index", e.index},
         {"handle", e.handle < 0 ? json(nullptr) : json(e.handle)},
         {"kind", e.kind},
         {"text", e.text}};
  if (!e.data.is_null()) j["data"] = e.data;
  return j;
}

json Decoder::events(uint64_t since) const {
  json out = json::array();
  for (size_t i = 0; i < events_.size(); ++i) {
    const Event& e = events_[(events_head_ + i) % events_.size()];
    if (e.seq > since) out.push_back(event_json(e));
  }
  return out;
}

std::vector<Event> Decoder::take_new_events() {
  std::vector<Event> out;
  for (size_t i = 0; i < events_.size(); ++i) {
    const Event& e = events_[(events_head_ + i) % events_.size()];
    if (e.seq > published_seq_) out.push_back(e);
  }
  published_seq_ = next_seq_ - 1;
  return out;
}

void Decoder::add_mark(int64_t ts_us, const std::string& text) {
  last_mark_ = text;
  last_mark_ts_ = ts_us;
  emit(ts_us, -1, -1, "mark", text);
}

// ---------------------------------------------------------------------------------------------
// Adapters and connections

Decoder::Adapter* Decoder::adapter(uint16_t index, bool create) {
  for (auto& a : adapters_) {
    if (a->index == index) return a.get();
  }
  if (!create || index == kHciDevNone) return nullptr;
  // Traces that start after NEW_INDEX (or H4 btsnoop, which has no index at all) still get one.
  auto a = std::make_unique<Adapter>();
  a->index = index;
  a->name = "hci" + std::to_string(index);
  a->up = true;
  adapters_.push_back(std::move(a));
  return adapters_.back().get();
}

void Decoder::set_acl_buffers(uint16_t index, uint16_t acl_mtu, uint16_t acl_pkts,
                              uint16_t sco_mtu, uint16_t sco_pkts) {
  Adapter* a = adapter(index, true);
  if (!a || a->acl_pkts) return;
  a->acl_mtu = acl_mtu;
  a->acl_pkts = acl_pkts;
  a->sco_mtu = sco_mtu;
  a->sco_pkts = sco_pkts;
}

Decoder::Conn* Decoder::conn(Adapter* a, uint16_t handle) {
  for (auto& c : a->conns) {
    if (c->handle == handle) return c.get();
  }
  return nullptr;
}

Decoder::Conn* Decoder::new_conn(int64_t ts, Adapter* a, uint16_t handle, int type,
                                 const uint8_t* peer, int peer_type, bool lazy) {
  // A handle is reused only after its disconnection; a stale entry (disconnected but still
  // waiting for its window to close) is replaced at once.
  for (auto it = a->conns.begin(); it != a->conns.end(); ++it) {
    if ((*it)->handle != handle) continue;
    Conn* old = it->get();
    if (old->pool != kPoolNone) {
      a->outstanding[old->pool] -= std::min(a->outstanding[old->pool], old->in_flight);
    }
    a->conns.erase(it);
    break;
  }

  auto c = std::make_unique<Conn>();
  c->index = a->index;
  c->handle = handle;
  c->type = type;
  c->pool = a->pool_for(type);
  c->lazy = lazy;
  c->since_us = ts;
  c->peer_type = peer_type;
  if (peer) {
    std::memcpy(c->peer, peer, 6);
    c->peer_known = true;
  }
  Conn* raw = c.get();
  a->conns.push_back(std::move(c));

  std::string text = std::string(link_name(type)) + " handle " + std::to_string(handle);
  if (raw->peer_known) {
    text += " to " + bdaddr_str(raw->peer);
    if (const char* pt = peer_type_name(peer_type)) text += std::string(" (") + pt + ")";
  }
  if (lazy) text += " (already connected when the trace started)";
  Event& e = emit(ts, a->index, handle, "conn", text);
  e.data = json{{"type", link_name(type)},
                {"peer", raw->peer_known ? bdaddr_str(raw->peer) : ""}};
  return raw;
}

void Decoder::drop_conn(int64_t ts, Adapter* a, Conn* c, const char* why, int reason) {
  if (!c->connected) return;
  // The controller frees a link's buffers when it goes: its in-flight packets will never be
  // completed by NOCP, so they stop counting against the pool.
  if (c->pool != kPoolNone) {
    a->outstanding[c->pool] -= std::min(a->outstanding[c->pool], c->in_flight);
  }
  c->in_flight = 0;
  c->fifo_n = 0;
  c->connected = false;
  for (auto& ch : c->chans) ch->closed = true;
  c->avdtp.streams.clear();
  c->avdtp.sig_uid = 0;

  std::string text = std::string(link_name(c->type)) + " handle " + std::to_string(c->handle);
  if (c->peer_known) text += " to " + bdaddr_str(c->peer);
  text += std::string(" ") + why;
  if (reason >= 0) text += " (reason " + hex2(static_cast<uint8_t>(reason)) + ")";
  emit(ts, a->index, c->handle, "disconn", text);
}

void Decoder::drop_all_conns(int64_t ts, Adapter* a, const char* why) {
  for (auto& c : a->conns) drop_conn(ts, a, c.get(), why, -1);
}

// ---------------------------------------------------------------------------------------------
// Packet entry

void Decoder::packet(int64_t ts, uint16_t index, uint16_t opcode, const uint8_t* d, size_t len) {
  ++packets_;
  advance(ts);

  switch (opcode) {
    case kMonNewIndex:
      on_new_index(ts, index, d, len);
      return;
    case kMonDelIndex:
      if (Adapter* a = adapter(index, false)) {
        drop_all_conns(ts, a, "lost: controller removed");
        emit(ts, index, -1, "index", a->name + " removed");
        // Kept until the window closes so its last stats are still reported.
        a->up = false;
        a->index = kHciDevNone;  // a re-added controller with this index starts fresh
      }
      return;
    case kMonOpenIndex:
      if (Adapter* a = adapter(index, true)) {
        a->up = true;
        emit(ts, index, -1, "index", a->name + " up");
      }
      return;
    case kMonCloseIndex:
      if (Adapter* a = adapter(index, false)) {
        a->up = false;
        drop_all_conns(ts, a, "lost: controller closed");
        emit(ts, index, -1, "index", a->name + " down");
      }
      return;
    case kMonIndexInfo:
      if (Adapter* a = adapter(index, true); a && len >= 8) {
        std::memcpy(a->addr, d, 6);
        a->addr_known = true;
        a->manufacturer = le16(d + 6);
      }
      return;
    case kMonUserLogging:
      on_user_logging(ts, index, d, len);
      return;
    default:
      break;
  }

  Adapter* a = adapter(index, true);
  if (!a) return;

  switch (opcode) {
    case kMonCommand:
      on_command(ts, a, d, len);
      break;
    case kMonEvent:
      on_event(ts, a, d, len);
      break;
    case kMonAclTx:
    case kMonAclRx:
      on_acl(ts, a, opcode == kMonAclTx, d, len);
      break;
    case kMonScoTx:
    case kMonScoRx:
      on_sco_iso(ts, a, opcode == kMonScoTx, false, d, len);
      break;
    case kMonIsoTx:
    case kMonIsoRx:
      on_sco_iso(ts, a, opcode == kMonIsoTx, true, d, len);
      break;
    default:
      break;
  }
}

void Decoder::on_new_index(int64_t ts, uint16_t index, const uint8_t* d, size_t len) {
  Adapter* a = adapter(index, true);
  if (!a) return;
  if (len >= 16) {
    a->bus = d[1];
    std::memcpy(a->addr, d + 2, 6);
    a->addr_known = true;
    char name[9] = {};
    std::memcpy(name, d + 8, 8);
    if (name[0]) a->name = name;
  }
  a->up = false;
  emit(ts, index, -1, "index",
       a->name + " added" + (a->addr_known ? " (" + bdaddr_str(a->addr) + ")" : ""));
}

void Decoder::on_user_logging(int64_t ts, uint16_t index, const uint8_t* d, size_t len) {
  if (len < 2) return;
  const size_t ident_len = d[1];
  if (len < 2 + ident_len) return;
  const char* ident = reinterpret_cast<const char*>(d + 2);
  // Only our own marks: bluetoothd logs everything here, and with -d that is a flood.
  if (ident_len < 7 || std::strncmp(ident, "btbench", 7) != 0) return;
  const char* msg = reinterpret_cast<const char*>(d + 2 + ident_len);
  const size_t msg_max = len - 2 - ident_len;
  std::string text(msg, strnlen(msg, msg_max));
  // POST /api/hci/mark already recorded it; this is the kernel's echo of that frame.
  if (text == last_mark_ && std::llabs(ts - last_mark_ts_) < 2000000) return;
  emit(ts, index == kHciDevNone ? -1 : index, -1, "mark", text);
}

// ---------------------------------------------------------------------------------------------
// HCI commands and events

void Decoder::on_command(int64_t, Adapter* a, const uint8_t* d, size_t len) {
  if (len < 3) return;
  const uint16_t opcode = le16(d);
  const uint8_t* p = d + 3;
  const size_t plen = std::min<size_t>(d[2], len - 3);

  switch (opcode) {
    case kCmdWriteSyncFlowControl:
      if (plen >= 1) a->pending_sco_flow = p[0];
      break;
    case kCmdLeCreateCis:
      // The local side (central) pairs each CIS with its ACL here; a peripheral learns the
      // same from the CIS Request event.
      if (plen >= 1) {
        for (size_t i = 0; i < p[0] && 1 + 4 * (i + 1) <= plen; ++i) {
          if (a->cis_links.size() >= 32) a->cis_links.erase(a->cis_links.begin());
          a->cis_links.emplace_back(le16(p + 1 + 4 * i) & 0x0fff,
                                    le16(p + 3 + 4 * i) & 0x0fff);
        }
      }
      break;
    default:
      break;
  }
}

void Decoder::on_cmd_complete(int64_t ts, Adapter* a, const uint8_t* d, size_t len) {
  if (len < 4) return;
  const uint16_t opcode = le16(d + 1);
  const uint8_t* p = d + 3;
  const size_t plen = len - 3;
  if (p[0] != 0) return;  // failed command: nothing learned

  switch (opcode) {
    case kCmdReadBufferSize:
      if (plen >= 8) {
        a->acl_mtu = le16(p + 1);
        a->sco_mtu = p[3];
        a->acl_pkts = le16(p + 4);
        a->sco_pkts = le16(p + 6);
      }
      break;
    case kCmdLeReadBufferSize:
      if (plen >= 4) {
        a->le_mtu = le16(p + 1);
        a->le_pkts = p[3];
      }
      break;
    case kCmdLeReadBufferSizeV2:
      if (plen >= 7) {
        a->le_mtu = le16(p + 1);
        a->le_pkts = p[3];
        a->iso_mtu = le16(p + 4);
        a->iso_pkts = p[6];
      }
      break;
    case kCmdReadBdAddr:
      if (plen >= 7) {
        std::memcpy(a->addr, p + 1, 6);
        a->addr_known = true;
      }
      break;
    case kCmdWriteSyncFlowControl:
      if (a->pending_sco_flow >= 0) a->sco_flow = a->pending_sco_flow != 0;
      a->pending_sco_flow = -1;
      break;
    case kCmdLeBigTerminateSync:
      if (plen >= 2) {
        for (auto& c : a->conns) {
          if (c->type == kBis && c->big == p[1]) drop_conn(ts, a, c.get(), "terminated", -1);
        }
      }
      break;
    default:
      break;
  }
}

void Decoder::on_event(int64_t ts, Adapter* a, const uint8_t* d, size_t len) {
  if (len < 2) return;
  const uint8_t evt = d[0];
  const uint8_t* p = d + 2;
  const size_t plen = std::min<size_t>(d[1], len - 2);

  switch (evt) {
    case kEvtConnComplete: {
      if (plen < 10) return;
      const uint16_t handle = le16(p + 1) & 0x0fff;
      if (p[0] != 0) {
        emit(ts, a->index, -1, "conn",
             "connection to " + bdaddr_str(p + 3) + " failed (status " + hex2(p[0]) + ")");
        return;
      }
      new_conn(ts, a, handle, p[9] == 0x01 ? kAcl : kSco, p + 3, -1, false);
      return;
    }
    case kEvtSyncConnComplete: {
      if (plen < 10) return;
      if (p[0] != 0) {
        emit(ts, a->index, -1, "conn",
             "SCO to " + bdaddr_str(p + 3) + " failed (status " + hex2(p[0]) + ")");
        return;
      }
      new_conn(ts, a, le16(p + 1) & 0x0fff, p[9] == 0x02 ? kEsco : kSco, p + 3, -1, false);
      return;
    }
    case kEvtDisconnComplete: {
      if (plen < 4 || p[0] != 0) return;
      if (Conn* c = conn(a, le16(p + 1) & 0x0fff)) drop_conn(ts, a, c, "disconnected", p[3]);
      return;
    }
    case kEvtCmdComplete:
      on_cmd_complete(ts, a, p, plen);
      return;
    case kEvtNumCompletedPackets:
      on_nocp(ts, a, p, plen);
      return;
    case kEvtLeMeta:
      on_le_meta(ts, a, p, plen);
      return;
    default:
      return;
  }
}

void Decoder::on_le_meta(int64_t ts, Adapter* a, const uint8_t* d, size_t len) {
  if (len < 1) return;
  const uint8_t sub = d[0];
  const uint8_t* p = d + 1;
  len -= 1;

  switch (sub) {
    case kLeConnComplete:
    case kLeEnhConnComplete:
    case kLeEnhConnCompleteV2: {
      // All three start status, handle, role, peer address type, peer address.
      if (len < 11) return;
      if (p[0] != 0) return;  // a cancelled/failed LE create connection: routine, not news
      Conn* c = new_conn(ts, a, le16(p + 1) & 0x0fff, kLe, p + 5, p[4], false);
      c->role = p[3];
      return;
    }
    case kLeCisRequest:
      if (len >= 4) {
        if (a->cis_links.size() >= 32) a->cis_links.erase(a->cis_links.begin());
        a->cis_links.emplace_back(le16(p + 2) & 0x0fff, le16(p) & 0x0fff);
      }
      return;
    case kLeCisEstablished:
    case kLeCisEstablishedV2: {
      if (len < 3 || p[0] != 0) return;
      const uint16_t handle = le16(p + 1) & 0x0fff;
      const uint8_t* peer = nullptr;
      int peer_type = -1;
      for (auto it = a->cis_links.rbegin(); it != a->cis_links.rend(); ++it) {
        if (it->first != handle) continue;
        if (Conn* acl = conn(a, it->second); acl && acl->peer_known) {
          peer = acl->peer;
          peer_type = acl->peer_type;
        }
        break;
      }
      new_conn(ts, a, handle, kCis, peer, peer_type, false);
      return;
    }
    case kLeBigComplete:
    case kLeBigSyncEstablished: {
      const size_t num_at = sub == kLeBigComplete ? 17 : 13;
      if (len < num_at + 1 || p[0] != 0) return;
      const uint8_t big = p[1];
      for (size_t i = 0; i < p[num_at] && num_at + 1 + 2 * (i + 1) <= len; ++i) {
        Conn* c = new_conn(ts, a, le16(p + num_at + 1 + 2 * i) & 0x0fff, kBis, nullptr, -1,
                           false);
        c->big = big;
      }
      return;
    }
    case kLeBigTerminate:
    case kLeBigSyncLost:
      if (len >= 2) {
        for (auto& c : a->conns) {
          if (c->type == kBis && c->big == p[0]) {
            drop_conn(ts, a, c.get(), sub == kLeBigTerminate ? "terminated" : "sync lost", p[1]);
          }
        }
      }
      return;
    default:
      return;
  }
}

// Number Of Completed Packets: each completion retires the oldest TX timestamp of that handle.
// The difference is how long the packet sat in the controller — from the host handing it over
// to the controller reporting its buffer free (for ACL: acknowledged by the peer's baseband, or
// flushed). It covers ACL, SCO (with flow control) and ISO alike: one handle space per controller.
void Decoder::on_nocp(int64_t ts, Adapter* a, const uint8_t* d, size_t len) {
  if (len < 1) return;
  const size_t n = d[0];
  for (size_t i = 0; i < n && 1 + 4 * (i + 1) <= len; ++i) {
    const uint16_t handle = le16(d + 1 + 4 * i) & 0x0fff;
    const uint16_t count = le16(d + 3 + 4 * i);
    Conn* c = conn(a, handle);
    if (!c || !c->connected) continue;

    for (uint16_t j = 0; j < count; ++j) {
      if (c->fifo_n == 0) {
        ++c->nocp_unmatched;  // sent before the trace started, or the FIFO overflowed
        continue;
      }
      const TxEntry e = c->fifo[c->fifo_head];
      c->fifo_head = (c->fifo_head + 1) % kTxFifo;
      --c->fifo_n;
      const int64_t lat = ts - e.ts_us;
      if (lat < 0) continue;  // the wall clock stepped back (NTP); no meaningful sample
      c->lat.add_us(lat);
      c->lat_total.add_us(lat);
      if (Channel* ch = c->find_uid(e.uid)) ch->lat.add_us(lat);
    }

    const uint32_t done = std::min<uint32_t>(count, c->in_flight);
    c->in_flight -= done;
    if (c->pool != kPoolNone) {
      a->outstanding[c->pool] -= std::min(a->outstanding[c->pool], done);
    }
  }
}

void Decoder::tx_packet(Adapter* a, Conn* c, int64_t ts, uint32_t bytes, Channel* ch) {
  c->tx.add(bytes);
  if (ch) ch->tx.add(bytes);
  if (c->pool == kPoolNone || !c->connected) return;

  if (c->fifo_n == kTxFifo) {
    // NOCP has not kept up with kTxFifo packets — far more than any controller buffers, so
    // completions were lost (or never come). Drop the oldest; its completion will be
    // attributed to the next one, which the overflow counter owns up to.
    c->fifo_head = (c->fifo_head + 1) % kTxFifo;
    --c->fifo_n;
    ++c->fifo_overflow;
  }
  c->fifo[(c->fifo_head + c->fifo_n) % kTxFifo] = TxEntry{ts, ch ? ch->uid : 0};
  ++c->fifo_n;

  ++c->in_flight;
  c->in_flight_max = std::max(c->in_flight_max, c->in_flight);
  ++a->outstanding[c->pool];
  a->outstanding_max[c->pool] = std::max(a->outstanding_max[c->pool], a->outstanding[c->pool]);
}

void Decoder::rx_packet(Conn* c, uint32_t bytes, Channel* ch) {
  c->rx.add(bytes);
  if (ch) ch->rx.add(bytes);
}

void Decoder::on_sco_iso(int64_t ts, Adapter* a, bool tx, bool iso, const uint8_t* d,
                         size_t len) {
  const size_t hdr = iso ? 4 : 3;
  if (len < hdr) return;
  const uint16_t handle = le16(d) & 0x0fff;
  Conn* c = conn(a, handle);
  if (!c) c = new_conn(ts, a, handle, iso ? kCis : kSco, nullptr, -1, true);
  const uint32_t bytes = static_cast<uint32_t>(len - hdr);
  if (tx) {
    tx_packet(a, c, ts, bytes, nullptr);
  } else {
    rx_packet(c, bytes, nullptr);
  }
}

// ---------------------------------------------------------------------------------------------
// ACL and L2CAP

Decoder::Channel* Decoder::add_channel(Conn* c, uint16_t lcid, uint16_t rcid, uint16_t psm,
                                       bool le) {
  if (c->chans.size() >= kMaxChannels) return nullptr;
  auto ch = std::make_unique<Channel>();
  ch->uid = next_uid_++;
  if (next_uid_ == 0) next_uid_ = 1;
  ch->lcid = lcid;
  ch->rcid = rcid;
  ch->psm = psm;
  ch->le = le;
  ch->name = psm ? psm_name(psm, le) : "CID " + hex4(lcid ? lcid : rcid);
  c->chans.push_back(std::move(ch));
  return c->chans.back().get();
}

Decoder::Channel* Decoder::channel_for_data(int64_t, Conn* c, bool tx, uint16_t cid) {
  if (cid < kCidDynamicStart) {
    for (auto& ch : c->chans) {
      if (ch->fixed && ch->lcid == cid) return ch.get();
    }
    // A link first seen by its data is assumed BR/EDR until an LE-only fixed channel shows.
    if (c->lazy && c->type == kAcl &&
        (cid == kCidAtt || cid == kCidLeSignaling || cid == kCidSmp)) {
      c->type = kLe;
    }
    Channel* ch = add_channel(c, cid, cid, 0, c->type == kLe);
    if (!ch) return nullptr;
    ch->fixed = true;
    ch->open = true;
    ch->name = fixed_cid_name(cid);
    if (cid == kCidSignaling || cid == kCidLeSignaling) ch->role = kRoleSig;
    return ch;
  }

  // TX carries the peer's CID (the destination), RX carries ours.
  for (auto& ch : c->chans) {
    if (ch->closed || ch->fixed) continue;
    if ((tx ? ch->rcid : ch->lcid) == cid) return ch.get();
  }
  // Opened before the trace started: count it under its CID, it has no PSM.
  Channel* ch = add_channel(c, tx ? 0 : cid, tx ? cid : 0, 0, c->type == kLe);
  if (ch) ch->open = true;
  return ch;
}

void Decoder::on_acl(int64_t ts, Adapter* a, bool tx, const uint8_t* d, size_t len) {
  if (len < 4) return;
  const uint16_t hf = le16(d);
  const uint16_t handle = hf & 0x0fff;
  const uint8_t pb = (hf >> 12) & 0x03;
  len = std::min<size_t>(len - 4, le16(d + 2));
  d += 4;

  Conn* c = conn(a, handle);
  if (!c) c = new_conn(ts, a, handle, kAcl, nullptr, -1, true);
  Frag& f = c->frag[tx ? kTx : kRx];
  Channel* ch = nullptr;

  if (pb == 0x01) {
    // Continuation of the PDU the last start fragment in this direction began.
    ch = c->find_uid(f.uid);
    if (f.collect && ch) {
      const size_t n = std::min<size_t>(len, f.want - f.have);
      std::memcpy(f.buf.data() + f.have, d, n);
      f.have += static_cast<uint32_t>(n);
      if (f.have == f.want) {
        f.collect = false;
        sdu(ts, a, c, ch, tx, f.buf.data(), f.want);
      }
    }
    f.remaining = f.remaining > len ? static_cast<uint32_t>(f.remaining - len) : 0;
    if (f.remaining == 0) {
      f.uid = 0;
      f.collect = false;
    }
  } else {
    f.uid = 0;
    f.collect = false;
    f.remaining = 0;
    if (len >= 4) {
      const uint16_t l2len = le16(d);
      const uint16_t cid = le16(d + 2);
      ch = channel_for_data(ts, c, tx, cid);
      // channel_for_data may have reclassified a lazily created link: refresh nothing else.
      const size_t got = len - 4;
      if (ch) {
        if (got >= l2len) {
          sdu(ts, a, c, ch, tx, d + 4, l2len);
        } else {
          f.uid = ch->uid;
          f.remaining = static_cast<uint32_t>(l2len - got);
          const bool needs_all = ch->role == kRoleSig || ch->role == kRoleAvdtpSig;
          if (needs_all && l2len <= kSigBuf) {
            std::memcpy(f.buf.data(), d + 4, got);
            f.have = static_cast<uint32_t>(got);
            f.want = l2len;
            f.collect = true;
          } else if (ch->role == kRoleAvdtpMedia) {
            // RTP and the codec's media header are in the first fragment: that is all we read.
            sdu(ts, a, c, ch, tx, d + 4, got);
          }
        }
      }
    }
  }

  const uint32_t bytes = static_cast<uint32_t>(len);
  if (tx) {
    tx_packet(a, c, ts, bytes, ch);
  } else {
    rx_packet(c, bytes, ch);
  }
}

void Decoder::sdu(int64_t ts, Adapter* a, Conn* c, Channel* ch, bool tx, const uint8_t* d,
                  size_t len) {
  switch (ch->role) {
    case kRoleSig:
      l2cap_sig(ts, a, c, tx, ch->lcid == kCidLeSignaling, d, len);
      break;
    case kRoleAvdtpSig:
      avdtp_sig(ts, a, c, tx, d, len);
      break;
    case kRoleAvdtpMedia:
      avdtp_media(ts, c, ch, tx, d, len);
      break;
    default:
      break;
  }
}

void Decoder::channel_opened(int64_t ts, Adapter* a, Conn* c, Channel* ch) {
  ch->open = true;
  std::string extra;
  if (ch->psm == kPsmAvdtp && !ch->le) {
    // AVDTP: the first channel on a link is signalling; every later one is the transport of
    // the stream that was just OPENed (AVDTP 1.3 §5.4.6, one transport channel per stream).
    Channel* sig = c->find_uid(c->avdtp.sig_uid);
    if (!sig || sig->closed) {
      c->avdtp.sig_uid = ch->uid;
      c->avdtp.streams.clear();
      ch->role = kRoleAvdtpSig;
      ch->name = "AVDTP signalling";
    } else {
      ch->role = kRoleAvdtpMedia;
      ch->name = "AVDTP media";
      Stream* s = c->stream_by_lseid(c->avdtp.awaiting_lseid);
      if (!s) {
        for (auto& st : c->avdtp.streams) {
          if (!st.media_uid) {
            s = &st;
            break;
          }
        }
      }
      if (s) {
        s->media_uid = ch->uid;
        extra = " for SEID " + std::to_string(s->lseid) + "/" + std::to_string(s->rseid);
      }
      c->avdtp.awaiting_lseid = -1;
    }
  }
  Event& e = emit(ts, a->index, c->handle, "l2cap",
                  ch->name + " open (PSM " + hex4(ch->psm) + ", scid " + hex4(ch->lcid) +
                      ", dcid " + hex4(ch->rcid) + ")" + extra);
  e.data = json{{"psm", ch->psm}, {"scid", ch->lcid}, {"dcid", ch->rcid}, {"name", ch->name},
                {"open", true}};
}

void Decoder::channel_closed(int64_t ts, Adapter* a, Conn* c, Channel* ch, const char* why) {
  if (ch->closed) return;
  ch->closed = true;
  if (ch->role == kRoleAvdtpSig && ch->uid == c->avdtp.sig_uid) {
    c->avdtp.sig_uid = 0;
    c->avdtp.streams.clear();
  } else if (ch->role == kRoleAvdtpMedia) {
    for (auto& s : c->avdtp.streams) {
      if (s.media_uid == ch->uid) s.media_uid = 0;
    }
  }
  Event& e = emit(ts, a->index, c->handle, "l2cap",
                  ch->name + " " + why + " (PSM " + hex4(ch->psm) + ", scid " + hex4(ch->lcid) +
                      ", dcid " + hex4(ch->rcid) + ")");
  e.data = json{{"psm", ch->psm}, {"scid", ch->lcid}, {"dcid", ch->rcid}, {"name", ch->name},
                {"open", false}};
}

// L2CAP signalling, CID 1 (BR/EDR, may carry several commands per frame) and CID 5 (LE). Only
// the commands that create and destroy channels matter here. The CIDs in them are from the
// sender's point of view: a request's Source CID is the requester's endpoint, a response's
// Destination CID is the responder's. "tx" means we sent this command.
void Decoder::l2cap_sig(int64_t ts, Adapter* a, Conn* c, bool tx, bool le, const uint8_t* d,
                        size_t len) {
  auto pend_add = [&](uint8_t ident, uint8_t n, const uint32_t* uids) {
    SigPending& p = c->sigpend[c->sigpend_next];
    c->sigpend_next = (c->sigpend_next + 1) % 8;
    p.valid = true;
    p.tx = tx;
    p.ident = ident;
    p.n = n;
    std::copy(uids, uids + n, p.uids);
  };
  // A response matches the request with the same identifier that went the other way.
  auto pend_take = [&](uint8_t ident) -> SigPending* {
    for (auto& p : c->sigpend) {
      if (p.valid && p.ident == ident && p.tx != tx) {
        p.valid = false;
        return &p;
      }
    }
    return nullptr;
  };
  auto pending_chan = [&](bool by_lcid, uint16_t cid) -> Channel* {
    for (auto& ch : c->chans) {
      if (ch->open || ch->closed || ch->fixed) continue;
      if (by_lcid ? (ch->lcid == cid && !ch->rcid) : (ch->rcid == cid && !ch->lcid)) {
        return ch.get();
      }
    }
    return nullptr;
  };
  auto set_peer_cid = [&](Channel* ch, uint16_t cid) {
    // In a response we receive the Destination CID is the peer's; in one we send, it is ours.
    if (tx) {
      ch->lcid = cid;
    } else {
      ch->rcid = cid;
    }
  };

  while (len >= 4) {
    const uint8_t code = d[0];
    const uint8_t ident = d[1];
    const size_t clen = le16(d + 2);
    const uint8_t* p = d + 4;
    if (clen > len - 4) break;

    switch (code) {
      case kSigConnReq:
        if (clen >= 4) {
          const uint16_t psm = le16(p), scid = le16(p + 2);
          add_channel(c, tx ? scid : 0, tx ? 0 : scid, psm, le);
        }
        break;
      case kSigConnRsp:
        if (clen >= 6) {
          const uint16_t dcid = le16(p), scid = le16(p + 2), result = le16(p + 4);
          // scid echoes the requester's CID: ours if we receive the response.
          Channel* ch = pending_chan(!tx, scid);
          if (!ch) break;
          if (result == 0x0000) {
            set_peer_cid(ch, dcid);
            channel_opened(ts, a, c, ch);
          } else if (result != 0x0001) {  // 1 = pending, a final response follows
            channel_closed(ts, a, c, ch, ("refused, result " + hex4(result)).c_str());
          }
        }
        break;
      case kSigDisconnRsp:
        if (clen >= 4) {
          // The response's DCID is the responder's endpoint, its SCID the requester's.
          const uint16_t dcid = le16(p), scid = le16(p + 2);
          const uint16_t lcid = tx ? dcid : scid, rcid = tx ? scid : dcid;
          for (auto& ch : c->chans) {
            if (!ch->closed && !ch->fixed && ch->lcid == lcid && ch->rcid == rcid) {
              channel_closed(ts, a, c, ch.get(), "closed");
              break;
            }
          }
        }
        break;
      case kSigLeConnReq:
        if (clen >= 10) {
          const uint16_t psm = le16(p), scid = le16(p + 2);
          if (Channel* ch = add_channel(c, tx ? scid : 0, tx ? 0 : scid, psm, true)) {
            pend_add(ident, 1, &ch->uid);
          }
        }
        break;
      case kSigLeConnRsp:
        if (clen >= 10) {
          const uint16_t dcid = le16(p), result = le16(p + 8);
          SigPending* sp = pend_take(ident);
          Channel* ch = sp ? c->find_uid(sp->uids[0]) : nullptr;
          if (!ch) break;
          if (result == 0x0000 && dcid) {
            set_peer_cid(ch, dcid);
            channel_opened(ts, a, c, ch);
          } else {
            channel_closed(ts, a, c, ch, ("refused, result " + hex4(result)).c_str());
          }
        }
        break;
      case kSigEcredConnReq:
        if (clen >= 10) {
          const uint16_t psm = le16(p);
          uint32_t uids[5];
          uint8_t n = 0;
          for (size_t off = 8; off + 2 <= clen && n < 5; off += 2) {
            const uint16_t scid = le16(p + off);
            if (Channel* ch = add_channel(c, tx ? scid : 0, tx ? 0 : scid, psm, le)) {
              uids[n++] = ch->uid;
            }
          }
          if (n) pend_add(ident, n, uids);
        }
        break;
      case kSigEcredConnRsp:
        if (clen >= 8) {
          SigPending* sp = pend_take(ident);
          if (!sp) break;
          const uint16_t result = le16(p + 6);
          for (uint8_t i = 0; i < sp->n; ++i) {
            Channel* ch = c->find_uid(sp->uids[i]);
            if (!ch) continue;
            const size_t off = 8 + 2u * i;
            const uint16_t dcid = off + 2 <= clen ? le16(p + off) : 0;
            // A zero DCID refuses that one channel; the others may still succeed.
            if (dcid) {
              set_peer_cid(ch, dcid);
              channel_opened(ts, a, c, ch);
            } else {
              channel_closed(ts, a, c, ch, ("refused, result " + hex4(result)).c_str());
            }
          }
        }
        break;
      default:
        break;
    }

    d += 4 + clen;
    len -= 4 + clen;
  }
}

// ---------------------------------------------------------------------------------------------
// AVDTP

namespace {

// Walks a service capability list; returns the Media Codec (category 7) if present.
bool find_codec(const uint8_t* p, size_t len, CodecInfo* codec, bool* delay_reporting) {
  bool found = false;
  while (len >= 2) {
    const uint8_t cat = p[0];
    const size_t clen = p[1];
    if (clen > len - 2) break;
    if (cat == 0x07) found = decode_media_codec(p + 2, clen, codec);
    if (cat == 0x08 && delay_reporting) *delay_reporting = true;
    p += 2 + clen;
    len -= 2 + clen;
  }
  return found;
}

}  // namespace

void Decoder::avdtp_sig(int64_t ts, Adapter* a, Conn* c, bool tx, const uint8_t* d, size_t len) {
  if (len < 2) return;
  const uint8_t label = d[0] >> 4;
  const uint8_t ptype = (d[0] >> 2) & 0x03;
  const uint8_t mtype = d[0] & 0x03;
  const char* dir = tx ? "TX" : "RX";

  if (ptype == 0x02 || ptype == 0x03) return;  // continue/end of a fragmented message
  if (ptype == 0x01) {
    // A start packet: the message spans several L2CAP SDUs. Rare (only huge capability lists);
    // name it and leave the content.
    if (len >= 3) {
      emit(ts, a->index, c->handle, "avdtp",
           std::string(dir) + " " + avdtp_signal_name(d[2] & 0x3f) + " (fragmented)");
    }
    return;
  }

  const uint8_t signal = d[1] & 0x3f;
  const char* name = avdtp_signal_name(signal);
  const uint8_t* p = d + 2;
  const size_t n = len - 2;
  std::string text = std::string(dir) + " " + name;
  json data{{"signal", name}, {"dir", tx ? "tx" : "rx"}, {"label", label}};

  if (mtype == 0x00) {
    AvdtpPending& pend = c->avdtp.pend[tx ? kTx : kRx][label];
    pend = AvdtpPending{};
    pend.valid = true;
    pend.signal = signal;
    data["type"] = "cmd";
    text += " cmd";

    switch (signal) {
      case 0x03:    // SET_CONFIGURATION: ACP SEID, INT SEID, capabilities
      case 0x05: {  // RECONFIGURE: ACP SEID, capabilities
        const size_t caps = signal == 0x03 ? 2 : 1;
        if (n < caps) break;
        pend.seids[0] = p[0] >> 2;
        pend.nseid = 1;
        text += " acp " + std::to_string(pend.seids[0]);
        if (signal == 0x03) {
          pend.int_seid = p[1] >> 2;
          text += " int " + std::to_string(pend.int_seid);
        }
        bool delay = false;
        pend.has_codec = find_codec(p + caps, n - caps, &pend.codec, &delay);
        if (pend.has_codec) {
          text += ": " + pend.codec.name;
          if (!pend.codec.config.empty()) text += " " + pend.codec.config;
          data["codec"] = pend.codec.name;
          data["config"] = pend.codec.config;
        }
        if (delay) text += ", delay reporting";
        data["acp_seid"] = pend.seids[0];
        break;
      }
      case 0x07:    // START
      case 0x09: {  // SUSPEND: a list of ACP SEIDs
        for (size_t i = 0; i < n && pend.nseid < 4; ++i) pend.seids[pend.nseid++] = p[i] >> 2;
        text += " acp";
        for (uint8_t i = 0; i < pend.nseid; ++i) text += " " + std::to_string(pend.seids[i]);
        break;
      }
      case 0x0d: {  // DELAY_REPORT: ACP SEID, delay in 1/10 ms (big-endian)
        if (n < 3) break;
        pend.seids[0] = p[0] >> 2;
        pend.nseid = 1;
        const int delay = be16(p + 1);
        char s[48];
        std::snprintf(s, sizeof(s), " acp %u: %d.%d ms", pend.seids[0], delay / 10, delay % 10);
        text += s;
        data["delay_ms"] = delay / 10.0;
        if (Stream* st = c->stream_for_cmd(tx, pend.seids[0])) st->delay_tenths_ms = delay;
        break;
      }
      case 0x01:  // DISCOVER: no parameters
        break;
      default:  // the rest carry one ACP SEID
        if (n >= 1) {
          pend.seids[0] = p[0] >> 2;
          pend.nseid = 1;
          text += " acp " + std::to_string(pend.seids[0]);
          data["acp_seid"] = pend.seids[0];
        }
        break;
    }
    Event& e = emit(ts, a->index, c->handle, "avdtp", text);
    e.data = std::move(data);
    return;
  }

  // A response travels opposite to its command.
  AvdtpPending& pend = c->avdtp.pend[tx ? kRx : kTx][label];
  const bool matched = pend.valid && pend.signal == signal;
  const bool cmd_tx = !tx;

  if (mtype == 0x02) {
    data["type"] = "accept";
    text += " accept";
    switch (signal) {
      case 0x01: {  // DISCOVER: 2 bytes per SEP
        text += ":";
        for (size_t i = 0; i + 1 < n; i += 2) {
          char s[48];
          std::snprintf(s, sizeof(s), " %u %s%s%s", p[i] >> 2,
                        (p[i + 1] >> 4) == 0 ? "audio " : "",
                        (p[i + 1] & 0x08) ? "sink" : "source", (p[i] & 0x02) ? " (in use)" : "");
          text += s;
          if (i + 3 < n) text += ",";
        }
        break;
      }
      case 0x02:
      case 0x0c: {  // GET_(ALL_)CAPABILITIES: capability list
        CodecInfo codec;
        bool delay = false;
        if (find_codec(p, n, &codec, &delay)) {
          text += ": " + codec.name;
          if (!codec.config.empty()) text += " " + codec.config;
          data["codec"] = codec.name;
          data["config"] = codec.config;
        }
        if (delay) text += ", delay reporting";
        break;
      }
      default:
        break;
    }

    if (matched) {
      switch (signal) {
        case 0x03: {
          const uint8_t lseid = cmd_tx ? pend.int_seid : pend.seids[0];
          const uint8_t rseid = cmd_tx ? pend.seids[0] : pend.int_seid;
          auto& v = c->avdtp.streams;
          v.erase(std::remove_if(v.begin(), v.end(),
                                 [&](const Stream& s) { return s.lseid == lseid; }),
                  v.end());
          Stream s;
          s.lseid = lseid;
          s.rseid = rseid;
          s.has_codec = pend.has_codec;
          s.codec = pend.codec;
          s.state = "configured";
          v.push_back(std::move(s));
          if (pend.has_codec) {
            text += ": " + pend.codec.name;
            if (!pend.codec.config.empty()) text += " " + pend.codec.config;
            data["codec"] = pend.codec.name;
            data["config"] = pend.codec.config;
          }
          data["lseid"] = lseid;
          data["rseid"] = rseid;
          break;
        }
        case 0x05:
          if (Stream* s = c->stream_for_cmd(cmd_tx, pend.seids[0]); s && pend.has_codec) {
            s->codec = pend.codec;
            s->has_codec = true;
          }
          break;
        case 0x06:
          if (Stream* s = c->stream_for_cmd(cmd_tx, pend.seids[0])) {
            s->state = "open";
            c->avdtp.awaiting_lseid = s->lseid;
          }
          break;
        case 0x07:
        case 0x09:
          for (uint8_t i = 0; i < pend.nseid; ++i) {
            if (Stream* s = c->stream_for_cmd(cmd_tx, pend.seids[i])) {
              s->state = signal == 0x07 ? "streaming" : "suspended";
            }
          }
          break;
        case 0x08:
        case 0x0a: {
          if (pend.nseid == 0) break;
          const uint8_t seid = pend.seids[0];
          auto& v = c->avdtp.streams;
          v.erase(std::remove_if(v.begin(), v.end(),
                                 [&](const Stream& s) {
                                   return (cmd_tx ? s.rseid : s.lseid) == seid;
                                 }),
                  v.end());
          break;
        }
        default:
          break;
      }
    }
  } else {
    // 1: general reject (unknown signal); 3: reject with an error code. Where the code sits
    // depends on the signal: after a category (configuration) or an ACP SEID (start/suspend).
    data["type"] = "reject";
    uint8_t err = 0;
    if (mtype == 0x03) {
      const bool second = signal == 0x03 || signal == 0x05 || signal == 0x07 || signal == 0x09;
      const size_t at = second ? 1 : 0;
      if (n > at) err = p[at];
      text += " reject (error " + hex2(err) + ")";
      data["error"] = err;
    } else {
      text += " general reject";
    }
  }

  if (matched) pend.valid = false;
  Event& e = emit(ts, a->index, c->handle, "avdtp", text);
  e.data = std::move(data);
}

// The transport channel: RTP header, then the codec's media payload. Loss is a gap in the RTP
// sequence numbers; jitter is RFC 3550's interarrival jitter, with the HCI timestamp as the
// arrival time — for TX that measures how evenly the source (PipeWire, bluealsa) hands packets to
// the kernel, for RX how evenly the peer's packets arrive.
void Decoder::avdtp_media(int64_t ts, Conn* c, Channel* ch, bool tx, const uint8_t* d,
                          size_t len) {
  const Stream* s = nullptr;
  for (const auto& st : c->avdtp.streams) {
    if (st.media_uid == ch->uid) s = &st;
  }
  if (s && s->has_codec && !s->codec.rtp) return;  // aptX / FastStream: no RTP header
  if (len < 12 || (d[0] >> 6) != 2) return;

  const uint16_t seq = be16(d + 2);
  const uint32_t rts = be32(d + 4);
  const uint32_t ssrc = be32(d + 8);
  const uint32_t rate = s && s->has_codec ? s->codec.rate : 0;
  Rtp& r = ch->rtp[tx ? kTx : kRx];

  if (!r.init || r.ssrc != ssrc) {
    const uint32_t keep_lost = r.lost_total;
    r = Rtp{};
    r.lost_total = keep_lost;
    r.init = true;
    r.ssrc = ssrc;
    r.pt = d[1] & 0x7f;
  } else {
    const uint16_t diff = static_cast<uint16_t>(seq - r.last_seq);
    if (diff == 0 || diff >= 0x8000) return;  // duplicate or late: neither loss nor progress
    if (diff > 1) {
      r.lost_total += diff - 1u;
      r.lost_win += diff - 1u;
    }
    if (rate && diff == 1) {
      // D(i-1,i) = (Rj - Ri) - (Sj - Si) in RTP clock units; J += (|D| - J) / 16.
      // Only between consecutive packets: across a gap — real loss, or a vendor snoop log that
      // samples media — and across a timestamp discontinuity (|D| over a second: the source
      // restarted its clock) the difference says nothing about delivery jitter.
      const double arrival = static_cast<double>(ts - r.last_arrival_us) * rate / 1e6;
      const double sent = static_cast<double>(static_cast<int32_t>(rts - r.last_ts));
      const double dd = std::fabs(arrival - sent);
      if (dd <= rate) r.jitter += (dd - r.jitter) / 16.0;
    }
  }
  r.last_seq = seq;
  r.last_ts = rts;
  r.last_arrival_us = ts;
  ++r.pkts_total;
  ++r.pkts_win;

  if (s && s->has_codec && s->codec.sbc) {
    size_t off = 12 + 4u * (d[0] & 0x0f);
    if ((d[0] & 0x10) && off + 4 <= len) off += 4 + 4u * be16(d + off + 2);
    if (off < len) {
      r.frames_last = d[off] & 0x0f;
      r.frames_sum_win += r.frames_last;
      ++r.frames_pkts_win;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Windows

int Decoder::advance(int64_t now_us) {
  if (window_start_us_ < 0) {
    window_start_us_ = now_us - now_us % kWindowUs;
    return 0;
  }
  // A wall clock stepped backwards (NTP on a board without RTC): restart the window grid.
  if (now_us < window_start_us_ - 10 * kWindowUs) {
    window_start_us_ = now_us - now_us % kWindowUs;
    return 0;
  }
  int closed = 0;
  while (now_us >= window_start_us_ + kWindowUs) {
    close_window(window_start_us_ + kWindowUs);
    ++closed;
    // An idle gap longer than the history (or a clock step forward) needs no empty windows
    // beyond what the history can show.
    if (static_cast<size_t>(closed) >= kHistorySize) {
      window_start_us_ = now_us - now_us % kWindowUs;
      break;
    }
  }
  return closed;
}

void Decoder::flush(int64_t now_us) {
  if (window_start_us_ < 0) return;
  if (now_us <= window_start_us_) now_us = window_start_us_ + 1000;
  close_window(now_us);
}

void Decoder::close_window(int64_t end_us) {
  const int64_t dur_us = std::max<int64_t>(1000, end_us - window_start_us_);
  const double per_s = 1e6 / static_cast<double>(dur_us);
  window_start_us_ = end_us;
  ++windows_closed_;

  json adapters = json::array();
  json conns = json::array();

  for (auto& ap : adapters_) {
    Adapter* a = ap.get();
    if (a->index != kHciDevNone) {
      adapters.push_back(json{{"index", a->index},
                              {"name", a->name},
                              {"addr", a->addr_known ? bdaddr_str(a->addr) : ""},
                              {"up", a->up},
                              {"acl_mtu", a->acl_mtu},
                              {"acl_pkts", a->acl_pkts},
                              {"sco_mtu", a->sco_mtu},
                              {"sco_pkts", a->sco_pkts},
                              {"le_mtu", a->le_mtu},
                              {"le_pkts", a->le_pkts},
                              {"iso_mtu", a->iso_mtu},
                              {"iso_pkts", a->iso_pkts},
                              {"sco_flow_control", a->sco_flow}});
    }

    for (auto& cp : a->conns) {
      Conn* c = cp.get();
      const int total = c->pool == kPoolNone ? 0 : a->pool_total(c->pool);
      json credits = nullptr, credits_min = nullptr;
      int32_t hist_credits_min = -1;
      if (total > 0) {
        credits = std::max<int>(0, total - static_cast<int>(a->outstanding[c->pool]));
        hist_credits_min = std::max<int>(0, total - static_cast<int>(a->outstanding_max[c->pool]));
        credits_min = hist_credits_min;
      }

      json chans = json::array();
      for (auto& chp : c->chans) {
        Channel* ch = chp.get();
        json cj{{"scid", ch->lcid},
                {"dcid", ch->rcid},
                {"psm", ch->psm},
                {"name", ch->name},
                {"open", ch->open && !ch->closed},
                {"tx_bps", static_cast<uint64_t>(ch->tx.bytes * 8 * per_s)},
                {"rx_bps", static_cast<uint64_t>(ch->rx.bytes * 8 * per_s)},
                {"tx_pps", static_cast<uint64_t>(ch->tx.pkts * per_s)},
                {"rx_pps", static_cast<uint64_t>(ch->rx.pkts * per_s)},
                {"lat_ms", lat_json(ch->lat)}};

        if (ch->role == kRoleAvdtpSig) {
          json streams = json::array();
          for (const auto& s : c->avdtp.streams) {
            streams.push_back(json{{"lseid", s.lseid},
                                   {"rseid", s.rseid},
                                   {"codec", s.has_codec ? s.codec.name : ""},
                                   {"config", s.has_codec ? s.codec.config : ""},
                                   {"state", s.state}});
          }
          cj["avdtp"] = json{{"role", "signalling"}, {"streams", std::move(streams)}};
        } else if (ch->role == kRoleAvdtpMedia) {
          const Stream* s = nullptr;
          for (const auto& st : c->avdtp.streams) {
            if (st.media_uid == ch->uid) s = &st;
          }
          // Report the direction that carries the stream (a source TXes, a sink RXes).
          const Rtp& r = ch->rtp[ch->rtp[kTx].pkts_total >= ch->rtp[kRx].pkts_total ? kTx : kRx];
          const uint32_t rate = s && s->has_codec ? s->codec.rate : 0;
          json av{{"role", "media"},
                  {"codec", s && s->has_codec ? s->codec.name : ""},
                  {"config", s && s->has_codec ? s->codec.config : ""},
                  {"state", s ? s->state : "idle"},
                  {"rtp_pkts", r.pkts_win},
                  {"rtp_lost", r.lost_win},
                  {"rtp_lost_total", ch->rtp[kTx].lost_total + ch->rtp[kRx].lost_total},
                  {"rtp_jitter_ms", rate ? json(round2(r.jitter * 1000.0 / rate)) : json(nullptr)},
                  {"frames_per_packet",
                   r.frames_pkts_win ? json(round2(static_cast<double>(r.frames_sum_win) /
                                                   r.frames_pkts_win))
                                     : json(nullptr)}};
          if (s) {
            av["lseid"] = s->lseid;
            av["rseid"] = s->rseid;
            av["delay_ms"] =
                s->delay_tenths_ms >= 0 ? json(s->delay_tenths_ms / 10.0) : json(nullptr);
          }
          cj["avdtp"] = std::move(av);
        }
        chans.push_back(std::move(cj));
      }

      json cjson{{"index", c->index},
                 {"handle", c->handle},
                 {"type", link_name(c->type)},
                 {"peer", c->peer_known ? bdaddr_str(c->peer) : ""},
                 {"connected", c->connected},
                 {"since", c->since_us / 1000},
                 {"tx_bps", static_cast<uint64_t>(c->tx.bytes * 8 * per_s)},
                 {"rx_bps", static_cast<uint64_t>(c->rx.bytes * 8 * per_s)},
                 {"tx_pps", static_cast<uint64_t>(c->tx.pkts * per_s)},
                 {"rx_pps", static_cast<uint64_t>(c->rx.pkts * per_s)},
                 {"pool", pool_name(c->pool)},
                 {"in_flight", c->in_flight},
                 {"in_flight_max", c->in_flight_max},
                 {"credits", credits},
                 {"credits_min", credits_min},
                 {"lat_ms", lat_json(c->lat)},
                 {"fifo_overflow", c->fifo_overflow},
                 {"nocp_unmatched", c->nocp_unmatched},
                 {"channels", std::move(chans)}};
      if (const char* pt = peer_type_name(c->peer_type)) cjson["peer_type"] = pt;
      if (c->role >= 0) cjson["role"] = c->role == 0 ? "central" : "peripheral";
      if (c->big >= 0) cjson["big"] = c->big;
      conns.push_back(std::move(cjson));

      HistEntry& h = c->hist[(c->hist_head + c->hist_n) % kHistorySize];
      if (c->hist_n < kHistorySize) {
        ++c->hist_n;
      } else {
        c->hist_head = (c->hist_head + 1) % kHistorySize;
      }
      const float nan = std::numeric_limits<float>::quiet_NaN();
      h.t_ms = end_us / 1000;
      h.tx_bps = static_cast<float>(c->tx.bytes * 8 * per_s);
      h.rx_bps = static_cast<float>(c->rx.bytes * 8 * per_s);
      h.p50 = c->lat.count() ? static_cast<float>(c->lat.percentile_ms(0.50)) : nan;
      h.p95 = c->lat.count() ? static_cast<float>(c->lat.percentile_ms(0.95)) : nan;
      h.in_flight = static_cast<uint16_t>(std::min<uint32_t>(c->in_flight_max, 0xffff));
      h.credits_min = hist_credits_min;

      // Reset for the next window.
      c->tx.clear();
      c->rx.clear();
      c->lat.clear();
      c->in_flight_max = c->in_flight;
      for (auto& ch : c->chans) {
        ch->tx.clear();
        ch->rx.clear();
        ch->lat.clear();
        for (auto& r : ch->rtp) {
          r.pkts_win = r.lost_win = r.frames_sum_win = r.frames_pkts_win = 0;
        }
      }
      // Closed channels have now been reported once.
      c->chans.erase(std::remove_if(c->chans.begin(), c->chans.end(),
                                    [](const std::unique_ptr<Channel>& ch) { return ch->closed; }),
                     c->chans.end());
    }
    for (int p = 0; p < 4; ++p) a->outstanding_max[p] = a->outstanding[p];

    // Disconnected links have been reported in this window; drop them (their events stay).
    a->conns.erase(std::remove_if(a->conns.begin(), a->conns.end(),
                                  [](const std::unique_ptr<Conn>& c) { return !c->connected; }),
                   a->conns.end());
  }
  adapters_.erase(std::remove_if(adapters_.begin(), adapters_.end(),
                                 [](const std::unique_ptr<Adapter>& a) {
                                   return a->index == kHciDevNone;
                                 }),
                  adapters_.end());

  stats_ = json{{"ts", end_us / 1000},
                {"window_ms", dur_us / 1000},
                {"adapters", std::move(adapters)},
                {"conns", std::move(conns)}};
}

bool Decoder::history(int index, int handle, int seconds, json* out) const {
  for (const auto& a : adapters_) {
    if (a->index != index) continue;
    for (const auto& c : a->conns) {
      if (c->handle != handle) continue;
      const size_t want = static_cast<size_t>(std::max(1, std::min<int>(seconds, kHistorySize)));
      const size_t n = std::min(want, c->hist_n);
      json t = json::array(), tx = json::array(), rx = json::array(), p50 = json::array(),
           p95 = json::array(), inf = json::array(), cred = json::array();
      for (size_t i = c->hist_n - n; i < c->hist_n; ++i) {
        const HistEntry& h = c->hist[(c->hist_head + i) % kHistorySize];
        t.push_back(h.t_ms);
        tx.push_back(static_cast<uint64_t>(h.tx_bps));
        rx.push_back(static_cast<uint64_t>(h.rx_bps));
        p50.push_back(std::isnan(h.p50) ? json(nullptr) : json(round2(h.p50)));
        p95.push_back(std::isnan(h.p95) ? json(nullptr) : json(round2(h.p95)));
        inf.push_back(h.in_flight);
        cred.push_back(h.credits_min < 0 ? json(nullptr) : json(h.credits_min));
      }
      *out = json{{"t", std::move(t)},          {"tx_bps", std::move(tx)},
                  {"rx_bps", std::move(rx)},    {"lat_p50", std::move(p50)},
                  {"lat_p95", std::move(p95)},  {"in_flight", std::move(inf)},
                  {"credits_min", std::move(cred)}};
      return true;
    }
  }
  return false;
}

bool Decoder::latency(int index, int handle, json* out) const {
  for (const auto& a : adapters_) {
    if (a->index != index) continue;
    for (const auto& c : a->conns) {
      if (c->handle != handle) continue;
      const auto& counts = c->lat_total.counts();
      json arr = json::array();
      for (int b = 0; b < LatencyHist::kBuckets; ++b) arr.push_back(counts[static_cast<size_t>(b)]);
      *out = json{{"bucket_ms", 1},
                  {"counts", std::move(arr)},
                  {"overflow", counts[LatencyHist::kBuckets]},
                  {"since", c->since_us / 1000},
                  {"lat_ms", lat_json(c->lat_total)}};
      return true;
    }
  }
  return false;
}

}  // namespace btb::hci
