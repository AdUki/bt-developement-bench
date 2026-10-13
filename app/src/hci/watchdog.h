#pragma once

// A controller that stops answering over HCI is reset. Seen on a Zero W's BCM43430 (UART) in the
// middle of A2DP: its radio kept the link up, but it sent nothing more to the host — no events,
// no completed packets — so every command timed out (adapter settings, connect, disconnect) and
// the kernel only noticed minutes later, until a reboot. Nothing in the kernel resets a UART
// controller; rebinding its driver does (hci_bcm powers the chip down and loads its firmware
// again). BlueZ then shows the adapter afresh, and the bench restores its settings and calls its
// devices back (bluetooth.cpp restore_adapters).
//
// The HCI monitor says which controllers were sent something and have answered nothing for a few
// seconds; such a one is asked something any controller answers at once (Read Local Version).
// A peer gone out of range also leaves its data unanswered, until the link's supervision timeout,
// but the controller still answers the question: only one that does not is reset.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace btb::hci {

// Read Local Version Information over a raw HCI socket, answered within timeout_ms. False with
// the reason (a timeout: "no answer in N ms").
bool probe_controller(uint16_t index, int timeout_ms, std::string* err);

// Unbinds hciN's device from its driver and binds it again (sysfs). *what says which, e.g.
// "hci_uart_bcm serial0-0". False with the reason.
bool rebind_controller(uint16_t index, std::string* what, std::string* err);

class ControllerWatchdog {
 public:
  using Silent = std::function<std::vector<uint16_t>(int64_t for_ms)>;
  using Note = std::function<void(const std::string&)>;  // a reset, said in the HCI capture
  ControllerWatchdog(Silent silent, Note note) : silent_(std::move(silent)), note_(std::move(note)) {}
  ~ControllerWatchdog() { stop(); }

  void start();
  void stop();

  static constexpr int64_t kSilentMs = 5000;
  static constexpr int kProbeMs = 5000;  // the kernel may first time out a command of its own (2 s)
  static constexpr int64_t kResetGapMs = 60000;  // at most one reset a minute per controller
  static constexpr int kMaxResets = 5;           // then left alone, until btbenchd restarts

 private:
  void run();

  Silent silent_;
  Note note_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::map<uint16_t, int64_t> reset_at_;  // index → when it was last reset (ms, monotonic)
  std::map<uint16_t, int> resets_;
};

}  // namespace btb::hci
