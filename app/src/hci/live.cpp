// The live packet view: a ring of classified packets fed by the reader thread, and per-viewer
// streams that filter and summarise it (live.h).

#include "live.h"

#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "wire.h"

namespace btb::hci {

using nlohmann::json;

namespace {

bool is_tx_data(uint8_t mon) { return mon == kMonAclTx || mon == kMonScoTx || mon == kMonIsoTx; }

bool uses_latency(const Filter& f) {
  for (const auto& t : f.terms) {
    if (t.kind == Filter::Term::kCmp && t.field == 'L') return true;
    if (t.kind == Filter::Term::kIs && (t.values[0] & 8)) return true;
  }
  return false;
}

// One packet line: packet_json()'s object plus "seq", written by hand. On the Zero W building a
// json object per packet (a std::map of 15 nodes) and dumping it is most of a stream's cost.
void append_escaped(std::string* o, const std::string& s) {
  static const char* hex = "0123456789abcdef";
  o->push_back('"');
  for (const char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') {
      o->push_back('\\');
      o->push_back(ch);
    } else if (u < 0x20) {
      *o += "\\u00";
      o->push_back(hex[u >> 4]);
      o->push_back(hex[u & 15]);
    } else {
      o->push_back(ch);
    }
  }
  o->push_back('"');
}

void append_uint(std::string* o, uint64_t v) {
  char b[24];
  char* p = b + sizeof(b);
  do {
    *--p = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  o->append(p, static_cast<size_t>(b + sizeof(b) - p));
}

// v / 10^digits as a decimal with exactly that many fraction digits: µs as ms or s, with no
// floating point (printf's %f was the biggest single cost of a line).
void append_fixed(std::string* o, int64_t v, int digits) {
  if (v < 0) {
    o->push_back('-');
    v = -v;
  }
  int64_t div = 1;
  for (int i = 0; i < digits; ++i) div *= 10;
  append_uint(o, static_cast<uint64_t>(v / div));
  o->push_back('.');
  char f[8];
  int64_t r = v % div;
  for (int i = digits - 1; i >= 0; --i) {
    f[i] = static_cast<char>('0' + r % 10);
    r /= 10;
  }
  o->append(f, static_cast<size_t>(digits));
}

void append_packet_line(std::string* o, const IndexEntry& e, uint64_t seq, int64_t t_us,
                        const std::string& summary) {
  auto field = [o](const char* name) {
    o->push_back(',');
    o->push_back('"');
    *o += name;
    *o += "\":";
  };
  auto num_or_null = [o](bool have, unsigned v) {
    if (have) append_uint(o, v);
    else *o += "null";
  };
  *o += "{\"seq\":";
  append_uint(o, seq);
  field("n");
  append_uint(o, seq);
  field("ts_ms");
  append_fixed(o, e.ts_us, 3);
  field("t");
  append_fixed(o, t_us, 6);
  field("index");
  num_or_null(e.index != kNoIndex, e.index);
  field("dir");
  *o += e.flags & kFlagTx ? "\"tx\"" : e.flags & kFlagRx ? "\"rx\"" : "\"\"";
  field("type");
  o->push_back('"');
  *o += pkt_type_name(pkt_type_of(e.mon));
  o->push_back('"');
  field("proto");
  if (const char* proto = proto_name(e.proto)) {
    o->push_back('"');
    *o += proto;
    o->push_back('"');
  } else {
    *o += "null";
  }
  field("handle");
  num_or_null(e.handle != kNoHandle, e.handle);
  field("cid");
  num_or_null(e.cid != 0, e.cid);
  field("psm");
  num_or_null(e.psm != 0, e.psm);
  field("len");
  append_uint(o, e.len);
  field("lat_ms");
  if (e.lat_us == kNoLat) *o += "null";
  else append_fixed(o, e.lat_us, 3);
  if (e.flags & kFlagErr) *o += ",\"err\":true";
  if (e.flags & kFlagMark) *o += ",\"mark\":true";
  if (e.flags & kFlagCont) *o += ",\"cont\":true";
  field("summary");
  append_escaped(o, summary);
  *o += "}\n";
}

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

}  // namespace

// The coarse clock: it only times the latency hold (a second), and it is read for every packet.
// It costs a memory read where the precise one may be a clock-source access (an HPET on some PCs).
int64_t LiveView::mono_us() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

LiveView::LiveView() {
  // A NOCP completes a packet pushed earlier; its id is the low 32 bits of its sequence number,
  // which with a power-of-two ring also names its slot. A slot that was reused since says so by
  // its own sequence number, and the late latency is then nobody's.
  static_assert((kRing & (kRing - 1)) == 0, "kRing must divide 2^32");
  cls_.on_latency = [this](uint32_t id, uint32_t lat_us) {
    Slot& s = ring_[id % kRing];
    if (static_cast<uint32_t>(s.seq) == id) s.e.lat_us = lat_us;
  };
}

LiveView::~LiveView() { close_all(); }

void LiveView::seed(const std::vector<LinkSeed>& links) {
  std::lock_guard<std::mutex> lk(m_);
  cls_.seed(links);
  needs_seed_.store(false);
}

void LiveView::push(int64_t ts_us, uint16_t index, uint16_t opcode, const uint8_t* d, size_t len) {
  std::lock_guard<std::mutex> lk(m_);
  if (ring_.empty()) ring_.resize(kRing);
  const uint64_t seq = head_;
  Slot& s = ring_[seq % kRing];
  cls_.classify(ts_us, index, opcode, d, len, static_cast<uint32_t>(seq), &s.e);
  s.seq = seq;
  s.arrived_us = mono_us();
  s.len = static_cast<uint16_t>(std::min<size_t>(len, 0xffff));
  // assign() keeps the vector's capacity: after the first lap of the ring nothing is allocated.
  s.bytes.assign(d, d + std::min(len, kMaxBytes));
  if (seq == 1) t0_us_ = ts_us;
  ++head_;
  if (waiters_) data_cv_.notify_all();
}

LiveView::ClientPtr LiveView::subscribe(const Filter& f, int64_t since) {
  std::lock_guard<std::mutex> lk(m_);
  if (closed_ || static_cast<int>(clients_.size()) >= kMaxSubscribers) return nullptr;
  auto c = std::make_shared<Client>();
  c->filter = f;
  c->lat_hold = uses_latency(f);
  const uint64_t oldest = head_ > kRing ? head_ - kRing : 1;
  c->cursor = since < 0 ? head_ : std::max<uint64_t>(static_cast<uint64_t>(since) + 1, oldest);
  c->cursor = std::min(c->cursor, head_);
  clients_.push_back(c);
  // The flag before the count: the reader sees the seed request no later than the viewer.
  if (clients_.size() == 1) needs_seed_.store(true);
  count_.store(static_cast<int>(clients_.size()));
  return c;
}

void LiveView::unsubscribe(const ClientPtr& c) {
  std::lock_guard<std::mutex> lk(m_);
  clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
  count_.store(static_cast<int>(clients_.size()));
  c->closed = true;
}

void LiveView::close_all() {
  {
    std::lock_guard<std::mutex> lk(m_);
    closed_ = true;
    for (auto& c : clients_) c->closed = true;
  }
  data_cv_.notify_all();
  pace_cv_.notify_all();
}

bool LiveView::wait(Client& c) {
  using clock = std::chrono::steady_clock;
  std::unique_lock<std::mutex> lk(m_);
  if (!c.started) return !c.closed;  // the first status line goes out at once
  pace_cv_.wait_until(lk, c.last_batch + std::chrono::milliseconds(kBatchMs),
                      [&] { return c.closed; });
  const auto beat = c.last_status + std::chrono::milliseconds(kHeartbeatMs);
  if (!c.closed && head_ <= c.cursor && clock::now() < beat) {
    ++waiters_;
    data_cv_.wait_until(lk, beat, [&] { return c.closed || head_ > c.cursor; });
    --waiters_;
  }
  return !c.closed;
}

void LiveView::drain(Client& c, int64_t now_us, std::string* out) {
  const auto now_tp = std::chrono::steady_clock::now();
  c.last_batch = now_tp;
  // A stream's first batch is the backlog it asked for with since= (a viewer re-filtering what
  // the board holds): it may go through the whole ring once. Every later batch is the traffic of
  // one batch period, where the limits keep a flood from costing more than the board can spare.
  const bool backlog = !c.started;
  const size_t max_scan = backlog ? kRing : kMaxScan;
  const size_t max_send = backlog ? kRing : kMaxSend;
  const size_t max_decode = backlog ? kRing : kMaxDecode;
  size_t ncand = 0;
  uint64_t head;
  int64_t t0;
  {
    std::lock_guard<std::mutex> lk(m_);
    head = head_;
    t0 = t0_us_;
    const uint64_t oldest = head_ > kRing ? head_ - kRing : 1;
    if (c.cursor < oldest) {
      c.dropped += oldest - c.cursor;
      c.cursor = oldest;
    }
    if (head - c.cursor > max_scan) {
      c.dropped += head - max_scan - c.cursor;
      c.cursor = head - max_scan;
    }
    // Pass 1, index terms only: which packets match. Cheap, so it runs under the lock.
    c.match_seqs.clear();
    for (; c.cursor < head; ++c.cursor) {
      const Slot& s = ring_[c.cursor % kRing];
      if (c.lat_hold && is_tx_data(s.e.mon) && s.e.lat_us == kNoLat &&
          now_us - s.arrived_us < kLatHoldUs) {
        break;  // its NOCP may still come; look again next batch
      }
      ++c.scanned;
      const double t = static_cast<double>(s.e.ts_us - t0) / 1e6;
      if (c.filter.match_index(s.e, t, static_cast<uint32_t>(s.seq))) c.match_seqs.push_back(s.seq);
    }
    // Only the newest matches can be sent; a text filter must decode its candidates to know, so
    // it gets a larger (but still bounded) window, and what is beyond it was never examined.
    const size_t keep = c.filter.has_text ? max_decode : max_send;
    size_t first = 0;
    if (c.match_seqs.size() > keep) {
      first = c.match_seqs.size() - keep;
      if (c.filter.has_text) c.dropped += first;
      else c.skipped += first;
    }
    // Pass 2: copy those out, so the decoding happens without holding up the reader thread.
    ncand = c.match_seqs.size() - first;
    if (c.cands.size() < ncand) c.cands.resize(ncand);
    for (size_t i = 0; i < ncand; ++i) {
      const Slot& s = ring_[c.match_seqs[first + i] % kRing];
      Client::Cand& k = c.cands[i];
      k.seq = s.seq;
      k.e = s.e;
      k.bytes.assign(s.bytes.begin(), s.bytes.end());
    }
  }

  // Decode, text-filter and serialize, the newest max_send kept.
  std::string& body = c.body;
  std::vector<size_t>& starts = c.starts;
  body.clear();
  starts.clear();
  Dissection ds;
  for (size_t i = 0; i < ncand; ++i) {
    const Client::Cand& k = c.cands[i];
    dissect(k.e, k.bytes.data(), k.bytes.size(), false, &ds);
    if (c.filter.has_text &&
        !c.filter.match_text(k.e, static_cast<double>(k.e.ts_us - t0) / 1e6, static_cast<uint32_t>(k.seq),
                             lower(ds.summary)))
      continue;
    starts.push_back(body.size());
    append_packet_line(&body, k.e, k.seq, k.e.ts_us - t0, ds.summary);
  }
  size_t from = 0;
  if (starts.size() > max_send) {
    from = starts.size() - max_send;
    c.skipped += from;
  }

  // A status line first when anything was dropped or skipped since the last one (the client
  // shows the gap where it happened), at the start, and as a heartbeat: it is also how a stream
  // to a vanished client fails its next write.
  const bool changed = c.dropped != c.reported_dropped || c.skipped != c.reported_skipped;
  if (!c.started || changed ||
      now_tp - c.last_status >= std::chrono::milliseconds(kHeartbeatMs)) {
    json st{{"seq", head - 1}, {"cursor", c.cursor}, {"scanned", c.scanned}, {"sent", c.sent},
            {"dropped", c.dropped}, {"skipped", c.skipped}};
    if (!c.started) {
      st["filter"] = c.filter.key;
      st["ring"] = kRing;
      st["batch_ms"] = kBatchMs;
      st["lat_hold"] = c.lat_hold;
      c.started = true;
    }
    *out += json{{"status", std::move(st)}}.dump();
    out->push_back('\n');
    c.reported_dropped = c.dropped;
    c.reported_skipped = c.skipped;
    c.last_status = now_tp;
  }
  if (from < starts.size()) out->append(body, starts[from], std::string::npos);
  c.sent += starts.size() - from;
  // The backlog may have grown the scratch to a whole ring's worth (a MiB or more, per stream):
  // give back what a steady batch never needs.
  if (c.cands.size() > kMaxDecode) {
    c.cands.resize(kMaxDecode);
    c.cands.shrink_to_fit();
    std::string().swap(c.body);
  }
}

bool LiveView::detail(uint64_t seq, json* out) {
  IndexEntry e;
  std::vector<uint8_t> bytes;
  uint16_t len;
  int64_t t0;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (ring_.empty() || seq == 0 || seq >= head_) return false;
    const Slot& s = ring_[seq % kRing];
    if (s.seq != seq) return false;
    e = s.e;
    bytes = s.bytes;
    len = s.len;
    t0 = t0_us_;
  }
  const double t = static_cast<double>(e.ts_us - t0) / 1e6;
  std::vector<json> extra{json{{"name", "Since the live view's first packet"},
                               {"value", std::to_string(t) + " s"}}};
  if (bytes.size() < len) {
    extra.push_back(json{{"name", "Kept"},
                         {"value", "the first " + std::to_string(bytes.size()) + " of " +
                                       std::to_string(len) + " bytes"}});
  }
  *out = packet_detail_json(e, seq, t, bytes.data(), bytes.size(), extra);
  (*out)["seq"] = seq;
  return true;
}

uint64_t LiveView::last_seq() const {
  std::lock_guard<std::mutex> lk(m_);
  return head_ - 1;
}

}  // namespace btb::hci
