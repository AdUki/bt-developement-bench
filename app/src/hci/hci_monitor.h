#pragma once

// btbenchd's HCI monitor (docs/contracts.md "HCI monitor API", docs/monitor.md).
//
// One reader thread owns the kernel's HCI monitor socket (or a btsnoop replay), decodes packet
// headers, and keeps 1 s windows of per-connection / per-L2CAP-channel throughput and TX latency
// (HCI TX → Number Of Completed Packets), AVDTP stream state and RTP loss/jitter. It publishes
// "hci.stats" once a second and "hci.event" per event through the Publish callback, and serves
// /api/hci/* and /api/capture* on the daemon's HTTP server.

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace httplib {
class Server;
}

namespace btb::hci {

struct Options {
  // Non-empty: feed the monitor from this btsnoop file (datalink 2001, or 1002 H4) instead of
  // the kernel socket, paced at the speed it was recorded.
  std::string replay_file;
  std::string capture_dir = "/data/btsnoop";
  std::string capture_unit = "btbench-btsnoop.service";

  // Beyond the contract (defaults are what the daemon wants):
  bool replay_fast = false;  // replay as fast as possible (tests)
  std::string systemctl = "systemctl";
  std::string btmon = "btmon";
};

using Publish = std::function<void(const std::string& topic, const nlohmann::json& data)>;

class Monitor {
 public:
  struct Impl;
  explicit Monitor(std::unique_ptr<Impl> impl);
  ~Monitor();  // stops the reader thread and ends live pcap streams
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;

  // False when the source could not be opened (no CAP_NET_RAW, no Bluetooth in the kernel, a
  // bad replay file); error() says why, and the /api/hci routes answer 503 with it.
  bool available() const;
  std::string error() const;

  // The bodies of the GET routes (see docs/contracts.md for the shapes).
  nlohmann::json stats() const;
  bool history(int index, int handle, int seconds, nlohmann::json* out) const;
  bool latency(int index, int handle, nlohmann::json* out) const;
  nlohmann::json events(uint64_t since) const;

  // Writes a user-logging frame (ident "btbench") to the kernel's HCI logging channel, so the mark
  // lands in btmon, the btsnoop ring and live pcap; and adds it to the events ring. *logged says
  // whether the kernel took it (in replay mode it never does).
  nlohmann::json mark(const std::string& text);

  // Replay only: blocks until the whole file was consumed (true) or the timeout expired.
  bool wait_replay_done(int timeout_ms) const;

  void stop();

  Impl& impl() { return *impl_; }

 private:
  std::unique_ptr<Impl> impl_;
};

// Never returns null: when the source fails the Monitor still exists (routes report the reason,
// capture management still works).
std::unique_ptr<Monitor> start(const Options& opts, Publish publish);

// /api/hci/{stats,history,latency,events,mark,live.pcap} and /api/capture*. The Monitor must
// outlive the server's request handling (stop the server before destroying it). A live pcap
// stream holds one HTTP worker thread for as long as the client stays connected.
void register_routes(httplib::Server& svr, Monitor& mon);

}  // namespace btb::hci
