#pragma once

// Monitor's internals, shared by monitor.cpp (threads, sources) and routes.cpp (HTTP).

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "btsnoop.h"
#include "capture.h"
#include "context.h"
#include "decoder.h"
#include "hci_monitor.h"
#include "packet_view.h"
#include "pcap.h"

namespace btb::hci {

struct Monitor::Impl {
  Impl(const Options& o, Publish p);
  ~Impl();

  bool start_source();
  void stop();
  void run_live();
  // The capture ring's directory, watched for new files (inotify) so each gets its context file
  // (context.h). Without a directory to watch there is no ring, and the journal is not kept.
  void watch_captures();
  void on_capture_events();
  void seed_journal();
  void run_replay();
  void query_devinfo(uint16_t index);
  void publish_pending(bool stats_closed);
  nlohmann::json stats_locked() const;
  nlohmann::json do_mark(const std::string& text);

  const Options opts;
  const Publish publish;
  // Shared with HTTP handlers that may outlive a request's view of the Monitor (a pcap stream's
  // releaser runs when the connection ends).
  const std::shared_ptr<PcapFeed> feed;
  const std::shared_ptr<Capture> capture;
  // The packet view of the capture files: indexes cached across requests, own locking.
  const std::shared_ptr<PacketStore> packets;
  // A replay has no btmon writing the ring, so the monitor records what it replays into the
  // capture directory itself (opts.replay_record): the packet view's live end then shows the
  // replay as it would a board's traffic. Touched by the replay thread only.
  BtsnoopWriter rec;
  std::string rec_name;  // "" while nothing is recorded

  // Everything the reader thread and the HTTP handlers both touch is under mu.
  mutable std::mutex mu;
  mutable std::condition_variable done_cv;
  Decoder dec;
  ContextJournal journal;
  int ino_fd = -1;
  int64_t ino_retry_us = 0;
  bool replay = false;
  bool replay_done = false;
  uint32_t kernel_drops = 0;

  std::string error;  // set once, before the reader thread starts

  BtsnoopReader reader;
  int mon_fd = -1;
  int wake_fd[2] = {-1, -1};
  std::thread thread;

  std::mutex stop_mu;
  std::condition_variable stop_cv;
  bool stopping = false;

  std::mutex log_mu;
  int log_fd = -1;
};

}  // namespace btb::hci
