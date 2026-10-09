#pragma once

// Live pcap for GET /api/hci/live.pcap. Linktype 254 (DLT_BLUETOOTH_LINUX_MONITOR): each record's
// data is a 4-byte pseudo-header — adapter index and monitor opcode, both big-endian (libpcap
// pcap/bluetooth.h, pcap_bluetooth_linux_monitor_header) — followed by the monitor payload, so
// Wireshark decodes it exactly like a btmon capture.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace btb::hci {

constexpr uint32_t kLinktypeBtMonitor = 254;

// 24-byte pcap global header (microsecond timestamps, host byte order as the magic declares).
std::string pcap_global_header();
// Appends one record (16-byte record header + pseudo-header + payload).
void pcap_append_record(std::string* out, int64_t ts_us, uint16_t index, uint16_t opcode,
                        const uint8_t* data, size_t len);

// Fan-out of monitor packets to live pcap streams. The reader thread pushes; each HTTP stream
// drains its own queue. A stream that cannot keep up (slow Wi-Fi to Wireshark) loses its oldest
// data rather than holding the reader: queues are bounded and drop whole chunks of records.
class PcapFeed {
 public:
  static constexpr size_t kChunk = 64 * 1024;
  static constexpr size_t kMaxQueued = 1024 * 1024;  // per subscriber
  static constexpr int kMaxSubscribers = 2;

  struct Sub {
    struct Chunk {
      std::string bytes;
      uint32_t records = 0;
    };
    std::mutex m;
    std::condition_variable cv;
    std::string header;  // the global header, sent ahead of everything else
    std::deque<Chunk> q;
    size_t queued = 0;
    uint64_t sent_records = 0;
    uint64_t dropped_records = 0;
    bool closed = false;
  };
  using SubPtr = std::shared_ptr<Sub>;

  // nullptr when kMaxSubscribers streams are already open. The global header is queued first.
  SubPtr subscribe();
  void unsubscribe(const SubPtr& s);
  // Cheap check for the reader thread: no subscribers, no work.
  bool active() const { return count_.load(std::memory_order_relaxed) > 0; }
  void push(int64_t ts_us, uint16_t index, uint16_t opcode, const uint8_t* data, size_t len);
  void close_all();

  // Waits up to timeout_ms for queued data and moves it into *out (which may stay empty on
  // timeout). Returns false once the feed was closed and the queue is drained.
  static bool take(Sub& s, std::string* out, int timeout_ms);

 private:
  std::mutex m_;
  std::vector<SubPtr> subs_;
  std::atomic<int> count_{0};
};

}  // namespace btb::hci
