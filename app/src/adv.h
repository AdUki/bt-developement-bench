#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "bluetooth.h"

namespace btb {

// One LE advertisement as the API describes it (docs/api.md, POST /api/le/adv). Checked and
// normalised by adv_spec_from_json, so what reaches the bus is always well-formed.
struct AdvSpec {
  std::string type = "peripheral";  // peripheral | broadcast
  std::string local_name;           // "" = none
  std::vector<std::string> service_uuids;  // full 128-bit form
  std::vector<std::string> solicit_uuids;
  std::map<uint16_t, std::vector<uint8_t>> manufacturer_data;
  std::map<std::string, std::vector<uint8_t>> service_data;  // full UUID -> bytes
  int appearance = -1;    // -1 = none
  int tx_power = 127;     // dBm; 127 = not set
  bool discoverable = true;
  bool has_discoverable = false;
  std::vector<std::string> includes;  // tx-power, appearance, local-name, rsi
  unsigned min_interval_ms = 0;       // 0 = BlueZ's default
  unsigned max_interval_ms = 0;
  unsigned duration_s = 0;
  unsigned timeout_s = 0;  // 0 = until removed
  std::string secondary_channel;  // "" | 1M | 2M | Coded
};

bool adv_spec_from_json(const nlohmann::json& j, AdvSpec* out, std::string* err);
nlohmann::json adv_spec_json(const AdvSpec& s);

// LEAdvertisingManager1 instances from JSON: each is an org.bluez.LEAdvertisement1 object this
// daemon exports and BlueZ reads (GetAll) when it is registered. Several at once, as many as the
// controller has instances. Registered again when BlueZ comes back; BlueZ calls Release on one it
// drops (its Timeout ran out), which is shown as "released".
class AdvManager : public BusUser {
 public:
  // Out of line, like the destructor: Instance is complete only in adv.cpp.
  explicit AdvManager(BtManager& bt);
  ~AdvManager() override;

  // {instances:[{id, path, state:"pending|registering|active|failed|released", error, spec}],
  //  supported_instances, active_instances, supported_includes} — the latter from the adapter.
  nlohmann::json list() const;
  // Registers it and waits for BlueZ's answer: the result carries the instance, failed or not.
  bool add(const AdvSpec& spec, nlohmann::json* out, std::string* err);
  bool remove(int id, std::string* err);
  void remove_all();

  // BusUser, on the bus thread.
  void bus_opened(sd_bus* bus) override;
  void bus_closing() override;
  void bluez_ready(sd_bus* bus, const std::string& adapter_path) override;
  void bluez_gone() override;

  struct Instance;
  // The sd-bus callbacks' way back in, on the bus thread.
  void bt_released(int id);
  void bt_registered(int id, const sd_bus_error* e);

 private:
  void export_one(sd_bus* bus, Instance& in);
  void register_one(sd_bus* bus, Instance& in);
  void unregister_one(sd_bus* bus, Instance& in);
  nlohmann::json instance_json(const Instance& in) const;

  BtManager& bt_;
  mutable std::mutex m_;
  std::map<int, std::unique_ptr<Instance>> instances_;
  int next_id_ = 1;
  // Bus thread only.
  std::string adapter_path_;
};

}  // namespace btb
