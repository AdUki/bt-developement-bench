#include "context.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "btsnoop.h"
#include "wire.h"

namespace btb::hci {

namespace {

void put_be32(std::string* out, uint32_t v) {
  const char b[4] = {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
                     static_cast<char>(v)};
  out->append(b, 4);
}

std::string record(int64_t ts_us, uint16_t index, uint16_t mon, const uint8_t* d, size_t len) {
  std::string r;
  r.reserve(24 + len);
  const uint64_t ts = static_cast<uint64_t>(ts_us + kBtsnoopEpochDelta);
  put_be32(&r, static_cast<uint32_t>(len));
  put_be32(&r, static_cast<uint32_t>(len));
  put_be32(&r, static_cast<uint32_t>(index) << 16 | mon);
  put_be32(&r, 0);
  put_be32(&r, static_cast<uint32_t>(ts >> 32));
  put_be32(&r, static_cast<uint32_t>(ts));
  r.append(reinterpret_cast<const char*>(d), len);
  return r;
}

}  // namespace

void ContextJournal::keep(std::deque<Rec>* q, size_t cap, int64_t ts_us, uint16_t index, uint16_t mon,
                          const uint8_t* d, size_t len) {
  q->push_back(Rec{seq_++, record(ts_us, index, mon, d, len)});
  while (q->size() > cap) q->pop_front();
}

void ContextJournal::packet(int64_t ts_us, uint16_t index, uint16_t mon, const uint8_t* d, size_t len) {
  // The Classifier is what tells a signalling or AVDTP packet from media: it follows the links'
  // channels exactly as the indexer does. Ids are unused (no latency is wanted here).
  IndexEntry e;
  cls_.classify(ts_us, index, mon, d, len, 0, &e);
  const ContextKind k = context_kind(e);
  if (k == kCtxNone) return;
  const uint32_t link = static_cast<uint32_t>(index) << 16 | e.handle;
  switch (k) {
    case kCtxIndex:
      // A controller that goes takes its links with it. (Not New Index: the kernel repeats it for
      // every controller to each monitor socket that opens, btmon's at every ring file.)
      // A controller that closes (powered off: bluetoothd restarting, an adapter power cycle)
      // ends its links without a Disconnection Complete for each; the decoder drops them too.
      if (mon == kMonCloseIndex) {
        for (auto it = links_.begin(); it != links_.end();) {
          it = (it->first >> 16) == index ? links_.erase(it) : std::next(it);
        }
      }
      if (mon == kMonDelIndex) {
        index_.erase(index);
        for (auto it = links_.begin(); it != links_.end();) {
          it = (it->first >> 16) == index ? links_.erase(it) : std::next(it);
        }
        return;
      }
      keep(&index_[index], kMaxIndex, ts_us, index, mon, d, len);
      return;
    case kCtxLink:
      if (e.handle == kNoHandle) return;
      // Disconnection Complete: nothing of the link applies any more.
      if (mon == kMonEvent && e.evt == 0x05) {
        links_.erase(link);
        return;
      }
      links_.erase(link);  // a reused handle is a new link
      keep(&links_[link].conn, 4, ts_us, index, mon, d, len);
      return;
    case kCtxSig:
      keep(&links_[link].sig, kMaxSig, ts_us, index, mon, d, len);
      return;
    case kCtxAvdtp:
      keep(&links_[link].avdtp, kMaxAvdtp, ts_us, index, mon, d, len);
      return;
    case kCtxAtt:
      if (att_discovery(d, len)) keep(&links_[link].att, kMaxAtt, ts_us, index, mon, d, len);
      return;
    default:
      return;
  }
}

size_t ContextJournal::size() const {
  size_t n = 0;
  for (const auto& [i, q] : index_) n += q.size();
  for (const auto& [k, l] : links_) n += l.conn.size() + l.sig.size() + l.avdtp.size() + l.att.size();
  return n;
}

std::string ContextJournal::snapshot() const {
  // In the order they happened: btmon and the Classifier both follow state forward.
  std::vector<const Rec*> all;
  all.reserve(size());
  for (const auto& [i, q] : index_) {
    for (const Rec& r : q) all.push_back(&r);
  }
  for (const auto& [k, l] : links_) {
    for (const auto* q : {&l.conn, &l.sig, &l.avdtp, &l.att}) {
      for (const Rec& r : *q) all.push_back(&r);
    }
  }
  std::sort(all.begin(), all.end(), [](const Rec* a, const Rec* b) { return a->seq < b->seq; });
  static const char hdr[16] = {'b', 't', 's', 'n', 'o', 'o', 'p', 0, 0, 0, 0, 1, 0, 0, 0x07, static_cast<char>(0xd1)};
  std::string out(hdr, sizeof hdr);
  for (const Rec* r : all) out += r->bytes;
  return out;
}

void ContextJournal::each(
    const std::function<void(int64_t ts_us, uint16_t index, uint16_t mon, const uint8_t* d, size_t len)>& fn) const {
  const std::string all = snapshot();
  const auto* p = reinterpret_cast<const uint8_t*>(all.data());
  auto be32 = [](const uint8_t* b) { return uint32_t{b[0]} << 24 | uint32_t{b[1]} << 16 | uint32_t{b[2]} << 8 | b[3]; };
  for (size_t at = 16; at + 24 <= all.size();) {
    const uint32_t incl = be32(p + at + 4);
    if (at + 24 + incl > all.size()) break;
    const uint32_t flags = be32(p + at + 8);
    const uint64_t raw = uint64_t{be32(p + at + 16)} << 32 | be32(p + at + 20);
    fn(static_cast<int64_t>(raw) - kBtsnoopEpochDelta, static_cast<uint16_t>(flags >> 16),
       static_cast<uint16_t>(flags & 0xffff), p + at + 24, incl);
    at += 24 + incl;
  }
}

size_t ContextJournal::replay(const std::string& path) {
  BtsnoopReader r;
  std::string err;
  if (!r.open(path, &err)) return 0;
  SnoopPacket p;
  size_t n = 0;
  while (r.read(&p)) {
    packet(p.ts_us, p.index, p.opcode, p.data, p.len);
    ++n;
  }
  return n;
}

bool ContextJournal::write(const std::string& path, std::string* err) const {
  const std::string tmp = path + ".new";
  FILE* f = std::fopen(tmp.c_str(), "wbe");
  if (!f) {
    *err = tmp + ": " + std::strerror(errno);
    return false;
  }
  const std::string s = snapshot();
  const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
  if (std::fclose(f) != 0 || !ok) {
    *err = tmp + ": " + std::strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    *err = path + ": " + std::strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace btb::hci
