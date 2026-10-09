#pragma once

// The board as a HID over GATT device (HoGP, the "device" role): a Bluetooth LE remote control that
// a phone, a TV or a PC pairs with like any other. It serves the HID service with two input
// reports — Consumer Control (play/pause, volume, next, home, back, ...) and a keyboard (the arrow
// keys, Enter, typing) — plus Battery and Device Information, which hosts expect next to it, and
// advertises itself as a remote with the HID UUID so a host's "add device" list shows it.
//
// The service is a second GattServer application with a fixed definition (its own D-Bus root),
// and the advertisement an AdvManager instance; both exist only while the remote is enabled,
// because a HID service in the board's database makes every host that pairs treat the board as an
// input device. Key presses become report values notified to the subscribed host: a press sets the
// usage, a release sets it back to zero. A worker thread spaces taps and typed text out, so a host
// sees each press and release as a separate notification.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "adv.h"
#include "gatt_server.h"

namespace btb {

// The two input reports of the report map.
enum class HidReport : uint8_t { consumer = 1, keyboard = 2 };

struct HidKey {
  HidReport report = HidReport::consumer;
  uint16_t usage = 0;     // Consumer page usage, or keyboard page usage
  uint8_t modifiers = 0;  // keyboard only: bit 1 is left shift
};

// The report descriptor: Consumer Control (report 1, one 16-bit usage) and a keyboard (report 2,
// the 8-byte boot layout: modifiers, reserved, six keys).
std::vector<uint8_t> hid_report_map();
// The GATT application: HID (Information, Report Map, Control Point, Protocol Mode, the two
// Reports with their Report Reference descriptors), Battery, Device Information (PnP ID).
GattAppDef hid_app_def();
// A key by its API name ("play_pause", "volume_up", "up", "enter", "a"), or a raw usage written
// "consumer:0xcd" / "keyboard:0x28". False for anything else.
bool hid_key(const std::string& name, HidKey* out);
// The keyboard key that types one printable ASCII character (with shift where it needs it).
bool hid_char_key(char c, HidKey* out);
// The report value for a key going down, or for every key up (all zero).
std::vector<uint8_t> hid_report_bytes(const HidKey& k, bool down);
// {consumer:[names], keyboard:[names]} for the API and the console.
nlohmann::json hid_key_names();

class HidRemote {
 public:
  // `gatt` is the remote's own GattServer instance (a root other than the console's).
  HidRemote(BtManager& bt, AdvManager& adv, GattServer& gatt);
  ~HidRemote();
  HidRemote(const HidRemote&) = delete;
  HidRemote& operator=(const HidRemote&) = delete;

  // {enabled, name, gatt:{state, error}, advertisement:{id, state, error}, subscribed:{consumer,
  //  keyboard}, battery, last:{key, action, ts}, keys:{consumer, keyboard}}
  nlohmann::json state() const;
  // Registers the HID service and starts advertising as `name`. Waits for BlueZ.
  bool enable(const std::string& name, std::string* err);
  void disable();
  // action: "tap" (press, hold_ms, release), "down" or "up". A key held down is released by
  // itself after 10 s, so a page that lost its pointer-up cannot hold volume-up forever.
  bool key(const std::string& name, const std::string& action, unsigned hold_ms, std::string* err);
  // Types printable ASCII on the keyboard report, one press and release a character.
  bool type(const std::string& text, std::string* err);
  bool set_battery(int percent, std::string* err);

 private:
  struct Step {
    unsigned delay_ms;  // after the previous step
    HidReport report;
    std::vector<uint8_t> value;
  };
  void queue(std::vector<Step> steps);
  void run();
  std::string report_path(HidReport r) const;

  BtManager& bt_;
  AdvManager& adv_;
  GattServer& gatt_;

  mutable std::mutex m_;
  std::condition_variable cv_;
  bool enabled_ = false;
  std::string name_ = "btbench remote";
  int adv_id_ = 0;
  int battery_ = 100;
  nlohmann::json last_;
  std::deque<Step> steps_;
  bool held_[3] = {false, false, false};  // by report id
  uint64_t held_since_ns_[3] = {0, 0, 0};
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace btb
