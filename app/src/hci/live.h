#pragma once

// The live packet view (docs/monitor.md "Live packets"): the monitor's packets as they happen,
// summarised and filtered per viewer by the capture view's code and filter language, and streamed
// as NDJSON over a chunked HTTP response (GET /api/hci/live).
//
// Why a chunked HTTP stream per viewer and not a WebSocket topic: the hub's topics are broadcast
// (one message, serialized once, for everyone), while here each viewer has its own filter, its
// own position in the stream and its own pace. A stream carries all three in its URL and in the
// worker that serves it; a viewer changes its filter by reopening the stream (with since= so it
// re-filters what it already saw), and curl can read it (bench mon packets). The cost is one HTTP
// worker per open stream, as for live.pcap: the pool grows for it.
//
// Sized for one ARMv6 core:
//   - Nothing at all while nobody watches: the reader thread checks active(), a 32-bit atomic,
//     and goes on. The channel tracking then restarts from the Decoder's view (seed()) when the
//     next viewer arrives, so channels opened in between still have their protocol.
//   - While someone watches, the reader thread only classifies each packet (headers, the same
//     Classifier a capture's index uses) and copies it into a ring of the last kRing packets.
//     No decoding and no JSON on that thread, and no allocation once the ring's buffers grew.
//   - Each stream's worker wakes at most every kBatchMs. It filters the ring from its cursor on
//     the index terms under the lock, then decodes summaries (for text terms, and for the lines
//     it sends) outside it. A client that cannot keep up is a cursor that falls behind: what the
//     ring overwrote before it got there is counted as dropped, oldest first, and the reader
//     never waits for anyone.
//   - At most kMaxSend lines go out per batch; when more match, the newest are sent and the rest
//     counted as skipped. A2DP alone is ~300 packets/s; an LE scan can be more. (A stream's first
//     batch is the backlog since= asked for, up to the whole ring.)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "link_seed.h"
#include "packet_view.h"

namespace btb::hci {

class LiveView {
 public:
  static constexpr size_t kRing = 2048;      // packets kept (a power of two: see on_latency)
  static constexpr size_t kMaxBytes = 2048;  // bytes kept of each (HCI ACL tops out at 1025 here)
  static constexpr int kMaxSubscribers = 4;
  static constexpr int kBatchMs = 150;
  static constexpr int kHeartbeatMs = 2000;   // a status line at least this often
  static constexpr size_t kMaxScan = 1000;    // packets one batch looks at; older ones: dropped
  static constexpr size_t kMaxSend = 200;     // packet lines per batch; more matches: skipped
  static constexpr size_t kMaxDecode = 400;   // text filters: summaries decoded per batch
  static constexpr int64_t kLatHoldUs = 1000000;  // see Client::lat_hold

  LiveView();
  ~LiveView();
  LiveView(const LiveView&) = delete;
  LiveView& operator=(const LiveView&) = delete;

  // ---- the reader thread (under the Monitor's mutex, so the seed matches the stream) ----

  bool active() const { return count_.load(std::memory_order_relaxed) > 0; }
  // True after a viewer arrived while nobody was watching: the Classifier missed packets.
  bool needs_seed() const { return needs_seed_.load(); }
  void seed(const std::vector<LinkSeed>& links);
  void push(int64_t ts_us, uint16_t index, uint16_t opcode, const uint8_t* d, size_t len);

  // ---- streams ----

  struct Client {
    Filter filter;
    // The filter has a latency term (lat>, is:lat). A TX packet's latency is known only when its
    // NOCP arrives, so such a stream looks at a TX data packet once it has one, or after
    // kLatHoldUs without — and the stream is up to that much behind.
    bool lat_hold = false;
    uint64_t cursor = 1;  // the next sequence number to look at
    uint64_t scanned = 0, sent = 0, dropped = 0, skipped = 0;
    bool started = false;  // the first status line went out
    bool closed = false;
    uint64_t reported_dropped = 0, reported_skipped = 0;
    std::chrono::steady_clock::time_point last_batch{}, last_status{};
    // drain()'s scratch, reused so a steady stream allocates nothing per batch.
    std::vector<uint64_t> match_seqs;
    struct Cand {
      uint64_t seq = 0;
      IndexEntry e{};
      std::vector<uint8_t> bytes;
    };
    std::vector<Cand> cands;
    std::string body;
    std::vector<size_t> starts;
  };
  using ClientPtr = std::shared_ptr<Client>;

  // since < 0: from the next packet on; since >= 0: the packets after that sequence number that
  // are still in the ring (0: all of them). Null when kMaxSubscribers streams are open.
  ClientPtr subscribe(const Filter& f, int64_t since);
  void unsubscribe(const ClientPtr& c);
  void close_all();

  // Blocks until the client's next batch is due: kBatchMs after its last one, and then until a
  // packet arrives or a status line is due. False once the view was closed.
  bool wait(Client& c);
  // The client's next batch as NDJSON lines (possibly none). now_us is the monotonic clock
  // (mono_us()); tests pass a later one to let held packets go.
  void drain(Client& c, int64_t now_us, std::string* out);

  // One packet from the ring, decoded as the capture view's detail. False if it is not (or no
  // longer) there.
  bool detail(uint64_t seq, nlohmann::json* out);

  uint64_t last_seq() const;  // 0: none yet
  static int64_t mono_us();  // CLOCK_MONOTONIC_COARSE, µs

 private:
  struct Slot {
    uint64_t seq = 0;
    int64_t arrived_us = 0;  // monotonic, for the latency hold
    uint16_t len = 0;        // as received; bytes keeps at most kMaxBytes of it
    IndexEntry e{};
    std::vector<uint8_t> bytes;
  };

  mutable std::mutex m_;
  std::condition_variable data_cv_;  // a packet arrived (notified only while someone waits)
  std::condition_variable pace_cv_;  // only the shutdown wakes a client sleeping out its pace
  int waiters_ = 0;
  std::vector<Slot> ring_;  // allocated by the first packet anyone watched
  uint64_t head_ = 1;       // the next sequence number
  int64_t t0_us_ = 0;       // the first packet's timestamp: t terms count from it
  Classifier cls_;
  std::vector<ClientPtr> clients_;
  bool closed_ = false;
  std::atomic<int> count_{0};
  std::atomic<bool> needs_seed_{true};
};

}  // namespace btb::hci
