#include "bluetooth.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <set>

#include "bluez_model.h"
#include "util/log.h"
#include "util/strings.h"
#include "uuids.h"

using json = nlohmann::json;

namespace btb {

uint64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

namespace {

constexpr const char* kBluez = "org.bluez";
constexpr const char* kAgentPath = "/org/btbench/agent";
constexpr const char* kAdapterIface = "org.bluez.Adapter1";
constexpr const char* kDeviceIface = "org.bluez.Device1";
constexpr const char* kBearerLeIface = "org.bluez.Bearer.LE1";
constexpr const char* kBearerBredrIface = "org.bluez.Bearer.BREDR1";
constexpr const char* kCharIface = "org.bluez.GattCharacteristic1";

// Calls that should be quick get this long: BlueZ answers a property write in milliseconds, and a
// bus thread stuck for the default 25 s is an agent that misses its pairings.
constexpr uint64_t kCallTimeoutUs = 5 * 1000000ull;
// Coalesces a burst of signals into one GetManagedObjects. While discovering, every advertisement
// moves an RSSI; on a Zero W in a busy room re-reading the whole tree for each would be the
// daemon's whole CPU, so that chatter has a floor of its own (on_signal). The console polls at 1 Hz.
constexpr uint64_t kRefreshDebounceNs = 300 * 1000000ull;
constexpr uint64_t kRefreshDebounceScanNs = 1000 * 1000000ull;
// ...and reads anyway now and then, so a missed signal cannot leave the console stale for long.
constexpr uint64_t kRefreshPeriodNs = 5 * 1000000000ull;
// The "bt" topic at most this often; a pairing question goes out at once.
constexpr uint64_t kChangeMinNs = 1000 * 1000000ull;
// How long the operator has to answer the agent: BlueZ's own timeout is 60 s, a little less here
// so ours is the answer BlueZ gets, not its own.
constexpr unsigned kRequestTimeoutS = 55;
constexpr unsigned kPairTimeoutS = 60;
constexpr unsigned kConnectTimeoutS = 30;
constexpr unsigned kReopenDelayS = 5;
// How long a web handler waits for the bus thread to take an answer.
constexpr auto kAnswerWait = std::chrono::seconds(6);

uint64_t s_to_ns(unsigned s) { return static_cast<uint64_t>(s) * 1000000000ull; }

std::string sixdigits(uint32_t v) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%06u", v % 1000000u);
  return buf;
}

}  // namespace

// ---- Decisions ----------------------------------------------------------------------------------

BtVerdict bt_agent_policy(const std::string& policy, BtAsk ask) {
  if (policy == "ask") return BtVerdict::Ask;
  return ask == BtAsk::Passkey ? BtVerdict::Ask : BtVerdict::Accept;
}

bool bt_agent_capability_ok(const std::string& cap) {
  static const char* const kCaps[] = {"DisplayOnly", "DisplayYesNo", "KeyboardOnly",
                                      "NoInputNoOutput", "KeyboardDisplay"};
  for (const char* c : kCaps)
    if (cap == c) return true;
  return false;
}

json bt_keep_adapter_settings(json kept, const json& set, uint32_t timeout_s) {
  if (!kept.is_object()) kept = json::object();
  for (const char* k : {"powered", "alias", "discoverable_timeout_s", "pairable", "discoverable"})
    if (set.contains(k)) kept[k] = set[k];
  if (kept.value("discoverable", false) && timeout_s) kept["discoverable"] = false;
  return kept;
}

std::vector<std::string> bt_bredr_connected(const json& objs, const std::string& adapter_path) {
  std::vector<std::string> out;
  if (!objs.is_object() || adapter_path.empty()) return out;
  const std::string prefix = adapter_path + "/dev_";
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (it.key().compare(0, prefix.size(), prefix) != 0 || !it.value().contains(kDeviceIface)) continue;
    const json& d = it.value()[kDeviceIface];
    if (!bool_of(d, "Paired")) continue;
    const bool bredr = it.value().contains(kBearerBredrIface)
                           ? bool_of(it.value()[kBearerBredrIface], "Connected")
                           : bool_of(d, "Connected") && d.contains("Class");
    if (bredr) out.push_back(upper(str_of(d, "Address")));
  }
  return out;
}

bool bt_connect_over_bredr(const json& obj) {
  if (!obj.is_object() || !obj.contains(kBearerBredrIface) || !obj.contains(kDeviceIface)) return false;
  // A2DP source/sink, AVRCP, HFP and HSP both ways, HID, PAN.
  static const char* const kClassic[] = {"0000110a", "0000110b", "0000110c", "0000110e", "0000111e", "0000111f",
                                         "00001108", "00001112", "00001124", "00001115", "00001116"};
  for (const std::string& u : strv_of(obj[kDeviceIface], "UUIDs"))
    for (const char* c : kClassic)
      if (u.compare(0, 8, c) == 0) return true;
  return false;
}

DDict bt_restore_adapter_props(const json& kept) {
  DDict props;
  if (!kept.is_object()) return props;
  const auto b = [&](const char* k) { return kept.contains(k) && kept[k].is_boolean(); };
  if (b("powered")) props.push_back({"Powered", DVar::boolean(kept["powered"].get<bool>())});
  // An adapter kept off takes nothing else now: BlueZ answers Not Ready, and what it keeps
  // itself (the alias) is already there when it is powered again.
  if (b("powered") && !kept["powered"].get<bool>()) return props;
  if (kept.contains("alias") && kept["alias"].is_string()) props.push_back({"Alias", DVar::str(kept["alias"].get<std::string>())});
  if (kept.contains("discoverable_timeout_s") && kept["discoverable_timeout_s"].is_number_integer()) {
    const long long t = kept["discoverable_timeout_s"].get<long long>();
    if (t >= 0 && t <= 0xffffffffLL) props.push_back({"DiscoverableTimeout", DVar::u32(static_cast<uint32_t>(t))});
  }
  if (b("pairable")) props.push_back({"Pairable", DVar::boolean(kept["pairable"].get<bool>())});
  if (b("discoverable")) props.push_back({"Discoverable", DVar::boolean(kept["discoverable"].get<bool>())});
  return props;
}

bool bt_scan_filter_from_json(const json& j, BtScanFilter* f, std::string* err) {
  BtScanFilter out;
  try {
    if (j.contains("transport")) {
      out.transport = j["transport"].get<std::string>();
      if (out.transport != "auto" && out.transport != "le" && out.transport != "bredr") {
        *err = "transport must be auto, le or bredr";
        return false;
      }
    }
    if (j.contains("rssi") && !j["rssi"].is_null()) {
      const int r = j["rssi"].get<int>();
      if (r < -127 || r > 20) {
        *err = "rssi must be -127..20";
        return false;
      }
      out.has_rssi = true;
      out.rssi = r;
    }
    if (j.contains("pathloss") && !j["pathloss"].is_null()) {
      const int p = j["pathloss"].get<int>();
      if (p < 0 || p > 137) {
        *err = "pathloss must be 0..137";
        return false;
      }
      out.has_pathloss = true;
      out.pathloss = p;
    }
    if (out.has_rssi && out.has_pathloss) {
      *err = "rssi and pathloss are either-or: BlueZ filters on one of them";
      return false;
    }
    if (j.contains("duplicate_data")) out.duplicate_data = j["duplicate_data"].get<bool>();
    if (j.contains("discoverable")) out.discoverable = j["discoverable"].get<bool>();
    if (j.contains("auto_connect")) out.auto_connect = j["auto_connect"].get<bool>();
    if (j.contains("uuids")) {
      for (const json& u : j["uuids"]) {
        const std::string full = uuid_full(u.get<std::string>());
        if (full.empty()) {
          *err = "not a UUID: " + u.get<std::string>();
          return false;
        }
        out.uuids.push_back(full);
      }
    }
    if (j.contains("pattern") && !j["pattern"].is_null()) {
      out.has_pattern = true;
      out.pattern = j["pattern"].get<std::string>();
      if (out.pattern.size() > 248) {
        *err = "pattern is a name or address prefix, at most 248 bytes";
        return false;
      }
    }
    if (out.auto_connect && !out.has_pattern) {
      *err = "auto_connect needs a pattern: it connects to the devices the pattern matches";
      return false;
    }
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
  *f = std::move(out);
  return true;
}

json bt_scan_filter_json(const BtScanFilter& f) {
  json u = json::array();
  for (const std::string& x : f.uuids) u.push_back(uuid_short(x));
  return json{{"transport", f.transport},
              {"rssi", f.has_rssi ? json(f.rssi) : json(nullptr)},
              {"pathloss", f.has_pathloss ? json(f.pathloss) : json(nullptr)},
              {"duplicate_data", f.duplicate_data},
              {"uuids", u},
              {"pattern", f.has_pattern ? json(f.pattern) : json(nullptr)},
              {"discoverable", f.discoverable},
              {"auto_connect", f.auto_connect}};
}

json bt_request_json(const BtRequest& r) {
  return json{{"id", r.id},           {"kind", r.kind},       {"address", r.address},
              {"name", r.name},       {"passkey", r.passkey}, {"uuid", r.uuid},
              {"uuid_name", r.uuid.empty() ? std::string{} : uuid_names().name(r.uuid)},
              {"expires_s", r.expires_s}};
}

// ---- The agent's open requests and the sd-bus callbacks -----------------------------------------

struct BtManager::Pending {
  sd_bus_message* msg = nullptr;  // held open until answered; null for a display-only request
  BtAsk ask = BtAsk::Confirm;
  BtRequest req;
  std::string dev_path;
  uint64_t deadline_ns = 0;
};

struct BtBus {
  struct AsyncOp {
    BtManager* self;
    std::string path;
    std::string method;
  };

  static int signal(sd_bus_message* m, void* self, sd_bus_error*) {
    static_cast<BtManager*>(self)->on_signal(m);
    return 0;
  }

  // BlueZ restarting drops our agent and every registration with it.
  static int owner(sd_bus_message* m, void* self, sd_bus_error*) {
    auto* b = static_cast<BtManager*>(self);
    const char *name = nullptr, *was = nullptr, *now = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &was, &now) < 0 || !name) return 0;
    if (strcmp(name, kBluez) != 0) return 0;
    b->bluez_owner_ = now ? now : "";
    b->agent_registered_ = false;
    b->clear_request(false);
    if (!b->ready_adapter_.empty()) {
      b->ready_adapter_.clear();
      for (BusUser* u : b->users_) u->bluez_gone();
    }
    if (b->bluez_owner_.empty()) LOG_WARN("bluetooth: BlueZ has left the bus");
    else LOG_INFO("bluetooth: BlueZ is on the bus ({})", b->bluez_owner_);
    b->dirty_ = true;
    return 0;
  }

  static int agent(sd_bus_message* m, void* self, sd_bus_error*) {
    return static_cast<BtManager*>(self)->agent_call(m, sd_bus_message_get_member(m));
  }

  static int async(sd_bus_message* m, void* data, sd_bus_error*) {
    auto* op = static_cast<AsyncOp*>(data);
    op->self->on_reply(op->path, op->method, m);
    return 0;
  }

  static const sd_bus_vtable* agent_vtable() {
    static const sd_bus_vtable v[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Release", "", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestPinCode", "o", "s", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("DisplayPinCode", "os", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestPasskey", "o", "u", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestConfirmation", "ou", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestAuthorization", "o", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("AuthorizeService", "os", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Cancel", "", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END};
    return v;
  }
};

// ---- Lifecycle -----------------------------------------------------------------------------------

BtManager::BtManager() : objs_(std::make_shared<const json>(json::object())) {}

BtManager::~BtManager() { stop(); }

void BtManager::set_adapter(std::string want) { want_adapter_ = std::move(want); }

void BtManager::set_state_file(std::string path) {
  state_file_ = std::move(path);
  std::ifstream f(state_file_);
  if (!f) return;
  try {
    const json j = json::parse(f);
    const std::string policy = j.value("agent", std::string{});
    const std::string cap = j.value("agent_capability", std::string{});
    if (policy == "auto" || policy == "ask") agent_policy_ = policy;
    if (bt_agent_capability_ok(cap)) agent_capability_ = cap;
    if (j.contains("adapters") && j["adapters"].is_object()) kept_adapters_ = j["adapters"];
    reconnect_on_ = j.value("reconnect", true);
    if (j.contains("reconnect_devices") && j["reconnect_devices"].is_array())
      for (const json& a : j["reconnect_devices"])
        if (a.is_string()) reconnect_.push_back(a.get<std::string>());
  } catch (const std::exception& e) {
    // A broken file is replaced by the next change; until then the defaults hold.
    LOG_WARN("bluetooth: {}: {}", state_file_, e.what());
  }
}

void BtManager::save_state_locked() const {
  if (state_file_.empty()) return;
  const std::string tmp = state_file_ + ".tmp";
  std::ofstream f(tmp);
  f << json{{"agent", agent_policy_}, {"agent_capability", agent_capability_}, {"adapters", kept_adapters_},
            {"reconnect", reconnect_on_}, {"reconnect_devices", reconnect_}}.dump(2)
    << "\n";
  f.close();
  if (!f || std::rename(tmp.c_str(), state_file_.c_str()) != 0)
    LOG_WARN("bluetooth: cannot write {}", state_file_);
}

namespace {
int restore_reply(sd_bus_message* m, void* userdata, sd_bus_error*) {
  std::unique_ptr<std::string> what(static_cast<std::string*>(userdata));
  if (sd_bus_message_is_method_error(m, nullptr)) {
    const sd_bus_error* e = sd_bus_message_get_error(m);
    LOG_WARN("bluetooth: restoring {}: {}", *what, e && e->message ? e->message : "refused");
  }
  return 0;
}
}  // namespace

void BtManager::set_reconnect(bool on) {
  std::lock_guard<std::mutex> lk(m_);
  reconnect_on_ = on;
  save_state_locked();
}

void BtManager::forget_reconnect(const std::string& address) {
  std::lock_guard<std::mutex> lk(m_);
  forgotten_.insert(upper(address));
  const auto it = std::find(reconnect_.begin(), reconnect_.end(), upper(address));
  if (it == reconnect_.end()) return;
  reconnect_.erase(it);
  save_state_locked();
}

// Bus thread, on every re-read. A device joins the list when it is seen connected over BR/EDR and
// leaves it when it is disconnected or removed from here (forget_reconnect) or is no longer paired;
// a device that went away by itself, or with the bench, stays: that is the one to call back.
void BtManager::note_connected(const json& objs, const std::string& apath) {
  if (apath.empty()) return;
  const std::vector<std::string> now = bt_bredr_connected(objs, apath);
  std::lock_guard<std::mutex> lk(m_);
  bool changed = false;
  for (auto it = forgotten_.begin(); it != forgotten_.end();)
    it = std::find(now.begin(), now.end(), *it) == now.end() ? forgotten_.erase(it) : std::next(it);
  for (const std::string& a : now)
    if (!forgotten_.count(a) && std::find(reconnect_.begin(), reconnect_.end(), a) == reconnect_.end()) {
      reconnect_.push_back(a);
      changed = true;
    }
  for (auto it = reconnect_.begin(); it != reconnect_.end();) {
    const std::string path = bt_device_path(apath, *it);
    const bool paired = objs.contains(path) && objs[path].contains(kDeviceIface) &&
                        bool_of(objs[path][kDeviceIface], "Paired");
    if (paired) {
      ++it;
    } else {
      it = reconnect_.erase(it);
      changed = true;
    }
  }
  if (changed) save_state_locked();
}

// Bus thread. Up to three tries, 35 s apart (a Connect gives up after 30): a speaker that is still
// booting, or busy connecting to us itself, gets another chance. The adapter must be powered first;
// waiting for that is not a try.
void BtManager::reconnect_tick(uint64_t now) {
  if (!reconnect_at_ns_ || now < reconnect_at_ns_) return;
  std::shared_ptr<const json> objs;
  std::string apath;
  std::vector<std::string> list;
  bool on;
  {
    std::lock_guard<std::mutex> lk(m_);
    objs = objs_;
    apath = adapter_path_;
    list = reconnect_;
    on = reconnect_on_;
  }
  reconnect_at_ns_ = 0;
  if (!on || list.empty() || apath.empty() || !objs->contains(apath)) return;
  if (!bool_of((*objs)[apath][kAdapterIface], "Powered")) {
    reconnect_at_ns_ = now + s_to_ns(2);
    return;
  }
  const std::vector<std::string> connected = bt_bredr_connected(*objs, apath);
  ++reconnect_tries_;
  bool tried = false;
  for (const std::string& a : list) {
    if (std::find(connected.begin(), connected.end(), a) != connected.end()) continue;
    const std::string path = bt_device_path(apath, a);
    if (!objs->contains(path)) continue;
    {
      std::lock_guard<std::mutex> lk(m_);
      const auto ds = dev_state_.find(path);
      if (ds != dev_state_.end() && !ds->second.busy.empty()) continue;
    }
    LOG_INFO("bluetooth: reconnecting {} (try {} of 3)", a, reconnect_tries_);
    // Over BR/EDR on purpose: a dual-mode device's Connect may pick LE, which is not the link the
    // audio profiles need.
    const bool bearer = (*objs)[path].contains(kBearerBredrIface);
    call_device(path, "Connect", "", "connecting", kConnectTimeoutS, bearer ? kBearerBredrIface : nullptr);
    tried = true;
  }
  if (tried && reconnect_tries_ < 3) reconnect_at_ns_ = now + s_to_ns(35);
}

// Bus thread. Each adapter gets its kept settings once each time BlueZ shows it, so a bluetoothd
// restarted by Setup, or a controller rebound after it hung, comes back as it was set. The writes
// are queued without waiting: bluetoothd handles them in order, and a refusal is only logged.
void BtManager::restore_adapters(const json& objs) {
  std::set<std::string> present;
  for (const BtAdapterId& a : model_adapter_ids(objs)) {
    present.insert(a.path);
    if (restored_.count(a.path)) continue;
    restored_.insert(a.path);
    const std::string name = a.path.substr(a.path.rfind('/') + 1);
    DDict props;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (kept_adapters_.contains(name)) props = bt_restore_adapter_props(kept_adapters_[name]);
    }
    if (props.empty()) continue;
    LOG_INFO("bluetooth: {}: restoring {} kept setting(s)", name, props.size());
    for (const auto& p : props) {
      sd_bus_message* m = nullptr;
      int r = sd_bus_message_new_method_call(bus_, &m, kBluez, a.path.c_str(), "org.freedesktop.DBus.Properties", "Set");
      if (r >= 0) r = sd_bus_message_append(m, "ss", kAdapterIface, p.first.c_str());
      if (r >= 0) r = append_variant(m, p.second);
      auto* what = new std::string(name + " " + p.first);
      if (r >= 0) r = sd_bus_call_async(bus_, nullptr, m, restore_reply, what, 5 * 1000000ull);
      if (r < 0) delete what;
      sd_bus_message_unref(m);
    }
    dirty_ = true;
  }
  for (auto it = restored_.begin(); it != restored_.end();) it = present.count(*it) ? std::next(it) : restored_.erase(it);
}
void BtManager::add_user(BusUser* u) { users_.push_back(u); }
void BtManager::on_notify(NotifyFn fn) { notify_ = std::move(fn); }
void BtManager::on_change(std::function<void(bool)> fn) { change_fn_ = std::move(fn); }

bool BtManager::start() {
  std::lock_guard<std::mutex> life(life_m_);
  if (running_.load()) return true;
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wake_fd_ < 0) {
    LOG_ERROR("bluetooth: eventfd: {}", strerror(errno));
    return false;
  }
  running_.store(true);
  thread_ = std::thread([this] { run(); });
  return true;
}

void BtManager::stop() {
  std::lock_guard<std::mutex> life(life_m_);
  if (!running_.exchange(false)) return;
  if (wake_fd_ >= 0) eventfd_write(wake_fd_, 1);
  if (thread_.joinable()) thread_.join();
  const int fd = wake_fd_.exchange(-1);
  if (fd >= 0) close(fd);
}

void BtManager::set_not_running_reason(std::string reason) {
  std::lock_guard<std::mutex> lk(m_);
  not_running_reason_ = std::move(reason);
}

void BtManager::post(std::function<void(sd_bus*)> fn) {
  if (!running_.load()) {
    fn(nullptr);  // nobody will ever run it: say "no bus" now rather than leave a waiter hanging
    return;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    commands_.push_back(std::move(fn));
  }
  const int fd = wake_fd_.load();
  if (fd >= 0) eventfd_write(fd, 1);
}

void BtManager::touch() {
  post([this](sd_bus*) { dirty_ = true; });
}

void BtManager::drain_commands() {
  std::deque<std::function<void(sd_bus*)>> todo;
  {
    std::lock_guard<std::mutex> lk(m_);
    todo.swap(commands_);
  }
  for (auto& fn : todo) fn(bus_);
}

void BtManager::run() {
  while (running_.load()) {
    if (!bus_ && !open_bus()) {
      if (error_ != logged_error_) {
        LOG_WARN("bluetooth: {} — retrying every {} s", error_, kReopenDelayS);
        logged_error_ = error_;
      }
      drain_commands();  // each sees no bus and gives up, rather than piling up for later
      pollfd pfd{wake_fd_, POLLIN, 0};
      poll(&pfd, 1, static_cast<int>(kReopenDelayS * 1000));
      eventfd_t v;
      eventfd_read(wake_fd_, &v);
      continue;
    }

    drain_commands();
    for (;;) {
      const int r = sd_bus_process(bus_, nullptr);
      if (r < 0) {
        LOG_WARN("bluetooth: lost the system bus: {}", strerror(-r));
        close_bus();
        break;
      }
      if (r == 0) break;
    }
    if (!bus_) continue;
    tick();

    pollfd pfd[2] = {{sd_bus_get_fd(bus_), static_cast<short>(sd_bus_get_events(bus_)), 0},
                     {wake_fd_, POLLIN, 0}};
    int timeout_ms = 100;  // the timers in tick() are this coarse, and need be no finer
    uint64_t until = 0;
    if (sd_bus_get_timeout(bus_, &until) >= 0 && until != UINT64_MAX) {
      const uint64_t now_us = mono_ns() / 1000;
      const uint64_t left_ms = until > now_us ? (until - now_us + 999) / 1000 : 0;
      timeout_ms = static_cast<int>(std::min<uint64_t>(left_ms, 100));
    }
    if (poll(pfd, 2, timeout_ms) > 0 && (pfd[1].revents & POLLIN)) {
      eventfd_t v;
      eventfd_read(wake_fd_, &v);
    }
  }
  if (bus_) {
    if (scan_until_ns_ || scanning_) stop_scan();
    close_bus();
  }
}

bool BtManager::open_bus() {
  int r = sd_bus_open_system(&bus_);
  if (r < 0) {
    bus_ = nullptr;
    set_error(std::string("cannot reach the system D-Bus: ") + strerror(-r));
    return false;
  }
  sd_bus_set_method_call_timeout(bus_, kCallTimeoutUs);

  r = sd_bus_add_object_vtable(bus_, &agent_slot_, kAgentPath, "org.bluez.Agent1",
                               BtBus::agent_vtable(), this);
  if (r < 0) LOG_WARN("bluetooth: cannot export the pairing agent: {}", strerror(-r));

  auto match = [this](const char* sender, const char* path, const char* iface, const char* member,
                      sd_bus_message_handler_t fn) {
    sd_bus_slot* slot = nullptr;
    if (sd_bus_match_signal(bus_, &slot, sender, path, iface, member, fn, this) >= 0)
      match_slots_.push_back(slot);
  };
  // Everything BlueZ says is a reason to look again (on_signal sorts out the notifications); the
  // debounce in tick() keeps a scan's flood to a re-read a second.
  match(kBluez, nullptr, nullptr, nullptr, BtBus::signal);
  match("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameOwnerChanged", BtBus::owner);

  agent_registered_ = false;
  dirty_ = true;
  ready_adapter_.clear();
  bluez_owner_.clear();
  for (BusUser* u : users_) u->bus_opened(bus_);
  return true;
}

void BtManager::close_bus() {
  clear_request(false);
  started_here_.clear();
  if (!ready_adapter_.empty()) {
    ready_adapter_.clear();
    for (BusUser* u : users_) u->bluez_gone();
  }
  for (BusUser* u : users_) u->bus_closing();
  for (sd_bus_slot* s : match_slots_) sd_bus_slot_unref(s);
  match_slots_.clear();
  if (agent_slot_) agent_slot_ = sd_bus_slot_unref(agent_slot_);
  if (bus_) bus_ = sd_bus_flush_close_unref(bus_);
  agent_registered_ = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    available_ = false;
    agent_registered_shared_ = false;
    scanning_ = false;
    objs_ = std::make_shared<const json>(json::object());
  }
  changed(false);
}

void BtManager::set_error(std::string e) {
  std::lock_guard<std::mutex> lk(m_);
  error_ = std::move(e);
}

void BtManager::changed(bool request) {
  change_pending_ = true;
  if (request) request_change_pending_ = true;
}

// A notification is a PropertiesChanged of a characteristic's Value, and at tens a second it must
// not cost a re-read of the whole tree each: it goes straight to the notify hook. A device's RSSI
// and advertising data change with every advertisement while scanning: those are re-read at the
// slow pace. Anything else BlueZ says (a connection, ServicesResolved, a pairing) is re-read soon.
void BtManager::on_signal(sd_bus_message* m) {
  const char* member = sd_bus_message_get_member(m);
  const char* siface = sd_bus_message_get_interface(m);
  // A bearer telling why it went down (org.bluez.Bearer.LE1 / .BREDR1): kept on the device, since
  // the reason is in the signal only and the next re-read shows just "not connected".
  if (member && siface && strcmp(member, "Disconnected") == 0 &&
      (strcmp(siface, kBearerLeIface) == 0 || strcmp(siface, kBearerBredrIface) == 0)) {
    const char *reason = nullptr, *message = nullptr;
    const char* path = sd_bus_message_get_path(m);
    if (path && sd_bus_message_read(m, "ss", &reason, &message) >= 0) {
      std::string r = reason ? reason : "";
      const std::string pre = "org.bluez.Reason.";
      if (r.compare(0, pre.size(), pre) == 0) r = lower(r.substr(pre.size()));
      const auto at = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
      LOG_INFO("bluetooth: {} {} disconnected: {} ({})", address_from_path(path),
               strcmp(siface, kBearerLeIface) == 0 ? "LE" : "BR/EDR", r, message ? message : "");
      std::lock_guard<std::mutex> lk(m_);
      dev_state_[path].last_disconnect = json{{"bearer", strcmp(siface, kBearerLeIface) == 0 ? "le" : "bredr"},
                                              {"reason", r},
                                              {"message", message ? message : ""},
                                              {"at_ms", at}};
    }
    sd_bus_message_rewind(m, 1);
  }
  if (member && strcmp(member, "PropertiesChanged") == 0) {
    const char* iface = nullptr;
    json changed;
    if (sd_bus_message_read(m, "s", &iface) >= 0 && iface && read_json(m, &changed) > 0 &&
        changed.is_object()) {
      sd_bus_message_rewind(m, 1);
      if (strcmp(iface, kCharIface) == 0 && changed.contains("Value")) {
        const char* path = sd_bus_message_get_path(m);
        if (notify_ && path) notify_(path, bytes_of(changed, "Value"));
        if (changed.size() == 1) return;
      }
      if (strcmp(iface, kDeviceIface) == 0) {
        bool chatter = true;
        for (auto it = changed.begin(); it != changed.end(); ++it) {
          const std::string& k = it.key();
          if (k != "RSSI" && k != "TxPower" && k != "ManufacturerData" && k != "ServiceData" &&
              k != "AdvertisingFlags" && k != "AdvertisingData")
            chatter = false;
        }
        if (chatter) {
          scan_dirty_ = true;
          return;
        }
      }
    }
    sd_bus_message_rewind(m, 1);
  }
  dirty_ = true;
}

// ---- The periodic part -------------------------------------------------------------------------

void BtManager::tick() {
  const uint64_t now = mono_ns();
  uint64_t scan_until;
  {
    std::lock_guard<std::mutex> lk(m_);
    scan_until = scan_until_ns_;
  }
  if ((dirty_ && now - last_refresh_ns_ >= kRefreshDebounceNs) ||
      (scan_dirty_ && now - last_refresh_ns_ >= kRefreshDebounceScanNs) ||
      now - last_refresh_ns_ >= kRefreshPeriodNs) {
    dirty_ = false;
    scan_dirty_ = false;
    last_refresh_ns_ = now;
    refresh();
  }
  expire_request(now);
  reconnect_tick(now);
  if (scan_until && now >= scan_until) {
    LOG_INFO("bluetooth: scan time is up");
    stop_scan();
  }
  for (BusUser* u : users_) u->tick(bus_, now);

  if (change_fn_ && (request_change_pending_ || (change_pending_ && now - last_change_ns_ >= kChangeMinNs))) {
    const bool req = request_change_pending_;
    change_pending_ = request_change_pending_ = false;
    last_change_ns_ = now;
    change_fn_(req);
  }
}

void BtManager::refresh() {
  if (bluez_owner_.empty()) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    if (sd_bus_call_method(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "GetNameOwner", &e, &reply, "s", kBluez) >= 0) {
      const char* owner = nullptr;
      if (sd_bus_message_read(reply, "s", &owner) >= 0 && owner) bluez_owner_ = owner;
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&e);
  }

  auto objs = std::make_shared<json>();
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  const int r = sd_bus_call_method(bus_, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
                                   "GetManagedObjects", &e, &reply, "");
  const bool ok = r >= 0 && read_json(reply, objs.get()) > 0 && objs->is_object();
  sd_bus_message_unref(reply);
  if (!ok) {
    const std::string why = is_error(&e, SD_BUS_ERROR_SERVICE_UNKNOWN) ||
                                    is_error(&e, SD_BUS_ERROR_NAME_HAS_NO_OWNER)
                                ? std::string("BlueZ is not running (bluetooth.service)")
                                : std::string("BlueZ: ") + (e.message ? e.message : "no answer");
    sd_bus_error_free(&e);
    if (why != logged_error_) {
      LOG_WARN("bluetooth: {}", why);
      logged_error_ = why;
    }
    restored_.clear();
    if (!ready_adapter_.empty()) {
      ready_adapter_.clear();
      for (BusUser* u : users_) u->bluez_gone();
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      available_ = false;
      error_ = why;
      objs_ = std::make_shared<const json>(json::object());
      adapter_path_.clear();
    }
    changed(false);
    return;
  }
  sd_bus_error_free(&e);
  if (!agent_registered_ || agent_cap_registered_ != agent_capability_) register_agent();
  restore_adapters(*objs);

  const std::string apath = bt_pick_adapter(model_adapter_ids(*objs), want_adapter_);
  std::string err;
  if (apath.empty()) {
    err = want_adapter_.empty()
              ? "no Bluetooth adapter (is the controller attached? dmesg | grep -i blue)"
              : "no Bluetooth adapter " + want_adapter_;
  }
  if (err != logged_error_) {
    if (!err.empty()) LOG_WARN("bluetooth: {}", err);
    else if (!apath.empty()) LOG_INFO("bluetooth: adapter {}", apath);
    logged_error_ = err;
  }

  {
    std::lock_guard<std::mutex> lk(m_);
    objs_ = objs;
    adapter_path_ = apath;
    available_ = !apath.empty();
    error_ = err;
    // A device that has gone (removed, or a temporary one expired) takes its busy/error with it.
    for (auto it = dev_state_.begin(); it != dev_state_.end();)
      it = objs->contains(it->first) ? std::next(it) : dev_state_.erase(it);
  }

  if (apath != ready_adapter_) {
    // The filter keys this adapter's BlueZ takes, for the console to offer only those.
    std::vector<std::string> filters;
    if (!apath.empty()) {
      sd_bus_message* reply = nullptr;
      sd_bus_error fe = SD_BUS_ERROR_NULL;
      if (sd_bus_call_method(bus_, kBluez, apath.c_str(), kAdapterIface, "GetDiscoveryFilters", &fe, &reply,
                             "") >= 0) {
        json v;
        if (read_json(reply, &v) > 0 && v.is_array())
          for (const json& x : v)
            if (x.is_string()) filters.push_back(x.get<std::string>());
      }
      sd_bus_message_unref(reply);
      sd_bus_error_free(&fe);
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      discovery_filters_ = std::move(filters);
    }
    if (!ready_adapter_.empty())
      for (BusUser* u : users_) u->bluez_gone();
    ready_adapter_ = apath;
    if (!apath.empty())
      for (BusUser* u : users_) u->bluez_ready(bus_, apath);
    // BlueZ (re)appeared with an adapter: at boot, or after a restart that dropped every link.
    // A few seconds' grace lets the adapter power up, and a peer that reconnects by itself do so.
    reconnect_tries_ = 0;
    reconnect_at_ns_ = apath.empty() ? 0 : mono_ns() + s_to_ns(5);
  }
  note_connected(*objs, apath);

  // A pairing in progress whose prompt is only for show is over once the device is paired.
  if (pending_ && !pending_->msg && objs->contains(pending_->dev_path) &&
      bool_of((*objs)[pending_->dev_path][kDeviceIface], "Paired")) {
    clear_request(false);
  }
  changed(false);
}

void BtManager::register_agent() {
  std::string cap;
  {
    std::lock_guard<std::mutex> lk(m_);
    cap = agent_capability_;
  }
  sd_bus_error e = SD_BUS_ERROR_NULL;
  if (agent_registered_) unregister_agent();
  int r = sd_bus_call_method(bus_, kBluez, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                             &e, nullptr, "os", kAgentPath, cap.c_str());
  if (r < 0 && !is_error(&e, "org.bluez.Error.AlreadyExists")) {
    LOG_WARN("bluetooth: cannot register the pairing agent: {}", friendly(&e));
    sd_bus_error_free(&e);
    return;
  }
  sd_bus_error_free(&e);
  // The default agent is the one BlueZ asks about pairings nobody started from a client: a phone
  // pairing with the bench. Without this those would be refused.
  r = sd_bus_call_method(bus_, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                         "RequestDefaultAgent", &e, nullptr, "o", kAgentPath);
  if (r < 0) LOG_WARN("bluetooth: not the default agent: {}", friendly(&e));
  sd_bus_error_free(&e);
  agent_registered_ = true;
  agent_cap_registered_ = cap;
  {
    std::lock_guard<std::mutex> lk(m_);
    agent_registered_shared_ = true;
  }
  LOG_INFO("bluetooth: pairing agent registered ({})", cap);
}

void BtManager::unregister_agent() {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_call_method(bus_, kBluez, "/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", &e,
                     nullptr, "o", kAgentPath);
  sd_bus_error_free(&e);
  agent_registered_ = false;
}

// ---- Scanning ------------------------------------------------------------------------------------

void BtManager::start_scan() {
  std::string apath;
  BtScanFilter f;
  {
    std::lock_guard<std::mutex> lk(m_);
    apath = adapter_path_;
    f = filter_;
  }
  if (!bus_ || apath.empty()) return;
  sd_bus_message* m = nullptr;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  DDict d{{"Transport", DVar::str(f.transport)}, {"DuplicateData", DVar::boolean(f.duplicate_data)}};
  if (f.has_rssi) d.push_back({"RSSI", DVar::i16(static_cast<int16_t>(f.rssi))});
  if (f.has_pathloss) d.push_back({"Pathloss", DVar::u16(static_cast<uint16_t>(f.pathloss))});
  if (!f.uuids.empty()) d.push_back({"UUIDs", DVar::strv(f.uuids)});
  if (f.has_pattern) d.push_back({"Pattern", DVar::str(f.pattern)});
  // Sent only when asked for: a BlueZ that does not know a key refuses the whole filter.
  if (f.discoverable) d.push_back({"Discoverable", DVar::boolean(true)});
  if (f.auto_connect) d.push_back({"AutoConnect", DVar::boolean(true)});
  int r = sd_bus_message_new_method_call(bus_, &m, kBluez, apath.c_str(), kAdapterIface,
                                         "SetDiscoveryFilter");
  if (r >= 0) r = append_dict(m, d);
  if (r >= 0) r = sd_bus_call(bus_, m, 0, &e, nullptr);
  sd_bus_message_unref(m);
  if (r < 0) {
    // Not a scan without the filter: what it found would look filtered and not be.
    const std::string why = "discovery filter refused: " + friendly(&e);
    LOG_WARN("bluetooth: {}", why);
    sd_bus_error_free(&e);
    std::lock_guard<std::mutex> lk(m_);
    error_ = why;
    scanning_ = false;
    scan_until_ns_ = 0;
    dirty_ = true;
    return;
  }
  sd_bus_error_free(&e);

  r = sd_bus_call_method(bus_, kBluez, apath.c_str(), kAdapterIface, "StartDiscovery", &e, nullptr, "");
  if (r < 0 && !is_error(&e, "org.bluez.Error.InProgress")) {
    const std::string why = "cannot scan: " + friendly(&e);
    LOG_WARN("bluetooth: {}", why);
    std::lock_guard<std::mutex> lk(m_);
    error_ = why;
    scanning_ = false;
    scan_until_ns_ = 0;
  } else {
    LOG_INFO("bluetooth: scanning ({})", f.transport);
  }
  sd_bus_error_free(&e);
  dirty_ = true;
}

void BtManager::stop_scan() {
  std::string apath;
  {
    std::lock_guard<std::mutex> lk(m_);
    scan_until_ns_ = 0;
    scanning_ = false;
    apath = adapter_path_;
  }
  if (!bus_ || apath.empty()) return;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_call_method(bus_, kBluez, apath.c_str(), kAdapterIface, "StopDiscovery", &e, nullptr, "");
  sd_bus_error_free(&e);
  // An empty filter clears ours, so the next client's scan is not narrowed by what we last asked.
  sd_bus_message* m = nullptr;
  if (sd_bus_message_new_method_call(bus_, &m, kBluez, apath.c_str(), kAdapterIface,
                                     "SetDiscoveryFilter") >= 0 &&
      append_dict(m, {}) >= 0) {
    sd_bus_call(bus_, m, 0, &e, nullptr);
  }
  sd_bus_message_unref(m);
  sd_bus_error_free(&e);
  dirty_ = true;
}

// ---- Device operations ---------------------------------------------------------------------------

void BtManager::set_device_state(const std::string& path, std::string busy, std::string error) {
  {
    std::lock_guard<std::mutex> lk(m_);
    DevState& s = dev_state_[path];
    s.busy = std::move(busy);
    s.error = std::move(error);
  }
  changed(false);
}

void BtManager::call_device(const std::string& path, const char* method, const std::string& uuid,
                            const char* busy, unsigned timeout_s, const char* iface) {
  if (!bus_) return;
  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus_, &m, kBluez, path.c_str(), iface ? iface : kDeviceIface, method);
  if (r >= 0 && !uuid.empty()) r = sd_bus_message_append(m, "s", uuid.c_str());
  auto* op = new BtBus::AsyncOp{this, path, method};
  sd_bus_slot* slot = nullptr;
  if (r >= 0) r = sd_bus_call_async(bus_, &slot, m, BtBus::async, op, s_to_ns(timeout_s) / 1000);
  sd_bus_message_unref(m);
  if (r < 0) {
    delete op;
    if (std::string(method) == "Pair") started_here_.erase(path);
    set_device_state(path, "", std::string(method) + ": " + strerror(-r));
    return;
  }
  // The slot owns the op from here: freed when the reply has been handled, and equally when the
  // bus is closed with the call still out, which never runs the callback.
  sd_bus_slot_set_destroy_callback(slot, [](void* p) { delete static_cast<BtBus::AsyncOp*>(p); });
  sd_bus_slot_set_floating(slot, 1);
  sd_bus_slot_unref(slot);
  LOG_INFO("bluetooth: {} {}{}", method, address_from_path(path), uuid.empty() ? "" : " " + uuid);
  set_device_state(path, busy, "");
}

void BtManager::on_reply(const std::string& path, const std::string& method, sd_bus_message* reply) {
  const sd_bus_error* e = sd_bus_message_get_error(reply);
  // Nothing to disconnect is done, not failed: it is also how a bearer's Disconnect answers when
  // it only cancelled a Connect still in progress.
  if (e && method == "Disconnect" && is_error(e, "org.bluez.Error.NotConnected")) e = nullptr;
  const std::string addr = address_from_path(path);
  if (method == "Pair") {
    started_here_.erase(path);
    if (pending_ && pending_->dev_path == path) clear_request(false);
  }
  if (e) LOG_WARN("bluetooth: {} {} failed: {} ({})", method, addr, friendly(e), e->name);
  else LOG_INFO("bluetooth: {} {} done", method, addr);
  set_device_state(path, "", e ? method + ": " + friendly(e) : "");
  dirty_ = true;
}

// ---- The agent -----------------------------------------------------------------------------------

bool BtManager::in_scope(const std::string& path) const {
  // With an adapter named (a PC), the others are the desktop's own: what pairs with those is not
  // the bench's to say yes to. Otherwise every adapter is the bench's (a board, plus a vhci).
  if (want_adapter_.empty()) return starts_with(path, "/org/bluez/");
  std::lock_guard<std::mutex> lk(m_);
  return !adapter_path_.empty() && starts_with(path, adapter_path_ + "/");
}

int BtManager::agent_call(sd_bus_message* m, const char* member) {
  // Only BlueZ may ask. The system bus's policy already keeps other users from calling an object
  // root exports; this keeps another root process from pairing a device by impersonating BlueZ.
  const char* sender = sd_bus_message_get_sender(m);
  if (!sender || bluez_owner_.empty() || bluez_owner_ != sender)
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "not BlueZ");
  const std::string what = member ? member : "";

  if (what == "Release") {
    agent_registered_ = false;
    return sd_bus_reply_method_return(m, "");
  }
  if (what == "Cancel") {
    // BlueZ has given up on the request already; there is no one left to answer.
    if (pending_) LOG_INFO("bluetooth: {} cancelled the pairing", pending_->req.name);
    clear_request(false);
    return sd_bus_reply_method_return(m, "");
  }

  const char* dev = nullptr;
  if (sd_bus_message_read(m, "o", &dev) < 0 || !dev)
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "bad arguments");
  const std::string path = dev;
  if (!in_scope(path)) {
    LOG_INFO("bluetooth: {} {} refused: not on the bench's adapter", what, path);
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "not the bench's adapter");
  }
  const auto started = started_here_.find(path);
  const bool here = started != started_here_.end();

  if (what == "DisplayPinCode" || what == "DisplayPasskey") {
    std::string shown;
    if (what == "DisplayPinCode") {
      const char* pin = nullptr;
      sd_bus_message_read(m, "s", &pin);
      shown = pin ? pin : "";
    } else {
      uint32_t key = 0;
      uint16_t entered = 0;
      sd_bus_message_read(m, "uq", &key, &entered);
      shown = sixdigits(key);
    }
    // Answered at once — there is nothing to say back — but kept on screen until the pairing
    // finishes, since that is the whole point of displaying it.
    const int r = sd_bus_reply_method_return(m, "");
    if (!pending_ || pending_->msg || pending_->dev_path != path || pending_->req.passkey != shown)
      hold_request(nullptr, BtAsk::Passkey, "display", path, shown, "");
    return r;
  }

  BtAsk ask = BtAsk::Confirm;
  std::string kind = "confirm";
  std::string passkey, uuid;
  if (what == "RequestConfirmation") {
    uint32_t key = 0;
    sd_bus_message_read(m, "u", &key);
    passkey = sixdigits(key);
  } else if (what == "RequestAuthorization") {
    ask = BtAsk::Authorize;
    kind = "authorize";
  } else if (what == "AuthorizeService") {
    ask = BtAsk::Service;
    kind = "service";
    const char* u = nullptr;
    sd_bus_message_read(m, "s", &u);
    uuid = u ? u : "";
  } else if (what == "RequestPinCode") {
    ask = BtAsk::Pin;
    kind = "pin";
  } else if (what == "RequestPasskey") {
    ask = BtAsk::Passkey;
    kind = "passkey";
  } else {
    return sd_bus_reply_method_errorf(m, "org.freedesktop.DBus.Error.UnknownMethod", "%s", what.c_str());
  }

  std::string policy;
  {
    std::lock_guard<std::mutex> lk(m_);
    policy = agent_policy_;
  }
  // A PIN given with the Pair request is the answer to that pairing's PinCode question.
  if (ask == BtAsk::Pin && here && !started->second.empty()) {
    LOG_INFO("bluetooth: {} {} — answered with the PIN given", what, address_from_path(path));
    return sd_bus_reply_method_return(m, "s", started->second.c_str());
  }
  if (bt_agent_policy(policy, ask) == BtVerdict::Accept) {
    LOG_INFO("bluetooth: {} {}{} — accepted (agent auto)", what, address_from_path(path),
             passkey.empty() ? "" : " " + passkey);
    if (ask == BtAsk::Pin) return sd_bus_reply_method_return(m, "s", "0000");
    return sd_bus_reply_method_return(m, "");
  }
  hold_request(m, ask, kind, path, passkey, uuid);
  return 1;
}

void BtManager::hold_request(sd_bus_message* m, BtAsk ask, const std::string& kind,
                             const std::string& dev_path, const std::string& passkey,
                             const std::string& uuid) {
  // BlueZ asks one thing at a time; a new question means the old one is moot.
  clear_request(true);
  auto* p = new Pending;
  p->msg = m ? sd_bus_message_ref(m) : nullptr;
  p->ask = ask;
  p->dev_path = dev_path;
  p->deadline_ns = mono_ns() + s_to_ns(kRequestTimeoutS);
  p->req.id = next_request_id_++;
  p->req.kind = kind;
  p->req.passkey = passkey;
  p->req.uuid = uuid.empty() ? std::string{} : uuid_short(uuid);
  p->req.address = address_from_path(dev_path);
  {
    std::shared_ptr<const json> objs;
    {
      std::lock_guard<std::mutex> lk(m_);
      objs = objs_;
    }
    if (objs->contains(dev_path)) p->req.name = str_of((*objs)[dev_path][kDeviceIface], "Alias");
  }
  if (p->req.name.empty() && bus_) {
    // A device that has never been scanned is not in the last read; its name is still known.
    char* alias = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_string(bus_, kBluez, dev_path.c_str(), kDeviceIface, "Alias", &e,
                                   &alias) >= 0 && alias) {
      p->req.name = alias;
    }
    free(alias);
    sd_bus_error_free(&e);
  }
  if (p->req.name.empty()) p->req.name = p->req.address;
  pending_ = p;
  if (m) {
    LOG_INFO("bluetooth: {} ({}) asks: {}{} — waiting for an answer", p->req.name, p->req.address,
             kind, passkey.empty() ? "" : " " + passkey);
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    has_request_ = true;
    request_ = p->req;
    request_deadline_ns_ = p->deadline_ns;
  }
  changed(true);
}

void BtManager::clear_request(bool reply_rejected) {
  if (!pending_) return;
  if (pending_->msg) {
    if (reply_rejected)
      sd_bus_reply_method_errorf(pending_->msg, "org.bluez.Error.Canceled", "superseded");
    sd_bus_message_unref(pending_->msg);
  }
  delete pending_;
  pending_ = nullptr;
  {
    std::lock_guard<std::mutex> lk(m_);
    has_request_ = false;
    request_ = BtRequest{};
    request_deadline_ns_ = 0;
  }
  changed(true);
}

void BtManager::expire_request(uint64_t now_ns) {
  if (!pending_ || now_ns < pending_->deadline_ns) return;
  if (pending_->msg) {
    LOG_INFO("bluetooth: nobody answered {} in time — declined", pending_->req.name);
    sd_bus_reply_method_errorf(pending_->msg, "org.bluez.Error.Canceled", "nobody answered in time");
  }
  clear_request(false);
}

// ---- Called from web handlers --------------------------------------------------------------------

std::shared_ptr<const json> BtManager::objects() const {
  std::lock_guard<std::mutex> lk(m_);
  return objs_;
}

std::string BtManager::adapter_path() const {
  std::lock_guard<std::mutex> lk(m_);
  return adapter_path_;
}

json BtManager::state() const {
  std::shared_ptr<const json> objs;
  std::string apath, error;
  bool available, has_req, scanning, agent_reg;
  BtRequest req;
  uint64_t deadline, scan_until;
  std::map<std::string, DevState> ds;
  std::string policy, cap;
  bool reconnect_on;
  std::vector<std::string> reconnect;
  BtScanFilter filter;
  std::vector<std::string> filters;
  {
    std::lock_guard<std::mutex> lk(m_);
    objs = objs_;
    apath = adapter_path_;
    available = available_;
    error = error_;
    has_req = has_request_;
    req = request_;
    deadline = request_deadline_ns_;
    ds = dev_state_;
    policy = agent_policy_;
    cap = agent_capability_;
    scanning = scanning_;
    scan_until = scan_until_ns_;
    filter = filter_;
    filters = discovery_filters_;
    agent_reg = agent_registered_shared_;
    reconnect_on = reconnect_on_;
    reconnect = reconnect_;
    if (!running_.load()) {
      available = false;
      error = not_running_reason_.empty() ? "Bluetooth is not running" : not_running_reason_;
    }
  }
  const uint64_t now = mono_ns();
  json adapters = model_adapters(*objs);
  json adapter = nullptr;
  for (const json& a : adapters)
    if (a["path"] == apath) adapter = a;
  json devices = model_devices(*objs, want_adapter_.empty() ? std::string{} : apath);
  for (json& d : devices) {
    const auto it = ds.find(d["path"].get<std::string>());
    d["busy"] = it == ds.end() ? "" : it->second.busy;
    d["error"] = it == ds.end() ? "" : it->second.error;
    d["last_disconnect"] = it == ds.end() || it->second.last_disconnect.is_null() ? json(nullptr)
                                                                                 : it->second.last_disconnect;
  }
  if (has_req) req.expires_s = deadline > now ? static_cast<unsigned>((deadline - now) / 1000000000ull) : 0;
  const bool discovering = adapter.is_object() && adapter.value("discovering", false);
  return json{{"available", available},
              {"error", error},
              {"adapter", adapter},
              {"adapters", adapters},
              {"devices", devices},
              {"request", has_req ? bt_request_json(req) : json(nullptr)},
              {"agent", {{"policy", policy}, {"capability", cap}, {"registered", agent_reg}}},
              {"reconnect", {{"on", reconnect_on}, {"devices", reconnect}}},
              {"discovery",
               {{"on", discovering},
                {"ours", scanning},
                {"filter", bt_scan_filter_json(filter)},
                // The keys the adapter's BlueZ takes (GetDiscoveryFilters): UUIDs, RSSI, Pathloss, ...
                {"filters", filters},
                {"until_s", scan_until > now ? json((scan_until - now) / 1000000000ull) : json(nullptr)}}}};
}

std::string BtManager::device_path(const std::string& address) const {
  if (!bt_address_ok(address)) return {};
  std::shared_ptr<const json> objs;
  std::string apath;
  {
    std::lock_guard<std::mutex> lk(m_);
    objs = objs_;
    apath = adapter_path_;
  }
  // The primary adapter's first; then any adapter's (a vhci's), unless one was named.
  if (!apath.empty()) {
    const std::string p = bt_device_path(apath, address);
    if (objs->contains(p) && (*objs)[p].contains(kDeviceIface)) return p;
  }
  if (!want_adapter_.empty()) return {};
  for (const BtAdapterId& a : model_adapter_ids(*objs)) {
    const std::string p = bt_device_path(a.path, address);
    if (objs->contains(p) && (*objs)[p].contains(kDeviceIface)) return p;
  }
  return {};
}

json BtManager::device(const std::string& address) const {
  const std::string path = device_path(address);
  if (path.empty()) return nullptr;
  std::shared_ptr<const json> objs;
  DevState st;
  {
    std::lock_guard<std::mutex> lk(m_);
    objs = objs_;
    const auto it = dev_state_.find(path);
    if (it != dev_state_.end()) st = it->second;
  }
  json d = model_device(path, (*objs)[path][kDeviceIface], &(*objs)[path]);
  d["busy"] = st.busy;
  d["error"] = st.error;
  d["last_disconnect"] = st.last_disconnect.is_null() ? json(nullptr) : st.last_disconnect;
  // How much of it there is beyond the device itself: a resolved GATT database, A2DP endpoints.
  size_t services = 0, endpoints = 0, transports = 0;
  const std::string prefix = path + "/";
  for (auto it = objs->begin(); it != objs->end(); ++it) {
    if (!starts_with(it.key(), prefix)) continue;
    if (it->contains("org.bluez.GattService1")) ++services;
    if (it->contains("org.bluez.MediaEndpoint1")) ++endpoints;
    if (it->contains("org.bluez.MediaTransport1")) ++transports;
  }
  d["gatt_services"] = services;
  d["media_endpoints"] = endpoints;
  d["media_transports"] = transports;
  return d;
}

bool BtManager::has_request(BtRequest* out) const {
  std::lock_guard<std::mutex> lk(m_);
  if (!has_request_) return false;
  *out = request_;
  const uint64_t now = mono_ns();
  out->expires_s = request_deadline_ns_ > now
                       ? static_cast<unsigned>((request_deadline_ns_ - now) / 1000000000ull) : 0;
  return true;
}

bool BtManager::set_adapter_props(const std::string& adapter, const json& j, std::string* err) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  std::string path;
  {
    std::lock_guard<std::mutex> lk(m_);
    path = adapter.empty() ? adapter_path_ : "/org/bluez/" + adapter;
    if (path.empty() || !objs_->contains(path)) {
      *err = adapter.empty() ? "no adapter" : "no such adapter";
      return false;
    }
  }
  // Every field is checked before anything is applied, so a rejected body changes nothing.
  DDict props;
  try {
    if (j.contains("powered")) props.push_back({"Powered", DVar::boolean(j["powered"].get<bool>())});
    if (j.contains("alias")) {
      const std::string a = j["alias"].get<std::string>();
      if (a.size() > 248) {
        *err = "alias is at most 248 bytes";
        return false;
      }
      props.push_back({"Alias", DVar::str(a)});
    }
    // The timeout first: BlueZ starts counting when Discoverable goes on, with the value in force.
    if (j.contains("discoverable_timeout_s"))
      props.push_back({"DiscoverableTimeout", DVar::u32(j["discoverable_timeout_s"].get<uint32_t>())});
    if (j.contains("pairable")) props.push_back({"Pairable", DVar::boolean(j["pairable"].get<bool>())});
    if (j.contains("discoverable"))
      props.push_back({"Discoverable", DVar::boolean(j["discoverable"].get<bool>())});
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
  for (const auto& p : props) {
    const BtCallResult r = set_property(path, kAdapterIface, p.first, p.second, 5000);
    if (!r.ok) {
      *err = p.first + ": " + r.error;
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    const std::string name = path.substr(path.rfind('/') + 1);
    uint32_t timeout = 0;
    if (j.contains("discoverable_timeout_s")) timeout = j["discoverable_timeout_s"].get<uint32_t>();
    else if (objs_->contains(path) && (*objs_)[path].contains(kAdapterIface))
      timeout = static_cast<uint32_t>(num_of((*objs_)[path][kAdapterIface], "DiscoverableTimeout"));
    kept_adapters_[name] = bt_keep_adapter_settings(kept_adapters_.value(name, json::object()), j, timeout);
    save_state_locked();
  }
  touch();
  return true;
}

bool BtManager::set_agent(const std::string& policy, const std::string& capability, std::string* err) {
  if (!policy.empty() && policy != "auto" && policy != "ask") {
    *err = "agent must be auto or ask";
    return false;
  }
  if (!capability.empty() && !bt_agent_capability_ok(capability)) {
    *err = "capability must be DisplayOnly, DisplayYesNo, KeyboardOnly, NoInputNoOutput or KeyboardDisplay";
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!policy.empty()) agent_policy_ = policy;
    if (!capability.empty()) agent_capability_ = capability;
    save_state_locked();
  }
  // A new capability needs a new registration; tick() → refresh() notices the difference.
  touch();
  return true;
}

bool BtManager::scan(bool on, const BtScanFilter& f, unsigned seconds, std::string* err) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    if (on && adapter_path_.empty()) {
      *err = "no adapter";
      return false;
    }
    if (on) {
      filter_ = f;
      scanning_ = true;
      scan_until_ns_ = seconds ? mono_ns() + s_to_ns(seconds) : 0;
    }
  }
  post([this, on](sd_bus*) {
    if (on) start_scan();
    else stop_scan();
  });
  return true;
}

bool BtManager::device_action(const std::string& address, Action action, const std::string& arg,
                              std::string* err) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  const std::string path = device_path(address);
  if (path.empty()) {
    *err = "no such device";
    return false;
  }
  {
    // Shown at once, so a console that reads the state straight back sees its request running.
    std::lock_guard<std::mutex> lk(m_);
    DevState& s = dev_state_[path];
    s.error.clear();
    if (action == Action::Pair) s.busy = "pairing";
    if (action == Action::Connect) s.busy = "connecting";
    if (action == Action::Disconnect || action == Action::DisconnectBearer) s.busy = "disconnecting";
    if (action == Action::ConnectBearer) s.busy = "connecting";
  }
  // Disconnected or removed on purpose: not to be called back at the next start.
  if ((action == Action::Disconnect && arg.empty()) || action == Action::Remove ||
      (action == Action::DisconnectBearer && arg == "bredr"))
    forget_reconnect(address);
  const bool bearer = action == Action::ConnectBearer || action == Action::DisconnectBearer;
  if (bearer && arg != "le" && arg != "bredr") {
    std::lock_guard<std::mutex> lk(m_);
    dev_state_[path].busy.clear();
    *err = "bearer is le or bredr";
    return false;
  }
  const std::string uuid = arg.empty() || action == Action::Pair || bearer ? std::string{} : uuid_full(arg);
  if (!arg.empty() && action != Action::Pair && !bearer && uuid.empty()) {
    *err = "not a UUID: " + arg;
    return false;
  }
  post([this, path, action, arg, uuid](sd_bus* bus) {
    if (!bus) return;
    switch (action) {
      case Action::Pair:
        started_here_[path] = arg;
        call_device(path, "Pair", "", "pairing", kPairTimeoutS);
        break;
      case Action::CancelPairing:
        call_device(path, "CancelPairing", "", "", 5);
        break;
      case Action::Connect: {
        bool bredr = false;
        if (uuid.empty()) {
          std::lock_guard<std::mutex> lk(m_);
          bredr = objs_->contains(path) && bt_connect_over_bredr((*objs_)[path]);
        }
        call_device(path, uuid.empty() ? "Connect" : "ConnectProfile", uuid, "connecting", kConnectTimeoutS,
                    bredr ? kBearerBredrIface : nullptr);
        break;
      }
      case Action::Disconnect:
        call_device(path, uuid.empty() ? "Disconnect" : "DisconnectProfile", uuid, "disconnecting",
                    kConnectTimeoutS);
        break;
      case Action::ConnectBearer:
      case Action::DisconnectBearer:
        // A device seen on one transport only has no bearer object for the other: BlueZ answers
        // UnknownObject, shown on the device like any refusal.
        call_device(path, action == Action::ConnectBearer ? "Connect" : "Disconnect", "",
                    action == Action::ConnectBearer ? "connecting" : "disconnecting", kConnectTimeoutS,
                    arg == "le" ? kBearerLeIface : kBearerBredrIface);
        break;
      case Action::Remove: {
        const std::string apath = path.substr(0, path.rfind('/'));
        sd_bus_error e = SD_BUS_ERROR_NULL;
        if (sd_bus_call_method(bus, kBluez, apath.c_str(), kAdapterIface, "RemoveDevice", &e,
                               nullptr, "o", path.c_str()) < 0) {
          set_device_state(path, "", "RemoveDevice: " + friendly(&e));
        } else {
          LOG_INFO("bluetooth: removed {}", address_from_path(path));
          std::lock_guard<std::mutex> lk(m_);
          dev_state_.erase(path);
        }
        sd_bus_error_free(&e);
        dirty_ = true;
        break;
      }
    }
  });
  return true;
}

bool BtManager::set_device_props(const std::string& address, const json& j, std::string* err) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  const std::string path = device_path(address);
  if (path.empty()) {
    *err = "no such device";
    return false;
  }
  DDict props;
  try {
    if (j.contains("alias")) props.push_back({"Alias", DVar::str(j["alias"].get<std::string>())});
    if (j.contains("trusted")) props.push_back({"Trusted", DVar::boolean(j["trusted"].get<bool>())});
    if (j.contains("blocked")) props.push_back({"Blocked", DVar::boolean(j["blocked"].get<bool>())});
    if (j.contains("wake_allowed"))
      props.push_back({"WakeAllowed", DVar::boolean(j["wake_allowed"].get<bool>())});
    if (j.contains("preferred_bearer")) {
      const std::string b = j["preferred_bearer"].get<std::string>();
      if (b != "last-used" && b != "bredr" && b != "le" && b != "last-seen") {
        *err = "preferred_bearer is last-used, bredr, le or last-seen";
        return false;
      }
      props.push_back({"PreferredBearer", DVar::str(b)});
    }
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
  for (const auto& p : props) {
    const BtCallResult r = set_property(path, kDeviceIface, p.first, p.second, 5000);
    if (!r.ok) {
      *err = p.first + ": " + r.error;
      return false;
    }
  }
  touch();
  return true;
}

bool BtManager::answer(uint64_t id, bool accept, const std::string& value, std::string* err) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  auto done = std::make_shared<std::promise<std::string>>();
  std::future<std::string> result = done->get_future();
  post([this, id, accept, value, done](sd_bus*) {
    if (!pending_ || pending_->req.id != id) {
      done->set_value("no such request — it was answered, withdrawn or has expired");
      return;
    }
    Pending& p = *pending_;
    if (!p.msg) {  // a code on display: nothing to say back, only to stop showing it
      clear_request(false);
      done->set_value({});
      return;
    }
    int r = 0;
    if (!accept) {
      LOG_INFO("bluetooth: {} declined", p.req.name);
      r = sd_bus_reply_method_errorf(p.msg, "org.bluez.Error.Rejected", "declined");
    } else if (p.req.kind == "pin") {
      if (value.empty() || value.size() > 16) {
        done->set_value("a PIN is 1 to 16 characters");
        return;
      }
      r = sd_bus_reply_method_return(p.msg, "s", value.c_str());
    } else if (p.req.kind == "passkey") {
      char* end = nullptr;
      const unsigned long v = strtoul(value.c_str(), &end, 10);
      if (value.empty() || *end || v > 999999ul) {
        done->set_value("a passkey is up to six digits");
        return;
      }
      r = sd_bus_reply_method_return(p.msg, "u", static_cast<uint32_t>(v));
    } else {
      r = sd_bus_reply_method_return(p.msg, "");
    }
    if (accept) LOG_INFO("bluetooth: {} accepted", p.req.name);
    clear_request(false);
    done->set_value(r < 0 ? std::string("BlueZ is gone: ") + strerror(-r) : std::string{});
  });
  if (result.wait_for(kAnswerWait) != std::future_status::ready) {
    *err = "Bluetooth is not answering";
    return false;
  }
  const std::string e = result.get();
  if (!e.empty()) {
    *err = e;
    return false;
  }
  return true;
}

// ---- Calls for the other modules ----------------------------------------------------------------

namespace {

struct CallOp {
  std::shared_ptr<std::promise<BtCallResult>> done;
};

int call_reply(sd_bus_message* m, void* data, sd_bus_error*) {
  auto* op = static_cast<CallOp*>(data);
  BtCallResult r;
  const sd_bus_error* e = sd_bus_message_get_error(m);
  if (e) {
    r.error = friendly(e);
    r.error_name = e->name ? e->name : "";
  } else {
    r.ok = true;
    for (;;) {
      json v;
      if (read_json(m, &v) <= 0) break;
      r.reply.push_back(std::move(v));
    }
  }
  op->done->set_value(std::move(r));
  op->done.reset();
  return 0;
}

}  // namespace

BtCallResult BtManager::call(const std::string& path, const std::string& iface, const std::string& method,
                             std::function<int(sd_bus_message*)> append, int timeout_ms) {
  auto done = std::make_shared<std::promise<BtCallResult>>();
  std::future<BtCallResult> fut = done->get_future();
  post([path, iface, method, append, timeout_ms, done](sd_bus* bus) {
    BtCallResult r;
    if (!bus) {
      r.error = "no D-Bus connection";
      done->set_value(std::move(r));
      return;
    }
    sd_bus_message* m = nullptr;
    int rc = sd_bus_message_new_method_call(bus, &m, kBluez, path.c_str(), iface.c_str(), method.c_str());
    if (rc >= 0 && append) rc = append(m);
    auto* op = new CallOp{done};
    sd_bus_slot* slot = nullptr;
    if (rc >= 0)
      rc = sd_bus_call_async(bus, &slot, m, call_reply, op, static_cast<uint64_t>(timeout_ms) * 1000);
    sd_bus_message_unref(m);
    if (rc < 0) {
      delete op;
      r.error = std::string("cannot call ") + method + ": " + strerror(-rc);
      done->set_value(std::move(r));
      return;
    }
    // A bus closed with the call out never runs the callback; the destroy hook then answers the
    // waiter instead of leaving it to its timeout.
    sd_bus_slot_set_destroy_callback(slot, [](void* p) {
      auto* o = static_cast<CallOp*>(p);
      if (o->done) {
        BtCallResult x;
        x.error = "the D-Bus connection closed";
        o->done->set_value(std::move(x));
      }
      delete o;
    });
    sd_bus_slot_set_floating(slot, 1);
    sd_bus_slot_unref(slot);
  });
  if (fut.wait_for(std::chrono::milliseconds(timeout_ms + 1500)) != std::future_status::ready) {
    BtCallResult r;
    r.timed_out = true;
    r.error = "no answer in time";
    return r;
  }
  BtCallResult r = fut.get();
  if (r.ok) touch();
  return r;
}

BtCallResult BtManager::set_property(const std::string& path, const std::string& iface,
                                     const std::string& prop, const DVar& value, int timeout_ms) {
  return call(path, "org.freedesktop.DBus.Properties", "Set",
              [iface, prop, value](sd_bus_message* m) {
                int r = sd_bus_message_append(m, "ss", iface.c_str(), prop.c_str());
                if (r >= 0) r = append_variant(m, value);
                return r;
              },
              timeout_ms);
}

}  // namespace btb
