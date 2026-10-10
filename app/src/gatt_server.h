#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "bluetooth.h"

namespace btb {

// A local GATT application as the API describes it (docs/api.md, PUT /api/le/gatt-server):
// services, their characteristics and descriptors, each with flags and an initial value. A
// characteristic with "counter" ticks a little-endian counter every period and notifies it; any
// writable one keeps what is written (reads return it, and a notifying one notifies it back).
// `handle` asks BlueZ for that attribute handle (0: BlueZ picks one). A service's is its declaration,
// a characteristic's its value (the declaration is the handle before), a descriptor's its own; a
// notifying characteristic's CCC takes the handle after its value. Fixed handles matter to a
// bonded client that cached the database (a HID host does): they stay the same across restarts.
struct GattDescDef {
  std::string uuid;  // full form
  uint16_t handle = 0;
  std::vector<std::string> flags;
  std::vector<uint8_t> value;
};
struct GattCharDef {
  std::string uuid;
  uint16_t handle = 0;
  std::vector<std::string> flags;
  std::vector<uint8_t> value;
  bool counter = false;
  unsigned period_ms = 1000;
  std::vector<GattDescDef> descriptors;
};
struct GattServiceDef {
  std::string uuid;
  uint16_t handle = 0;
  bool primary = true;
  std::vector<GattCharDef> characteristics;
};
struct GattAppDef {
  std::vector<GattServiceDef> services;
};

bool gatt_app_from_json(const nlohmann::json& j, GattAppDef* out, std::string* err);
nlohmann::json gatt_app_json(const GattAppDef& d);
// What the console offers to start from: a Heart Rate service with a counter, Battery, and a
// writable echo characteristic with a user-description descriptor.
nlohmann::json gatt_app_example();
// The little-endian counter value of `width` bytes (1..4).
std::vector<uint8_t> gatt_counter_bytes(uint32_t count, size_t width);

// Exports the application under its root (with an ObjectManager, which is how BlueZ reads it) and
// registers it with GattManager1. One application per instance: a new definition replaces the old.
// Registered again when BlueZ comes back. The console's GATT server is the instance at
// /org/btbench/gatt; the HID remote (hid_device.h) is a second one with a fixed definition, which
// BlueZ merges into the same local database.
class GattServer : public BusUser {
 public:
  // Reads and writes by remote devices, for the "gatt.server" topic: {op, path, uuid, value, device}.
  using EventFn = std::function<void(const nlohmann::json&)>;
  GattServer(BtManager& bt, EventFn ev, std::string root = "/org/btbench/gatt");
  ~GattServer() override;

  // {defined, state:"none|pending|registering|registered|failed", error, app, values:[{path, uuid,
  // value, notifying}]}
  nlohmann::json state() const;
  // Replaces the application and waits for BlueZ's answer.
  bool set(const GattAppDef& def, std::string* err);
  void clear();
  // A characteristic's new value, from any thread; notified if a client subscribed. `path` is
  // root/svcN/chrM as built from the definition. False when there is no such characteristic.
  bool set_value(const std::string& path, std::vector<uint8_t> value, bool* notifying = nullptr);
  const std::string& root() const { return root_; }

  void bus_opened(sd_bus* bus) override;
  void bus_closing() override;
  void bluez_ready(sd_bus* bus, const std::string& adapter_path) override;
  void bluez_gone() override;
  void tick(sd_bus* bus, uint64_t now_ns) override;

  struct Obj;
  // The sd-bus callbacks' way back in, on the bus thread.
  void emit_value(Obj& o);
  void on_registered(const sd_bus_error* e);
  void event(const nlohmann::json& ev) {
    if (event_) event_(ev);
  }
  std::mutex& values_mutex() const { return m_; }
  std::vector<Obj*> ordered_objects() const;  // bus thread: in definition order

 private:
  void build(sd_bus* bus);
  void drop_objects();
  void register_app(sd_bus* bus);
  void unregister_app(sd_bus* bus);

  BtManager& bt_;
  EventFn event_;
  const std::string root_;
  mutable std::mutex m_;  // guards everything below that a web handler reads
  bool defined_ = false;
  GattAppDef def_;
  std::string state_ = "none";
  std::string error_;
  std::vector<std::unique_ptr<Obj>> objs_;
  std::shared_ptr<std::promise<void>> waiter_;
  // Bus thread only.
  sd_bus* bus_ = nullptr;
  sd_bus_slot* om_slot_ = nullptr;
  std::string adapter_path_;
  bool registered_ = false;
  // A RegisterApplication sent and not answered: BlueZ coming back (bluez_ready) and a new
  // definition (set) can both want to register at once, and the second would be refused with
  // "already exists" although the first succeeds.
  bool registering_ = false;
  bool reregister_ = false;  // asked for while one was in flight: done again after its answer
};

}  // namespace btb
