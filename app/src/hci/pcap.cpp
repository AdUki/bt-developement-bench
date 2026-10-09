#include "pcap.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "wire.h"

namespace btb::hci {

namespace {

void put_host32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put_host16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }

}  // namespace

std::string pcap_global_header() {
  uint8_t h[24];
  put_host32(h, 0xa1b2c3d4);  // µs resolution; written in host order, the reader swaps if needed
  put_host16(h + 4, 2);
  put_host16(h + 6, 4);
  put_host32(h + 8, 0);  // thiszone
  put_host32(h + 12, 0);  // sigfigs
  put_host32(h + 16, 65535 + 4);  // snaplen: the largest monitor payload plus the pseudo-header
  put_host32(h + 20, kLinktypeBtMonitor);
  return std::string(reinterpret_cast<const char*>(h), sizeof(h));
}

void pcap_append_record(std::string* out, int64_t ts_us, uint16_t index, uint16_t opcode,
                        const uint8_t* data, size_t len) {
  uint8_t h[20];
  const uint32_t caplen = static_cast<uint32_t>(len + 4);
  put_host32(h, static_cast<uint32_t>(ts_us / 1000000));
  put_host32(h + 4, static_cast<uint32_t>(ts_us % 1000000));
  put_host32(h + 8, caplen);
  put_host32(h + 12, caplen);
  put_be16(h + 16, index);
  put_be16(h + 18, opcode);
  out->append(reinterpret_cast<const char*>(h), sizeof(h));
  out->append(reinterpret_cast<const char*>(data), len);
}

PcapFeed::SubPtr PcapFeed::subscribe() {
  std::lock_guard<std::mutex> lk(m_);
  if (static_cast<int>(subs_.size()) >= kMaxSubscribers) return nullptr;
  auto s = std::make_shared<Sub>();
  s->header = pcap_global_header();
  subs_.push_back(s);
  count_.store(static_cast<int>(subs_.size()), std::memory_order_relaxed);
  return s;
}

void PcapFeed::unsubscribe(const SubPtr& s) {
  std::lock_guard<std::mutex> lk(m_);
  subs_.erase(std::remove(subs_.begin(), subs_.end(), s), subs_.end());
  count_.store(static_cast<int>(subs_.size()), std::memory_order_relaxed);
}

void PcapFeed::push(int64_t ts_us, uint16_t index, uint16_t opcode, const uint8_t* data,
                    size_t len) {
  std::lock_guard<std::mutex> lk(m_);
  for (auto& s : subs_) {
    {
      std::lock_guard<std::mutex> sl(s->m);
      if (s->closed) continue;
      // Records are appended to the newest chunk until it is full, so the queue costs one
      // allocation per 64 KiB of traffic, not one per packet.
      if (s->q.empty() || s->q.back().bytes.size() + len + 20 > kChunk) {
        Sub::Chunk c;
        c.bytes.reserve(std::max(kChunk, len + 20));
        s->q.push_back(std::move(c));
      }
      Sub::Chunk& c = s->q.back();
      const size_t before = c.bytes.size();
      pcap_append_record(&c.bytes, ts_us, index, opcode, data, len);
      ++c.records;
      s->queued += c.bytes.size() - before;
      // Drop the oldest whole chunks; the header is kept apart and always goes out first.
      while (s->queued > kMaxQueued && s->q.size() > 1) {
        s->queued -= s->q.front().bytes.size();
        s->dropped_records += s->q.front().records;
        s->q.pop_front();
      }
    }
    s->cv.notify_one();
  }
}

void PcapFeed::close_all() {
  std::lock_guard<std::mutex> lk(m_);
  for (auto& s : subs_) {
    {
      std::lock_guard<std::mutex> sl(s->m);
      s->closed = true;
    }
    s->cv.notify_all();
  }
}

bool PcapFeed::take(Sub& s, std::string* out, int timeout_ms) {
  out->clear();
  std::unique_lock<std::mutex> lk(s.m);
  s.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                [&] { return s.closed || !s.q.empty() || !s.header.empty(); });
  if (!s.header.empty()) out->swap(s.header);
  if (s.q.empty()) return !s.closed || !out->empty();
  if (s.q.size() == 1 && out->empty()) {
    // Hand the buffer over rather than copying it.
    s.sent_records += s.q.front().records;
    out->swap(s.q.front().bytes);
    s.q.clear();
  } else {
    for (auto& c : s.q) {
      out->append(c.bytes);
      s.sent_records += c.records;
    }
    s.q.clear();
  }
  s.queued = 0;
  return true;
}

}  // namespace btb::hci
