#include "adv.h"

#include <errno.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include <chrono>
#include <future>

#include "bluez_model.h"
#include "dbus_export.h"
#include "util/log.h"
#include "util/strings.h"
#include "uuids.h"

using json = nlohmann::json;

namespace btb {

namespace {

constexpr const char* kAdvIface = "org.bluez.LEAdvertisement1";
constexpr const char* kAdvMgrIface = "org.bluez.LEAdvertisingManager1";

// The size of a legacy advertisement's payload. An extended one can be longer, but whether the
// controller does extended advertising is the controller's business: BlueZ says no if it does not.
constexpr size_t kMaxPayload = 251;

bool read_uuid_list(const json& j, const char* key, std::vector<std::string>* out, std::string* err) {
  if (!j.contains(key)) return true;
  for (const json& u : j.at(key)) {
    const std::string full = uuid_full(u.get<std::string>());
    if (full.empty()) {
      *err = std::string(key) + ": not a UUID: " + u.get<std::string>();
      return false;
    }
    out->push_back(full);
  }
  return true;
}

// A number the way JSON or a person writes it: 76, "76", "0x004c".
bool read_number(const json& v, uint64_t max, uint64_t* out) {
  if (v.is_number_unsigned()) {
    *out = v.get<uint64_t>();
    return *out <= max;
  }
  if (v.is_string()) return parse_uint(v.get<std::string>(), max, out);
  return false;
}

std::string hex4(unsigned v) {
  char b[16];
  snprintf(b, sizeof(b), "0x%04x", v);
  return b;
}

}  // namespace

bool adv_spec_from_json(const json& j, AdvSpec* out, std::string* err) {
  AdvSpec s;
  try {
    if (!j.is_object()) {
      *err = "an advertisement is a JSON object";
      return false;
    }
    if (j.contains("type")) {
      s.type = j["type"].get<std::string>();
      if (s.type != "peripheral" && s.type != "broadcast") {
        *err = "type must be peripheral or broadcast";
        return false;
      }
    }
    if (j.contains("local_name")) s.local_name = j["local_name"].get<std::string>();
    if (s.local_name.size() > 29) {
      *err = "local_name is at most 29 bytes in a legacy advertisement";
      return false;
    }
    if (!read_uuid_list(j, "service_uuids", &s.service_uuids, err)) return false;
    if (!read_uuid_list(j, "solicit_uuids", &s.solicit_uuids, err)) return false;
    if (j.contains("manufacturer_data")) {
      for (auto it = j["manufacturer_data"].begin(); it != j["manufacturer_data"].end(); ++it) {
        uint64_t id = 0;
        if (!parse_uint(it.key(), 0xffff, &id)) {
          *err = "manufacturer_data: company id must be 0..0xffff, not " + it.key();
          return false;
        }
        std::vector<uint8_t> b;
        if (!from_hex(it->get<std::string>(), &b)) {
          *err = "manufacturer_data: not hex: " + it->get<std::string>();
          return false;
        }
        s.manufacturer_data[static_cast<uint16_t>(id)] = b;
      }
    }
    if (j.contains("service_data")) {
      for (auto it = j["service_data"].begin(); it != j["service_data"].end(); ++it) {
        const std::string u = uuid_full(it.key());
        std::vector<uint8_t> b;
        if (u.empty()) {
          *err = "service_data: not a UUID: " + it.key();
          return false;
        }
        if (!from_hex(it->get<std::string>(), &b)) {
          *err = "service_data: not hex: " + it->get<std::string>();
          return false;
        }
        s.service_data[u] = b;
      }
    }
    if (j.contains("appearance") && !j["appearance"].is_null()) {
      uint64_t a = 0;
      if (!read_number(j["appearance"], 0xffff, &a)) {
        *err = "appearance must be 0..0xffff";
        return false;
      }
      s.appearance = static_cast<int>(a);
    }
    if (j.contains("tx_power") && !j["tx_power"].is_null()) {
      s.tx_power = j["tx_power"].get<int>();
      if (s.tx_power < -127 || s.tx_power > 20) {
        *err = "tx_power must be -127..20 dBm";
        return false;
      }
    }
    if (j.contains("discoverable")) {
      s.discoverable = j["discoverable"].get<bool>();
      s.has_discoverable = true;
    }
    if (j.contains("includes")) {
      for (const json& i : j["includes"]) {
        const std::string v = i.get<std::string>();
        if (v != "tx-power" && v != "appearance" && v != "local-name" && v != "rsi") {
          *err = "includes: " + v + " is not one of tx-power, appearance, local-name, rsi";
          return false;
        }
        s.includes.push_back(v);
      }
    }
    if (j.contains("min_interval_ms")) s.min_interval_ms = j["min_interval_ms"].get<unsigned>();
    if (j.contains("max_interval_ms")) s.max_interval_ms = j["max_interval_ms"].get<unsigned>();
    if (s.min_interval_ms && s.max_interval_ms && s.min_interval_ms > s.max_interval_ms) {
      *err = "min_interval_ms is above max_interval_ms";
      return false;
    }
    // 20 ms .. 10.24 s is what a legacy advertising interval can be (Core Vol 6 Part B 4.4.2.2).
    for (unsigned v : {s.min_interval_ms, s.max_interval_ms}) {
      if (v && (v < 20 || v > 10240)) {
        *err = "advertising intervals are 20..10240 ms";
        return false;
      }
    }
    if (j.contains("duration_s")) s.duration_s = j["duration_s"].get<unsigned>();
    if (j.contains("timeout_s")) s.timeout_s = j["timeout_s"].get<unsigned>();
    if (s.duration_s > 0xffff || s.timeout_s > 0xffff) {
      *err = "duration_s and timeout_s are at most 65535";
      return false;
    }
    if (j.contains("secondary_channel")) {
      s.secondary_channel = j["secondary_channel"].get<std::string>();
      if (!s.secondary_channel.empty() && s.secondary_channel != "1M" && s.secondary_channel != "2M" &&
          s.secondary_channel != "Coded") {
        *err = "secondary_channel must be 1M, 2M or Coded";
        return false;
      }
    }
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
  // A rough size check, so the common mistake (a long name plus a 128-bit UUID) is caught here
  // with a reason rather than as BlueZ's "Invalid Length".
  size_t bytes = 3;  // the flags
  if (!s.local_name.empty()) bytes += 2 + s.local_name.size();
  for (const std::string& u : s.service_uuids) bytes += 2 + uuid_short(u).size() / 2;
  for (const auto& m : s.manufacturer_data) bytes += 4 + m.second.size();
  for (const auto& d : s.service_data) bytes += 2 + uuid_short(d.first).size() / 2 + d.second.size();
  if (bytes > kMaxPayload) {
    *err = "the advertisement would be " + std::to_string(bytes) + " bytes";
    return false;
  }
  *out = std::move(s);
  return true;
}

json adv_spec_json(const AdvSpec& s) {
  json su = json::array(), so = json::array(), md = json::object(), sd = json::object();
  for (const std::string& u : s.service_uuids) su.push_back(uuid_short(u));
  for (const std::string& u : s.solicit_uuids) so.push_back(uuid_short(u));
  for (const auto& m : s.manufacturer_data) md[hex4(m.first)] = to_hex(m.second);
  for (const auto& d : s.service_data) sd[uuid_short(d.first)] = to_hex(d.second);
  return json{{"type", s.type},
              {"local_name", s.local_name},
              {"service_uuids", su},
              {"solicit_uuids", so},
              {"manufacturer_data", md},
              {"service_data", sd},
              {"appearance", s.appearance >= 0 ? json(hex4(static_cast<unsigned>(s.appearance))) : json(nullptr)},
              {"tx_power", s.tx_power != 127 ? json(s.tx_power) : json(nullptr)},
              {"discoverable", s.discoverable},
              {"includes", s.includes},
              {"min_interval_ms", s.min_interval_ms},
              {"max_interval_ms", s.max_interval_ms},
              {"duration_s", s.duration_s},
              {"timeout_s", s.timeout_s},
              {"secondary_channel", s.secondary_channel}};
}

// ---- The exported objects --------------------------------------------------------------------------

struct AdvManager::Instance {
  AdvManager* mgr = nullptr;
  int id = 0;
  std::string path;
  AdvSpec spec;
  // Under mgr->m_.
  std::string state = "pending";
  std::string error;
  std::shared_ptr<std::promise<void>> waiter;
  // Bus thread only.
  std::unique_ptr<VtableBuilder> vt;
  sd_bus_slot* slot = nullptr;
  bool registered = false;
};

namespace {

struct RegOp {
  AdvManager* mgr;
  int id;
};

int append_strv(sd_bus_message* reply, const std::vector<std::string>& v) {
  int r = sd_bus_message_open_container(reply, 'a', "s");
  for (const std::string& s : v)
    if (r >= 0) r = sd_bus_message_append(reply, "s", s.c_str());
  if (r >= 0) r = sd_bus_message_close_container(reply);
  return r;
}

int adv_get(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
            void* userdata, sd_bus_error*) {
  const AdvSpec& s = static_cast<AdvManager::Instance*>(userdata)->spec;
  const std::string p = property;
  if (p == "Type") return sd_bus_message_append(reply, "s", s.type.c_str());
  if (p == "LocalName") return sd_bus_message_append(reply, "s", s.local_name.c_str());
  if (p == "ServiceUUIDs") return append_strv(reply, s.service_uuids);
  if (p == "SolicitUUIDs") return append_strv(reply, s.solicit_uuids);
  if (p == "Includes") return append_strv(reply, s.includes);
  if (p == "Appearance") return sd_bus_message_append(reply, "q", static_cast<uint16_t>(s.appearance));
  if (p == "TxPower") return sd_bus_message_append(reply, "n", static_cast<int16_t>(s.tx_power));
  if (p == "Discoverable") return sd_bus_message_append(reply, "b", static_cast<int>(s.discoverable));
  if (p == "MinInterval") return sd_bus_message_append(reply, "u", s.min_interval_ms);
  if (p == "MaxInterval") return sd_bus_message_append(reply, "u", s.max_interval_ms);
  if (p == "Duration") return sd_bus_message_append(reply, "q", static_cast<uint16_t>(s.duration_s));
  if (p == "Timeout") return sd_bus_message_append(reply, "q", static_cast<uint16_t>(s.timeout_s));
  if (p == "SecondaryChannel") return sd_bus_message_append(reply, "s", s.secondary_channel.c_str());
  if (p == "ManufacturerData") {
    int r = sd_bus_message_open_container(reply, 'a', "{qv}");
    for (const auto& m : s.manufacturer_data) {
      if (r >= 0) r = sd_bus_message_open_container(reply, 'e', "qv");
      if (r >= 0) r = sd_bus_message_append(reply, "q", m.first);
      if (r >= 0) r = append_variant(reply, DVar::bytes(m.second));
      if (r >= 0) r = sd_bus_message_close_container(reply);
    }
    if (r >= 0) r = sd_bus_message_close_container(reply);
    return r;
  }
  if (p == "ServiceData") {
    int r = sd_bus_message_open_container(reply, 'a', "{sv}");
    for (const auto& d : s.service_data) {
      if (r >= 0) r = sd_bus_message_open_container(reply, 'e', "sv");
      if (r >= 0) r = sd_bus_message_append(reply, "s", d.first.c_str());
      if (r >= 0) r = append_variant(reply, DVar::bytes(d.second));
      if (r >= 0) r = sd_bus_message_close_container(reply);
    }
    if (r >= 0) r = sd_bus_message_close_container(reply);
    return r;
  }
  return -ENOENT;
}

}  // namespace

// BlueZ calls Release when it drops an advertisement of its own accord: its Timeout ran out, or
// the adapter went away. Nothing to free: the object stays exported and can be registered again.
static int adv_release(sd_bus_message* m, void* userdata, sd_bus_error*) {
  auto* in = static_cast<AdvManager::Instance*>(userdata);
  LOG_INFO("adv: {} released by BlueZ", in->path);
  in->registered = false;
  in->mgr->bt_released(in->id);
  return sd_bus_reply_method_return(m, "");
}

AdvManager::AdvManager(BtManager& bt) : bt_(bt) {}
AdvManager::~AdvManager() = default;

void AdvManager::bt_released(int id) {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = instances_.find(id);
  if (it != instances_.end()) it->second->state = "released";
}

void AdvManager::export_one(sd_bus* bus, Instance& in) {
  if (in.slot) return;
  const AdvSpec& s = in.spec;
  auto vt = std::make_unique<VtableBuilder>();
  vt->method("Release", "", "", adv_release);
  vt->prop("Type", "s", adv_get, false);
  if (!s.local_name.empty()) vt->prop("LocalName", "s", adv_get, false);
  if (!s.service_uuids.empty()) vt->prop("ServiceUUIDs", "as", adv_get, false);
  if (!s.solicit_uuids.empty()) vt->prop("SolicitUUIDs", "as", adv_get, false);
  if (!s.manufacturer_data.empty()) vt->prop("ManufacturerData", "a{qv}", adv_get, false);
  if (!s.service_data.empty()) vt->prop("ServiceData", "a{sv}", adv_get, false);
  if (!s.includes.empty()) vt->prop("Includes", "as", adv_get, false);
  if (s.appearance >= 0) vt->prop("Appearance", "q", adv_get, false);
  if (s.tx_power != 127) vt->prop("TxPower", "n", adv_get, false);
  if (s.has_discoverable) vt->prop("Discoverable", "b", adv_get, false);
  if (s.min_interval_ms) vt->prop("MinInterval", "u", adv_get, false);
  if (s.max_interval_ms) vt->prop("MaxInterval", "u", adv_get, false);
  if (s.duration_s) vt->prop("Duration", "q", adv_get, false);
  if (s.timeout_s) vt->prop("Timeout", "q", adv_get, false);
  if (!s.secondary_channel.empty()) vt->prop("SecondaryChannel", "s", adv_get, false);
  const int r = sd_bus_add_object_vtable(bus, &in.slot, in.path.c_str(), kAdvIface, vt->finish(), &in);
  if (r < 0) {
    LOG_WARN("adv: cannot export {}: {}", in.path, strerror(-r));
    in.slot = nullptr;
    std::lock_guard<std::mutex> lk(m_);
    in.state = "failed";
    in.error = std::string("cannot export the object: ") + strerror(-r);
    return;
  }
  in.vt = std::move(vt);
}

static int adv_registered(sd_bus_message* m, void* data, sd_bus_error*) {
  auto* op = static_cast<RegOp*>(data);
  op->mgr->bt_registered(op->id, sd_bus_message_get_error(m));
  return 0;
}

void AdvManager::bt_registered(int id, const sd_bus_error* e) {
  std::shared_ptr<std::promise<void>> w;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = instances_.find(id);
    if (it == instances_.end()) return;
    Instance& in = *it->second;
    in.registered = !e;
    in.state = e ? "failed" : "active";
    in.error = e ? friendly(e) : "";
    w = std::move(in.waiter);
    if (e) LOG_WARN("adv: {} refused: {}", in.path, in.error);
    else LOG_INFO("adv: {} active", in.path);
  }
  if (w) w->set_value();
  bt_.touch();  // ActiveInstances moved
}

void AdvManager::register_one(sd_bus* bus, Instance& in) {
  if (!in.slot || in.registered || adapter_path_.empty()) return;
  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus, &m, "org.bluez", adapter_path_.c_str(), kAdvMgrIface,
                                         "RegisterAdvertisement");
  if (r >= 0) r = sd_bus_message_append(m, "o", in.path.c_str());
  if (r >= 0) r = append_dict(m, {});
  auto* op = new RegOp{this, in.id};
  sd_bus_slot* slot = nullptr;
  if (r >= 0) r = sd_bus_call_async(bus, &slot, m, adv_registered, op, 10 * 1000000ull);
  sd_bus_message_unref(m);
  if (r < 0) {
    delete op;
    std::shared_ptr<std::promise<void>> w;
    {
      std::lock_guard<std::mutex> lk(m_);
      in.state = "failed";
      in.error = std::string("RegisterAdvertisement: ") + strerror(-r);
      w = std::move(in.waiter);
    }
    if (w) w->set_value();
    return;
  }
  sd_bus_slot_set_destroy_callback(slot, [](void* p) { delete static_cast<RegOp*>(p); });
  sd_bus_slot_set_floating(slot, 1);
  sd_bus_slot_unref(slot);
  std::lock_guard<std::mutex> lk(m_);
  in.state = "registering";
}

void AdvManager::unregister_one(sd_bus* bus, Instance& in) {
  if (!in.registered || adapter_path_.empty() || !bus) return;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_call_method(bus, "org.bluez", adapter_path_.c_str(), kAdvMgrIface, "UnregisterAdvertisement",
                     &e, nullptr, "o", in.path.c_str());
  sd_bus_error_free(&e);
  in.registered = false;
}

void AdvManager::bus_opened(sd_bus* bus) {
  std::vector<Instance*> all;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& kv : instances_) all.push_back(kv.second.get());
  }
  for (Instance* in : all) export_one(bus, *in);
}

void AdvManager::bus_closing() {
  std::lock_guard<std::mutex> lk(m_);
  for (auto& kv : instances_) {
    Instance& in = *kv.second;
    if (in.slot) in.slot = sd_bus_slot_unref(in.slot);
    in.vt.reset();
    in.registered = false;
    if (in.state == "active" || in.state == "registering") in.state = "pending";
  }
}

void AdvManager::bluez_ready(sd_bus* bus, const std::string& adapter_path) {
  adapter_path_ = adapter_path;
  std::vector<Instance*> all;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& kv : instances_)
      if (kv.second->state != "released") all.push_back(kv.second.get());
  }
  for (Instance* in : all) register_one(bus, *in);
}

void AdvManager::bluez_gone() {
  adapter_path_.clear();
  std::lock_guard<std::mutex> lk(m_);
  for (auto& kv : instances_) {
    kv.second->registered = false;
    if (kv.second->state == "active" || kv.second->state == "registering") kv.second->state = "pending";
  }
}

json AdvManager::instance_json(const Instance& in) const {
  return json{{"id", in.id}, {"path", in.path}, {"state", in.state}, {"error", in.error},
              {"spec", adv_spec_json(in.spec)}};
}

json AdvManager::list() const {
  json out = json::array();
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& kv : instances_) out.push_back(instance_json(*kv.second));
  }
  json mgr = nullptr;
  const std::string apath = bt_.adapter_path();
  for (const json& a : model_adapters(*bt_.objects()))
    if (a["path"] == apath) mgr = a["adv"];
  return json{{"instances", out}, {"manager", mgr}};
}

bool AdvManager::add(const AdvSpec& spec, json* out, std::string* err) {
  if (!bt_.running()) {
    *err = "Bluetooth is not running";
    return false;
  }
  auto waiter = std::make_shared<std::promise<void>>();
  std::future<void> done = waiter->get_future();
  int id;
  {
    std::lock_guard<std::mutex> lk(m_);
    id = next_id_++;
    auto in = std::make_unique<Instance>();
    in->mgr = this;
    in->id = id;
    in->path = "/org/btbench/adv" + std::to_string(id);
    in->spec = spec;
    in->waiter = waiter;
    instances_[id] = std::move(in);
  }
  bt_.post([this, id](sd_bus* bus) {
    Instance* in = nullptr;
    {
      std::lock_guard<std::mutex> lk(m_);
      const auto it = instances_.find(id);
      if (it != instances_.end()) in = it->second.get();
    }
    if (!in) return;
    std::shared_ptr<std::promise<void>> w;
    if (bus) {
      export_one(bus, *in);
      if (in->slot && !adapter_path_.empty()) {
        register_one(bus, *in);
        return;  // the reply completes the waiter
      }
    }
    // No bus or no BlueZ: it waits as "pending" and is registered when BlueZ is there.
    std::lock_guard<std::mutex> lk(m_);
    w = std::move(in->waiter);
    if (w) w->set_value();
  });
  done.wait_for(std::chrono::seconds(12));
  std::lock_guard<std::mutex> lk(m_);
  const auto it = instances_.find(id);
  if (it == instances_.end()) {
    *err = "removed meanwhile";
    return false;
  }
  *out = instance_json(*it->second);
  return true;
}

bool AdvManager::remove(int id, std::string* err) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!instances_.count(id)) {
      *err = "no such advertisement";
      return false;
    }
  }
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> f = done->get_future();
  bt_.post([this, id, done](sd_bus* bus) {
    std::unique_ptr<Instance> in;
    {
      std::lock_guard<std::mutex> lk(m_);
      const auto it = instances_.find(id);
      if (it != instances_.end()) {
        in = std::move(it->second);
        instances_.erase(it);
      }
    }
    if (in) {
      unregister_one(bus, *in);
      if (in->slot) sd_bus_slot_unref(in->slot);
      LOG_INFO("adv: {} removed", in->path);
    }
    done->set_value();
    bt_.touch();
  });
  f.wait_for(std::chrono::seconds(6));
  return true;
}

void AdvManager::remove_all() {
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& kv : instances_) ids.push_back(kv.first);
  }
  std::string err;
  for (int id : ids) remove(id, &err);
}

}  // namespace btb
