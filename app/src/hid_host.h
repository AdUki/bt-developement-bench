#pragma once

// The board as a HID host (HoGP's host role, and classic HID too): what Bluetooth keyboards,
// remotes, mice and game pads connected to it are, and what they send.
//
// bluetoothd does the protocol: its input plugin (HoG for LE, HIDP for BR/EDR) reads a paired
// device's report map and creates a kernel HID device through uhid, which the kernel turns into
// input event devices. So this side only looks: it finds the input devices on the Bluetooth bus in
// sysfs (their name, the device's address, the report descriptor of the HID device behind them)
// and, while someone listens, reads their events from /dev/input. BlueZ claims the HID service, so
// it is not in the GATT explorer; this is where it shows.
//
// Nothing is opened while nobody listens: the reader thread starts on the first subscriber to
// "hid.input" (or with media keys on) and closes the event devices when the last one goes. With no
// Bluetooth input device connected it looks at sysfs only when BlueZ's devices change (poke()) or
// every five seconds.
//
// Media keys: with media_keys on, a remote's play/pause, next, previous, stop and volume keys drive
// the board's own media (the hook main() installs: the connected phone's AVRCP player and its
// transport volume), the way a remote paired with a speaker would.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace btb {

struct HidInputNode {
  std::string event;    // "event3"
  std::string name;     // the input device's name ("Remote Consumer Control")
  std::string address;  // the Bluetooth device's address (uniq), upper case
  std::string phys;     // the adapter's address, as bluetoothd sets it
  int bustype = 0, vendor = 0, product = 0, version = 0;
  std::string hid;      // the HID device's sysfs name ("0005:1D6B:0246.0001")
  std::string hidraw;   // "hidraw0", "" if none
  std::vector<uint8_t> report_descriptor;
};

// Every input event device on the Bluetooth bus (BUS_BLUETOOTH), under `sys_root` ("/sys").
std::vector<HidInputNode> hid_scan(const std::string& sys_root);
// A report descriptor read for people: {applications:[{page, usage, name}], reports:[{id, input_bits,
// output_bits, feature_bits}], items:N}. Tolerant: a malformed descriptor gives what was read.
nlohmann::json hid_rdesc_summary(const std::vector<uint8_t>& d);
// The kernel's name for an EV_KEY code ("KEY_PLAYPAUSE", "BTN_LEFT"), or "KEY_<n>".
std::string evdev_key_name(int code);

struct HidHostOptions {
  std::string sys_root = "/sys";
  std::string dev_root = "/dev";
  // Where media_keys is kept across restarts ("" = not kept). On by default: a remote paired with
  // the board should do what a remote paired with a speaker does.
  std::string state_file;
};

class HidHost {
 public:
  using Publish = std::function<void(const std::string& topic, const nlohmann::json& data)>;
  using HasSubscribers = std::function<bool(const std::string& topic)>;
  // A key event from a Bluetooth input device: its address, the EV_KEY code, 1 down / 0 up / 2 repeat.
  using KeyHook = std::function<void(const std::string& address, int code, int value)>;
  HidHost(HidHostOptions opt, Publish publish, HasSubscribers has_subscribers, KeyHook media_hook);
  ~HidHost();
  HidHost(const HidHost&) = delete;
  HidHost& operator=(const HidHost&) = delete;

  // {media_keys, reading, devices:[{address, name, nodes:[{event, name, hidraw}], vendor, product,
  //  hid, report_descriptor (hex), summary}], recent:[events]}
  nlohmann::json state();
  void set_media_keys(bool on);
  bool media_keys() const { return media_keys_; }
  // Wakes the reader to look again now: at the subscribers, and at the input devices (main()
  // calls it when BlueZ's devices change, so a remote that connects is opened at once).
  void poke();

 private:
  void run();
  bool wanted() const;

  const HidHostOptions opt_;
  const Publish publish_;
  const HasSubscribers has_subscribers_;
  const KeyHook hook_;
  std::atomic<bool> media_keys_{true};
  std::atomic<bool> rescan_{true};

  std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool reading_ = false;
  std::deque<nlohmann::json> recent_;
  std::thread thread_;
};

}  // namespace btb
