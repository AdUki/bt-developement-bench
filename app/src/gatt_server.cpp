#include "gatt_server.h"

#include <errno.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <chrono>

#include "dbus_export.h"
#include "util/log.h"
#include "util/strings.h"
#include "uuids.h"

using json = nlohmann::json;

namespace btb {

namespace {

constexpr const char* kRoot = "/org/btbench/gatt";
constexpr const char* kServiceIface = "org.bluez.GattService1";
constexpr const char* kCharIface = "org.bluez.GattCharacteristic1";
constexpr const char* kDescIface = "org.bluez.GattDescriptor1";
constexpr const char* kGattMgrIface = "org.bluez.GattManager1";

// What BlueZ's gatt-database.c accepts in Flags (doc/org.bluez.GattCharacteristic.rst).
const std::vector<std::string>& known_flags() {
  static const std::vector<std::string> f = {
      "broadcast", "read", "write-without-response", "write", "notify", "indicate",
      "authenticated-signed-writes", "extended-properties", "reliable-write", "writable-auxiliaries",
      "encrypt-read", "encrypt-write", "encrypt-notify", "encrypt-indicate",
      "encrypt-authenticated-read", "encrypt-authenticated-write", "encrypt-authenticated-notify",
      "encrypt-authenticated-indicate", "secure-read", "secure-write", "secure-notify",
      "secure-indicate", "authorize"};
  return f;
}

bool has(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

bool read_flags(const json& j, std::vector<std::string>* out, std::string* err, const std::string& what) {
  if (!j.contains("flags")) return true;
  for (const json& f : j["flags"]) {
    const std::string s = f.get<std::string>();
    if (!has(known_flags(), s)) {
      *err = what + ": unknown flag " + s;
      return false;
    }
    out->push_back(s);
  }
  return true;
}

// "value": hex, or "text": a string taken as its UTF-8 bytes.
bool read_value(const json& j, std::vector<uint8_t>* out, std::string* err, const std::string& what) {
  if (j.contains("text")) {
    const std::string t = j["text"].get<std::string>();
    out->assign(t.begin(), t.end());
  } else if (j.contains("value")) {
    if (!from_hex(j["value"].get<std::string>(), out)) {
      *err = what + ": value is not hex";
      return false;
    }
  }
  if (out->size() > 512) {
    *err = what + ": an attribute value is at most 512 bytes";
    return false;
  }
  return true;
}

bool read_uuid(const json& j, std::string* out, std::string* err, const std::string& what) {
  *out = uuid_full(j.at("uuid").get<std::string>());
  if (out->empty()) {
    *err = what + ": not a UUID";
    return false;
  }
  return true;
}

}  // namespace

bool gatt_app_from_json(const json& j, GattAppDef* out, std::string* err) {
  GattAppDef d;
  try {
    if (!j.contains("services") || !j["services"].is_array() || j["services"].empty()) {
      *err = "an application has a non-empty \"services\" array";
      return false;
    }
    int si = 0;
    for (const json& sj : j["services"]) {
      GattServiceDef s;
      const std::string sw = "service " + std::to_string(si++);
      if (!read_uuid(sj, &s.uuid, err, sw)) return false;
      s.primary = sj.value("primary", true);
      int ci = 0;
      for (const json& cj : sj.value("characteristics", json::array())) {
        GattCharDef c;
        const std::string cw = sw + " characteristic " + std::to_string(ci++);
        if (!read_uuid(cj, &c.uuid, err, cw)) return false;
        if (!read_flags(cj, &c.flags, err, cw)) return false;
        if (c.flags.empty()) {
          *err = cw + ": no flags (read, write, notify, ...)";
          return false;
        }
        if (!read_value(cj, &c.value, err, cw)) return false;
        if (cj.contains("counter")) {
          const json& k = cj["counter"];
          if (k.is_boolean()) {
            c.counter = k.get<bool>();
          } else if (k.is_object()) {
            c.counter = true;
            c.period_ms = k.value("period_ms", 1000u);
          }
          if (c.counter && (c.period_ms < 10 || c.period_ms > 60000)) {
            *err = cw + ": counter period_ms is 10..60000";
            return false;
          }
          if (c.counter && c.value.size() > 4) {
            *err = cw + ": a counter is at most 4 bytes wide (its initial value's width)";
            return false;
          }
        }
        int di = 0;
        for (const json& dj : cj.value("descriptors", json::array())) {
          GattDescDef ds;
          const std::string dw = cw + " descriptor " + std::to_string(di++);
          if (!read_uuid(dj, &ds.uuid, err, dw)) return false;
          // BlueZ makes the CCC itself for a characteristic that notifies; one defined here too
          // would be a second.
          if (uuid_short(ds.uuid) == "2902") {
            *err = dw + ": BlueZ adds the Client Characteristic Configuration (0x2902) itself";
            return false;
          }
          if (!read_flags(dj, &ds.flags, err, dw)) return false;
          if (ds.flags.empty()) ds.flags = {"read"};
          if (!read_value(dj, &ds.value, err, dw)) return false;
          c.descriptors.push_back(std::move(ds));
        }
        s.characteristics.push_back(std::move(c));
      }
      d.services.push_back(std::move(s));
    }
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
  *out = std::move(d);
  return true;
}

json gatt_app_json(const GattAppDef& d) {
  json services = json::array();
  for (const GattServiceDef& s : d.services) {
    json chars = json::array();
    for (const GattCharDef& c : s.characteristics) {
      json descs = json::array();
      for (const GattDescDef& ds : c.descriptors)
        descs.push_back(json{{"uuid", uuid_short(ds.uuid)}, {"flags", ds.flags}, {"value", to_hex(ds.value)}});
      json cj{{"uuid", uuid_short(c.uuid)}, {"flags", c.flags}, {"value", to_hex(c.value)},
              {"descriptors", descs}};
      if (c.counter) cj["counter"] = json{{"period_ms", c.period_ms}};
      chars.push_back(std::move(cj));
    }
    services.push_back(json{{"uuid", uuid_short(s.uuid)}, {"primary", s.primary}, {"characteristics", chars}});
  }
  return json{{"services", services}};
}

json gatt_app_example() {
  return json::parse(R"({
  "services": [
    {"uuid": "180d", "characteristics": [
      {"uuid": "2a37", "flags": ["notify"], "value": "0048", "counter": {"period_ms": 1000}},
      {"uuid": "2a38", "flags": ["read"], "value": "01"}
    ]},
    {"uuid": "180f", "characteristics": [
      {"uuid": "2a19", "flags": ["read", "notify"], "value": "64"}
    ]},
    {"uuid": "12345678-1234-5678-1234-56789abcdef0", "characteristics": [
      {"uuid": "12345678-1234-5678-1234-56789abcdef1",
       "flags": ["read", "write", "write-without-response", "notify"], "text": "echo",
       "descriptors": [{"uuid": "2901", "flags": ["read"], "text": "Echo: reads return what was written"}]}
    ]}
  ]
})");
}

std::vector<uint8_t> gatt_counter_bytes(uint32_t count, size_t width) {
  width = std::clamp<size_t>(width, 1, 4);
  std::vector<uint8_t> b(width);
  for (size_t i = 0; i < width; ++i) b[i] = static_cast<uint8_t>(count >> (8 * i));
  return b;
}

// ---- The exported objects --------------------------------------------------------------------------

struct GattServer::Obj {
  GattServer* srv = nullptr;
  std::string path;
  std::string iface;
  std::string uuid;
  std::string parent;  // the service of a characteristic, the characteristic of a descriptor
  bool primary = true;
  std::vector<std::string> flags;
  // Under srv->m_.
  std::vector<uint8_t> value;
  bool notifying = false;
  // Bus thread only.
  bool counter = false;
  unsigned period_ms = 1000;
  size_t width = 1;
  uint32_t count = 0;
  uint64_t next_ns = 0;
  std::unique_ptr<VtableBuilder> vt;
  sd_bus_slot* slot = nullptr;
};

namespace {

int obj_get(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
            void* userdata, sd_bus_error*) {
  auto* o = static_cast<GattServer::Obj*>(userdata);
  const std::string p = property;
  if (p == "UUID") return sd_bus_message_append(reply, "s", o->uuid.c_str());
  if (p == "Primary") return sd_bus_message_append(reply, "b", static_cast<int>(o->primary));
  if (p == "Service" || p == "Characteristic") return sd_bus_message_append(reply, "o", o->parent.c_str());
  if (p == "Flags") {
    int r = sd_bus_message_open_container(reply, 'a', "s");
    for (const std::string& f : o->flags)
      if (r >= 0) r = sd_bus_message_append(reply, "s", f.c_str());
    if (r >= 0) r = sd_bus_message_close_container(reply);
    return r;
  }
  std::lock_guard<std::mutex> lk(o->srv->values_mutex());
  if (p == "Value") return append_bytes(reply, o->value);
  if (p == "Notifying") return sd_bus_message_append(reply, "b", static_cast<int>(o->notifying));
  return -ENOENT;
}

// The options dict of ReadValue/WriteValue: the offset, and which device asks (for the log and the
// event). The other keys (type, mtu, link, prepare-authorize) are BlueZ's to worry about.
void read_options(sd_bus_message* m, uint16_t* offset, std::string* device) {
  json opts;
  if (read_json(m, &opts) > 0 && opts.is_object()) {
    *offset = static_cast<uint16_t>(num_of(opts, "offset"));
    *device = address_from_path(str_of(opts, "device"));
  }
}

int on_read(sd_bus_message* m, void* userdata, sd_bus_error*) {
  auto* o = static_cast<GattServer::Obj*>(userdata);
  uint16_t offset = 0;
  std::string device;
  read_options(m, &offset, &device);
  std::vector<uint8_t> v;
  {
    std::lock_guard<std::mutex> lk(o->srv->values_mutex());
    v = o->value;
  }
  if (offset > v.size())
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.InvalidOffset", "offset %u past %zu bytes",
                                      offset, v.size());
  v.erase(v.begin(), v.begin() + offset);
  o->srv->event(json{{"op", "read"}, {"path", o->path}, {"uuid", uuid_short(o->uuid)},
                     {"value", to_hex(v)}, {"offset", offset}, {"device", device}});
  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(m, &reply);
  if (r >= 0) r = append_bytes(reply, v);
  if (r >= 0) r = sd_bus_send(nullptr, reply, nullptr);
  sd_bus_message_unref(reply);
  return r;
}

int on_write(sd_bus_message* m, void* userdata, sd_bus_error*) {
  auto* o = static_cast<GattServer::Obj*>(userdata);
  const void* p = nullptr;
  size_t n = 0;
  int r = sd_bus_message_read_array(m, 'y', &p, &n);
  if (r < 0) return r;
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint16_t offset = 0;
  std::string device;
  read_options(m, &offset, &device);
  bool notify = false;
  std::vector<uint8_t> now;
  {
    std::lock_guard<std::mutex> lk(o->srv->values_mutex());
    if (offset > o->value.size())
      return sd_bus_reply_method_errorf(m, "org.bluez.Error.InvalidOffset", "offset past the value");
    if (offset + n > 512)
      return sd_bus_reply_method_errorf(m, "org.bluez.Error.InvalidValueLength", "over 512 bytes");
    o->value.resize(offset);
    o->value.insert(o->value.end(), b, b + n);
    now = o->value;
    notify = o->notifying;
  }
  o->srv->event(json{{"op", "write"}, {"path", o->path}, {"uuid", uuid_short(o->uuid)},
                     {"value", to_hex(now)}, {"offset", offset}, {"device", device}});
  // The echo: a client that subscribed hears back what was written.
  if (notify) o->srv->emit_value(*o);
  return sd_bus_reply_method_return(m, "");
}

int on_notify(sd_bus_message* m, void* userdata, sd_bus_error*) {
  auto* o = static_cast<GattServer::Obj*>(userdata);
  const bool on = strcmp(sd_bus_message_get_member(m), "StartNotify") == 0;
  {
    std::lock_guard<std::mutex> lk(o->srv->values_mutex());
    o->notifying = on;
  }
  LOG_INFO("gatt-server: {} notify {}", o->path, on ? "on" : "off");
  o->srv->event(json{{"op", on ? "notify-on" : "notify-off"}, {"path", o->path}, {"uuid", uuid_short(o->uuid)}});
  sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), o->path.c_str(), kCharIface, "Notifying", nullptr);
  return sd_bus_reply_method_return(m, "");
}

int on_confirm(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }

int app_registered(sd_bus_message* m, void* data, sd_bus_error*) {
  static_cast<GattServer*>(data)->on_registered(sd_bus_message_get_error(m));
  return 0;
}

}  // namespace

GattServer::GattServer(BtManager& bt, EventFn ev) : bt_(bt), event_(std::move(ev)) {}
GattServer::~GattServer() = default;

void GattServer::emit_value(Obj& o) {
  if (bus_) sd_bus_emit_properties_changed(bus_, o.path.c_str(), kCharIface, "Value", nullptr);
}

void GattServer::drop_objects() {
  for (auto& o : objs_) {
    if (o->slot) o->slot = sd_bus_slot_unref(o->slot);
    o->vt.reset();
  }
}

// The objects of the current definition, exported. Called with m_ not held.
void GattServer::build(sd_bus* bus) {
  std::vector<std::unique_ptr<Obj>> objs;
  GattAppDef def;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!defined_) return;
    def = def_;
  }
  const uint64_t now = mono_ns();
  for (size_t si = 0; si < def.services.size(); ++si) {
    const GattServiceDef& s = def.services[si];
    auto so = std::make_unique<Obj>();
    so->srv = this;
    so->path = std::string(kRoot) + "/svc" + std::to_string(si);
    so->iface = kServiceIface;
    so->uuid = s.uuid;
    so->primary = s.primary;
    const std::string spath = so->path;
    objs.push_back(std::move(so));
    for (size_t ci = 0; ci < s.characteristics.size(); ++ci) {
      const GattCharDef& c = s.characteristics[ci];
      auto co = std::make_unique<Obj>();
      co->srv = this;
      co->path = spath + "/chr" + std::to_string(ci);
      co->iface = kCharIface;
      co->uuid = c.uuid;
      co->parent = spath;
      co->flags = c.flags;
      co->value = c.value;
      co->counter = c.counter;
      co->period_ms = c.period_ms;
      co->width = c.value.empty() ? 1 : c.value.size();
      co->next_ns = now + static_cast<uint64_t>(c.period_ms) * 1000000ull;
      const std::string cpath = co->path;
      objs.push_back(std::move(co));
      for (size_t di = 0; di < c.descriptors.size(); ++di) {
        const GattDescDef& d = c.descriptors[di];
        auto dobj = std::make_unique<Obj>();
        dobj->srv = this;
        dobj->path = cpath + "/dsc" + std::to_string(di);
        dobj->iface = kDescIface;
        dobj->uuid = d.uuid;
        dobj->parent = cpath;
        dobj->flags = d.flags;
        dobj->value = d.value;
        objs.push_back(std::move(dobj));
      }
    }
  }
  for (auto& o : objs) {
    auto vt = std::make_unique<VtableBuilder>();
    vt->prop("UUID", "s", obj_get, false);
    if (o->iface == kServiceIface) {
      vt->prop("Primary", "b", obj_get, false);
    } else if (o->iface == kCharIface) {
      vt->prop("Service", "o", obj_get, false);
      vt->prop("Flags", "as", obj_get, false);
      vt->prop("Value", "ay", obj_get, true);
      vt->prop("Notifying", "b", obj_get, true);
      vt->method("ReadValue", "a{sv}", "ay", on_read);
      vt->method("WriteValue", "aya{sv}", "", on_write);
      vt->method("StartNotify", "", "", on_notify);
      vt->method("StopNotify", "", "", on_notify);
      vt->method("Confirm", "", "", on_confirm);
    } else {
      vt->prop("Characteristic", "o", obj_get, false);
      vt->prop("Flags", "as", obj_get, false);
      vt->prop("Value", "ay", obj_get, true);
      vt->method("ReadValue", "a{sv}", "ay", on_read);
      vt->method("WriteValue", "aya{sv}", "", on_write);
    }
    const int r = sd_bus_add_object_vtable(bus, &o->slot, o->path.c_str(), o->iface.c_str(), vt->finish(), o.get());
    if (r < 0) {
      LOG_WARN("gatt-server: cannot export {}: {}", o->path, strerror(-r));
      o->slot = nullptr;
    }
    o->vt = std::move(vt);
  }
  std::lock_guard<std::mutex> lk(m_);
  objs_ = std::move(objs);
}

void GattServer::register_app(sd_bus* bus) {
  if (registered_ || adapter_path_.empty() || !bus) return;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!defined_ || objs_.empty()) return;
    state_ = "registering";
  }
  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus, &m, "org.bluez", adapter_path_.c_str(), kGattMgrIface,
                                         "RegisterApplication");
  if (r >= 0) r = sd_bus_message_append(m, "o", kRoot);
  if (r >= 0) r = append_dict(m, {});
  if (r >= 0) r = sd_bus_call_async(bus, nullptr, m, app_registered, this, 15 * 1000000ull);
  sd_bus_message_unref(m);
  if (r < 0) {
    std::shared_ptr<std::promise<void>> w;
    {
      std::lock_guard<std::mutex> lk(m_);
      state_ = "failed";
      error_ = std::string("RegisterApplication: ") + strerror(-r);
      w = std::move(waiter_);
    }
    if (w) w->set_value();
  }
}

void GattServer::on_registered(const sd_bus_error* e) {
  std::shared_ptr<std::promise<void>> w;
  {
    std::lock_guard<std::mutex> lk(m_);
    registered_ = !e;
    state_ = e ? "failed" : "registered";
    error_ = e ? friendly(e) : "";
    w = std::move(waiter_);
  }
  if (e) LOG_WARN("gatt-server: RegisterApplication refused: {}", friendly(e));
  else LOG_INFO("gatt-server: application registered");
  if (w) w->set_value();
}

void GattServer::unregister_app(sd_bus* bus) {
  if (!registered_ || adapter_path_.empty() || !bus) return;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_call_method(bus, "org.bluez", adapter_path_.c_str(), kGattMgrIface, "UnregisterApplication",
                     &e, nullptr, "o", kRoot);
  sd_bus_error_free(&e);
  registered_ = false;
}

void GattServer::bus_opened(sd_bus* bus) {
  bus_ = bus;
  sd_bus_add_object_manager(bus, &om_slot_, kRoot);
  build(bus);
}

void GattServer::bus_closing() {
  std::lock_guard<std::mutex> lk(m_);
  drop_objects();
  objs_.clear();
  if (om_slot_) om_slot_ = sd_bus_slot_unref(om_slot_);
  registered_ = false;
  bus_ = nullptr;
  if (defined_ && state_ != "failed") state_ = "pending";
}

void GattServer::bluez_ready(sd_bus* bus, const std::string& adapter_path) {
  adapter_path_ = adapter_path;
  register_app(bus);
}

void GattServer::bluez_gone() {
  adapter_path_.clear();
  std::lock_guard<std::mutex> lk(m_);
  registered_ = false;
  if (defined_) state_ = "pending";
}

void GattServer::tick(sd_bus* bus, uint64_t now_ns) {
  std::vector<Obj*> fire;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& o : objs_) {
      if (!o->counter || now_ns < o->next_ns) continue;
      o->next_ns += static_cast<uint64_t>(o->period_ms) * 1000000ull;
      if (o->next_ns < now_ns) o->next_ns = now_ns + static_cast<uint64_t>(o->period_ms) * 1000000ull;
      o->value = gatt_counter_bytes(++o->count, o->width);
      if (o->notifying) fire.push_back(o.get());
    }
  }
  if (!bus) return;
  for (Obj* o : fire) sd_bus_emit_properties_changed(bus, o->path.c_str(), kCharIface, "Value", nullptr);
}

json GattServer::state() const {
  std::lock_guard<std::mutex> lk(m_);
  json values = json::array();
  for (const auto& o : objs_) {
    if (o->iface == kServiceIface) continue;
    values.push_back(json{{"path", o->path}, {"uuid", uuid_short(o->uuid)}, {"value", to_hex(o->value)},
                          {"text", printable(o->value)}, {"notifying", o->notifying}});
  }
  return json{{"defined", defined_},
              {"state", state_},
              {"error", error_},
              {"path", kRoot},
              {"app", defined_ ? gatt_app_json(def_) : json(nullptr)},
              {"values", values}};
}

bool GattServer::set(const GattAppDef& def, std::string* err) {
  if (!bt_.running()) {
    *err = "Bluetooth is not running";
    return false;
  }
  auto waiter = std::make_shared<std::promise<void>>();
  std::future<void> done = waiter->get_future();
  bt_.post([this, def, waiter](sd_bus* bus) {
    unregister_app(bus);
    {
      std::lock_guard<std::mutex> lk(m_);
      drop_objects();
      objs_.clear();
      def_ = def;
      defined_ = true;
      state_ = "pending";
      error_.clear();
      waiter_ = waiter;
    }
    if (bus) build(bus);
    if (bus && !adapter_path_.empty()) {
      register_app(bus);
      return;  // the reply completes the waiter
    }
    std::shared_ptr<std::promise<void>> w;
    {
      std::lock_guard<std::mutex> lk(m_);
      w = std::move(waiter_);
    }
    if (w) w->set_value();
  });
  done.wait_for(std::chrono::seconds(16));
  std::lock_guard<std::mutex> lk(m_);
  if (state_ == "failed") {
    *err = error_;
    return false;
  }
  return true;
}

void GattServer::clear() {
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> f = done->get_future();
  bt_.post([this, done](sd_bus* bus) {
    unregister_app(bus);
    {
      std::lock_guard<std::mutex> lk(m_);
      drop_objects();
      objs_.clear();
      defined_ = false;
      def_ = GattAppDef{};
      state_ = "none";
      error_.clear();
    }
    done->set_value();
  });
  f.wait_for(std::chrono::seconds(6));
}

}  // namespace btb
