#pragma once

// The HCI monitor's state machine: fed one monitor-channel packet at a time (from the kernel
// socket or a btsnoop replay), it tracks controllers, connections, L2CAP channels and AVDTP
// streams, and turns them into 1 s windows of throughput and TX latency. Modelled on bluez
// monitor/analyze.c (connection/channel bookkeeping, NOCP latency), with the L2CAP signalling of
// monitor/l2cap.c and the AVDTP/A2DP decoding of monitor/avdtp.c and monitor/a2dp.c.
//
// Not thread-safe: the Monitor owns one Decoder behind its mutex. Nothing here allocates per
// packet on the steady-state paths (ACL/SCO/ISO data, NOCP); allocation happens when a
// connection, channel, stream or event is created, and once a second when a window closes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "codec.h"
#include "histogram.h"

namespace btb::hci {

constexpr int64_t kWindowUs = 1000000;
constexpr size_t kHistorySize = 300;   // windows kept per connection for /api/hci/history
constexpr size_t kEventRing = 500;     // /api/hci/events
constexpr size_t kTxFifo = 128;        // TX timestamps awaiting NOCP, per connection
constexpr size_t kMaxChannels = 32;    // L2CAP channels tracked per connection
constexpr size_t kSigBuf = 1024;       // reassembly buffer for signalling SDUs, per direction

struct Event {
  uint64_t seq = 0;
  int64_t ts_ms = 0;
  int index = -1;
  int handle = -1;  // -1: not about one connection
  std::string kind;  // index | conn | disconn | l2cap | avdtp | mark
  std::string text;
  nlohmann::json data;  // optional structured detail (AVDTP codec, PSM, ...)
};

class Decoder {
 public:
  Decoder();
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  // One monitor packet: ts in µs since the epoch (SO_TIMESTAMP / btsnoop), index = controller,
  // opcode = MonOpcode, data = payload after the 6-byte monitor header.
  void packet(int64_t ts_us, uint16_t index, uint16_t opcode, const uint8_t* data, size_t len);

  // Closes every 1 s window that ended at or before now_us. Returns the number closed.
  int advance(int64_t now_us);
  // Closes the current window early (end of a replay), so its packets show up in stats.
  void flush(int64_t now_us);

  // A mark from POST /api/hci/mark: appended to the events ring. (The kernel echoes the logging
  // frame back on the monitor channel; that echo is recognised by its ident and not duplicated.)
  void add_mark(int64_t ts_us, const std::string& text);

  // Controller buffer sizes learned outside the packet stream (HCIGETDEVINFO when the monitor
  // starts after the controller was initialised).
  void set_acl_buffers(uint16_t index, uint16_t acl_mtu, uint16_t acl_pkts, uint16_t sco_mtu,
                       uint16_t sco_pkts);

  // ---- views (JSON shapes are those of docs/contracts.md) ----
  const nlohmann::json& stats() const { return stats_; }
  bool history(int index, int handle, int seconds, nlohmann::json* out) const;
  bool latency(int index, int handle, nlohmann::json* out) const;
  nlohmann::json events(uint64_t since) const;
  // Events created since the last call, for the hci.event topic.
  std::vector<Event> take_new_events();
  uint64_t last_seq() const { return next_seq_ - 1; }
  uint64_t windows_closed() const { return windows_closed_; }
  uint64_t packets() const { return packets_; }

  struct Adapter;
  struct Conn;
  struct Channel;

 private:
  Adapter* adapter(uint16_t index, bool create);
  Conn* conn(Adapter* a, uint16_t handle);
  Conn* new_conn(int64_t ts, Adapter* a, uint16_t handle, int type, const uint8_t* peer,
                 int peer_type, bool announce);
  void drop_conn(int64_t ts, Adapter* a, Conn* c, const char* why, int reason);
  void drop_all_conns(int64_t ts, Adapter* a, const char* why);

  void on_new_index(int64_t ts, uint16_t index, const uint8_t* d, size_t len);
  void on_command(int64_t ts, Adapter* a, const uint8_t* d, size_t len);
  void on_event(int64_t ts, Adapter* a, const uint8_t* d, size_t len);
  void on_cmd_complete(int64_t ts, Adapter* a, const uint8_t* d, size_t len);
  void on_le_meta(int64_t ts, Adapter* a, const uint8_t* d, size_t len);
  void on_nocp(int64_t ts, Adapter* a, const uint8_t* d, size_t len);
  void on_acl(int64_t ts, Adapter* a, bool tx, const uint8_t* d, size_t len);
  void on_sco_iso(int64_t ts, Adapter* a, bool tx, bool iso, const uint8_t* d, size_t len);
  void on_user_logging(int64_t ts, uint16_t index, const uint8_t* d, size_t len);

  void tx_packet(Adapter* a, Conn* c, int64_t ts, uint32_t bytes, Channel* ch);
  void rx_packet(Conn* c, uint32_t bytes, Channel* ch);

  Channel* channel_for_data(int64_t ts, Conn* c, bool tx, uint16_t cid);
  Channel* add_channel(Conn* c, uint16_t lcid, uint16_t rcid, uint16_t psm, bool le);
  void channel_opened(int64_t ts, Adapter* a, Conn* c, Channel* ch);
  void channel_closed(int64_t ts, Adapter* a, Conn* c, Channel* ch, const char* why);
  void sdu(int64_t ts, Adapter* a, Conn* c, Channel* ch, bool tx, const uint8_t* d, size_t len);
  void l2cap_sig(int64_t ts, Adapter* a, Conn* c, bool tx, bool le, const uint8_t* d, size_t len);
  void avdtp_sig(int64_t ts, Adapter* a, Conn* c, bool tx, const uint8_t* d, size_t len);
  void avdtp_media(int64_t ts, Conn* c, Channel* ch, bool tx, const uint8_t* d, size_t len);

  Event& emit(int64_t ts_us, int index, int handle, const char* kind, std::string text);
  void close_window(int64_t end_us);

  std::vector<std::unique_ptr<Adapter>> adapters_;
  std::vector<Event> events_;  // ring of kEventRing, oldest at events_head_
  size_t events_head_ = 0;
  uint64_t next_seq_ = 1;
  uint64_t published_seq_ = 0;
  int64_t window_start_us_ = -1;
  uint64_t windows_closed_ = 0;
  uint64_t packets_ = 0;
  uint32_t next_uid_ = 1;
  int64_t last_mark_ts_ = 0;
  std::string last_mark_;
  nlohmann::json stats_;
};

}  // namespace btb::hci
