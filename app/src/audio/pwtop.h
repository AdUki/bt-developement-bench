#pragma once

// PipeWire's own timing, from `pw-top -b` (its profiler data, one table a second): per driver the
// quantum and rate, how long the graph cycle waited and worked, that as a share of the quantum
// (the DSP load), and the xruns; per follower its own share. Published on the "pipewire" topic
// for the Graphs tab and Audio → PipeWire, and only while someone is subscribed: pw-top is a process
// of its own, and on an idle Zero W it is not started at all.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace btb::audio {

// One row of pw-top's batch table (src/tools/pw-top.c print_node()).
struct PwTopNode {
  char state = '?';        // R running, I idle, S suspended, C creating, E error, T/t transport
  uint32_t id = 0;
  uint32_t quant = 0;      // driver: the quantum in samples; follower: its latency numerator
  uint32_t rate = 0;       // driver: the rate; follower: its latency denominator
  double wait_us = -1;     // -1: not measured (---, +++)
  double busy_us = -1;
  double wait_q = -1;      // as a share of the quantum
  double busy_q = -1;
  uint32_t err = 0;        // xruns since pw-top started
  std::string format;      // "S16LE 2 48000", "" when not active
  std::string name;
  bool follower = false;   // " + " (or " = ", asynchronous) in front of the name: driven by the driver above
};

// One line of `pw-top -b`; false for the header and anything else.
bool parse_pwtop_line(const std::string& line, PwTopNode* out);

// A table as the API serves it (docs/api.md "PipeWire"): drivers with their followers under them,
// and per node the xruns since the previous table (`prev` keeps the counts, by node id).
nlohmann::json pwtop_json(const std::vector<PwTopNode>& rows, int64_t ts_ms,
                          std::map<uint32_t, uint32_t>* prev);

class PwTop {
 public:
  using HasSubscribers = std::function<bool()>;
  using Publish = std::function<void(const nlohmann::json&)>;
  PwTop(HasSubscribers has, Publish pub) : has_(std::move(has)), pub_(std::move(pub)) {}
  ~PwTop() { stop(); }

  void start();
  void stop();
  // The last table, or null if none was read in the last few seconds (nobody watching).
  nlohmann::json last() const;

 private:
  void run();
  // One pw-top for as long as someone watches; false if it could not start or died at once.
  bool run_once(std::string* err);

  HasSubscribers has_;
  Publish pub_;
  mutable std::mutex m_;
  nlohmann::json last_;
  int64_t last_at_ms_ = 0;
  std::string error_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace btb::audio
