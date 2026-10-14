#include "bluetooth.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
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
// The whole tree is read asynchronously, one read at a time, and may take this long: on a Zero W
// scanning a busy room bluetoothd can need seconds for it, and a late answer is still an answer.
// Only BlueZ leaving the bus (or not being there) clears the devices; a read that times out leaves
// the last tree in place, marked stale.
constexpr uint64_t kRefreshTimeoutUs = 20 * 1000000ull;
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
              {"adapter", r.adapter},
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
    std::string iface;  // empty: Device1
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
    // A re-read can find the new BlueZ (GetNameOwner) before this signal is handled; the signal
    // then names the owner already adopted, with the agent and every BusUser set up for it.
    // Treating it as yet another BlueZ registered the agent and the player twice.
    if (now && *now && b->bluez_owner_ == now) return 0;
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
    op->self->on_reply(op->path, op->method, op->iface, m);
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
void BtManager::set_default_alias(bool on) { default_alias_ = on; }
void BtManager::set_accept_central(bool on) { accept_central_ = on; }

namespace {

// The kernel's hostname: btbench-hostname has made it the board's own (btbench-257bfc).
std::string host_name() {
  char h[256] = {};
  if (gethostname(h, sizeof h - 1) != 0) return {};
  return h;
}

// Whether the controller behind an adapter object is on USB: its hci device's parent in sysfs is a
// USB interface (/sys/class/bluetooth/hci1/device → …/usb1/1-1/1-1.1/1-1.1:1.0), where the board's
// own is a serdev on a UART or an SDIO function. False for an adapter with no hci in sysfs (a fake
// BlueZ's).
bool adapter_on_usb(const std::string& apath) {
  const std::string name = adapter_from_path(apath);
  if (name.empty() || name.find('/') != std::string::npos) return false;
  char buf[PATH_MAX];
  const char* r = realpath(("/sys/class/bluetooth/" + name + "/device").c_str(), buf);
  return r && strstr(r, "/usb") != nullptr;
}

// hciN's link mode to "accept, and become central" (hciconfig hciN lm accept,master). The kernel
// keeps it until the controller is unregistered (a rebind after a hang makes a new one).
bool accept_as_central(const std::string& name, std::string* err) {
  struct HciDevReq {
    uint16_t dev_id;
    uint32_t dev_opt;
  };
  constexpr unsigned long kHciSetLinkMode = _IOW('H', 226, int);
  constexpr uint32_t kLmAccept = 0x8000, kLmMaster = 0x0001;
  if (name.compare(0, 3, "hci") != 0 || name.size() < 4) {
    *err = "not an hci device";
    return false;
  }
  const int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
  if (fd < 0) {
    *err = strerror(errno);
    return false;
  }
  HciDevReq dr{static_cast<uint16_t>(atoi(name.c_str() + 3)), kLmAccept | kLmMaster};
  const bool ok = ioctl(fd, kHciSetLinkMode, &dr) == 0;
  if (!ok) *err = strerror(errno);
  close(fd);
  return ok;
}

}  // namespace

std::vector<BtAdapterId> BtManager::adapter_ids(const json& objs) const {
  std::vector<BtAdapterId> ids = model_adapter_ids(objs);
  for (BtAdapterId& a : ids) {
    a.address = upper(a.address);
    a.usb = adapter_on_usb(a.path);
  }
  return ids;
}

// The board's name (btbench-257bfc) for its own controller; a USB adapter adds the end of its own
// address (btbench-257bfc-5e2e), so a phone that scans tells the two apart.
std::string BtManager::default_alias(const BtAdapterId& a) const {
  std::string host = host_name();
  if (host.empty() || !a.usb || a.address.size() != 17) return host;
  return host + "-" + lower(a.address.substr(12, 2) + a.address.substr(15, 2));
}

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
    primary_ = upper(j.value("primary", std::string{}));
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
            {"primary", primary_}}.dump(2)
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

// Bus thread. Each adapter gets its kept settings once each time BlueZ shows it, so a bluetoothd
// restarted by Setup, or a controller rebound after it hung, comes back as it was set. The writes
// are queued without waiting: bluetoothd handles them in order, and a refusal is only logged.
// An adapter that appears (BlueZ starting, a dongle plugged in) is also asked which discovery
// filter keys it takes.
void BtManager::restore_adapters(const json& objs, const std::vector<BtAdapterId>& ids) {
  std::set<std::string> present;
  for (const BtAdapterId& a : ids) {
    present.insert(a.path);
    if (restored_.count(a.path)) continue;
    restored_.insert(a.path);

    std::vector<std::string> filters;
    sd_bus_message* reply = nullptr;
    sd_bus_error fe = SD_BUS_ERROR_NULL;
    if (sd_bus_call_method(bus_, kBluez, a.path.c_str(), kAdapterIface, "GetDiscoveryFilters", &fe, &reply, "") >= 0) {
      json v;
      if (read_json(reply, &v) > 0 && v.is_array())
        for (const json& x : v)
          if (x.is_string()) filters.push_back(x.get<std::string>());
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&fe);

    const std::string name = adapter_from_path(a.path);
    if (accept_central_ && !a.usb) {
      std::string e;
      if (accept_as_central(name, &e)) LOG_INFO("bluetooth: {} becomes central on the links it accepts", name);
      else LOG_WARN("bluetooth: {}: link mode central: {}", name, e);
    }
    DDict props;
    bool own_alias = false;
    {
      std::lock_guard<std::mutex> lk(m_);
      discovery_filters_[a.path] = std::move(filters);
      if (!kept_adapters_.contains(a.address) && kept_adapters_.contains(name)) {
        LOG_INFO("bluetooth: {}'s kept settings are {}'s from now on", name, a.address);
        kept_adapters_[a.address] = kept_adapters_[name];
        kept_adapters_.erase(name);
        save_state_locked();
      }
      if (kept_adapters_.contains(a.address)) props = bt_restore_adapter_props(kept_adapters_[a.address]);
      own_alias = kept_adapters_.contains(a.address) && kept_adapters_[a.address].contains("alias");
    }
    // No alias set here: the board's name, unless BlueZ already has it.
    const std::string host = default_alias_ && !own_alias ? default_alias(a) : std::string{};
    if (!host.empty() && str_of(objs[a.path][kAdapterIface], "Alias") != host) props.push_back({"Alias", DVar::str(host)});
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
  for (auto it = restored_.begin(); it != restored_.end();) {
    if (present.count(*it)) {
      ++it;
      continue;
    }
    std::lock_guard<std::mutex> lk(m_);
    discovery_filters_.erase(*it);
    scans_.erase(*it);
    it = restored_.erase(it);
  }
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
    std::vector<std::string> ours;
    {
      std::lock_guard<std::mutex> lk(m_);
      for (const auto& kv : scans_) ours.push_back(kv.first);
    }
    for (const std::string& a : ours) stop_scan(a);
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
  after_pair_.clear();
  if (!ready_adapter_.empty()) {
    ready_adapter_.clear();
    for (BusUser* u : users_) u->bluez_gone();
  }
  for (BusUser* u : users_) u->bus_closing();
  for (sd_bus_slot* s : match_slots_) sd_bus_slot_unref(s);
  match_slots_.clear();
  if (agent_slot_) agent_slot_ = sd_bus_slot_unref(agent_slot_);
  if (bus_) bus_ = sd_bus_flush_close_unref(bus_);
  refresh_in_flight_ = false;
  agent_registered_ = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    available_ = false;
    stale_ = false;
    agent_registered_shared_ = false;
    scans_.clear();
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
  std::vector<std::string> expired;  // adapters whose scan's time is up
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& kv : scans_)
      if (kv.second.until_ns && now >= kv.second.until_ns) expired.push_back(kv.first);
  }
  // A slow read (a Zero W while scanning) spaces the reads out: the chatter's to a quarter of
  // the time at most, a change's and the periodic one's in proportion.
  const uint64_t took = refresh_took_ns_;
  if (!refresh_in_flight_ && bus_ &&
      ((dirty_ && now - last_refresh_ns_ >= std::max(kRefreshDebounceNs, 2 * took)) ||
      (scan_dirty_ && now - last_refresh_ns_ >= std::max(kRefreshDebounceScanNs, 4 * took)) ||
      now - last_refresh_ns_ >= std::max(kRefreshPeriodNs, 8 * took))) {
    dirty_ = false;
    scan_dirty_ = false;
    last_refresh_ns_ = now;
    refresh();
  }
  expire_request(now);
  for (const std::string& a : expired) {
    LOG_INFO("bluetooth: {}: scan time is up", adapter_from_path(a));
    stop_scan(a);
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
      if (sd_bus_message_read(reply, "s", &owner) >= 0 && owner) {
        bluez_owner_ = owner;
        LOG_INFO("bluetooth: BlueZ is on the bus ({})", bluez_owner_);
      }
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&e);
  }

  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus_, &m, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
                                         "GetManagedObjects");
  if (r >= 0)
    r = sd_bus_call_async(bus_, nullptr, m, [](sd_bus_message* reply, void* self, sd_bus_error*) {
      static_cast<BtManager*>(self)->refreshed(reply);
      return 0;
    }, this, kRefreshTimeoutUs);
  sd_bus_message_unref(m);
  if (r >= 0) refresh_in_flight_ = true;
  else LOG_WARN("bluetooth: cannot read BlueZ's objects: {}", strerror(-r));
}

void BtManager::refreshed(sd_bus_message* reply) {
  refresh_in_flight_ = false;
  refresh_took_ns_ = mono_ns() - last_refresh_ns_;
  auto objs = std::make_shared<json>();
  sd_bus_error e = SD_BUS_ERROR_NULL;
  if (const sd_bus_error* re = sd_bus_message_get_error(reply)) sd_bus_error_copy(&e, re);
  const bool ok = !sd_bus_error_is_set(&e) && read_json(reply, objs.get()) > 0 && objs->is_object();
  // BlueZ can take its name between the GetNameOwner above and this call; then the answer's sender
  // is the owner. Left unknown, the NameOwnerChanged that follows looked like another BlueZ, and the
  // agent and every BusUser were set up a second time.
  if (ok && bluez_owner_.empty()) {
    const char* sender = sd_bus_message_get_sender(reply);
    if (sender && *sender) {
      bluez_owner_ = sender;
      LOG_INFO("bluetooth: BlueZ is on the bus ({})", bluez_owner_);
    }
  }
  if (!ok) {
    // Gone: the bus says nobody has the name, or BlueZ left it (NameOwnerChanged cleared the
    // owner) — then a read fails however it fails: the bus may try to start BlueZ for it, and a
    // start that fails answers with a systemd error or not at all.
    const bool gone = is_error(&e, SD_BUS_ERROR_SERVICE_UNKNOWN) || is_error(&e, SD_BUS_ERROR_NAME_HAS_NO_OWNER) ||
                      bluez_owner_.empty();
    const std::string why = gone ? std::string("BlueZ is not running (bluetooth.service)")
                                 : std::string("BlueZ: ") + (e.message ? e.message : "no answer");
    sd_bus_error_free(&e);
    if (why != logged_error_) {
      LOG_WARN("bluetooth: {}", why);
      logged_error_ = why;
    }
    // Slow, not gone (a timeout while bluetoothd is busy): what was read last stays, devices and
    // all, and the next read comes at the scan pace rather than at once.
    std::shared_ptr<const json> have;
    {
      std::lock_guard<std::mutex> lk(m_);
      have = objs_;
    }
    if (!gone && have && !have->empty()) {
      {
        std::lock_guard<std::mutex> lk(m_);
        stale_ = true;
      }
      scan_dirty_ = true;
      changed(false);
      return;
    }
    restored_.clear();
    if (!ready_adapter_.empty()) {
      ready_adapter_.clear();
      for (BusUser* u : users_) u->bluez_gone();
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      available_ = false;
      stale_ = false;
      error_ = why;
      objs_ = std::make_shared<const json>(json::object());
      ids_.clear();
      adapter_path_.clear();
      discovery_filters_.clear();
      scans_.clear();
    }
    changed(false);
    return;
  }
  sd_bus_error_free(&e);
  if (!agent_registered_ || agent_cap_registered_ != agent_capability_) register_agent();
  const std::vector<BtAdapterId> ids = adapter_ids(*objs);
  restore_adapters(*objs, ids);

  // Confined to one adapter (set_adapter), that one; else the primary chosen while it is there, or
  // the board's own controller.
  std::string primary;
  {
    std::lock_guard<std::mutex> lk(m_);
    primary = primary_;
  }
  std::string apath = bt_pick_adapter(ids, want_adapter_.empty() ? primary : want_adapter_);
  if (apath.empty() && want_adapter_.empty()) apath = bt_pick_adapter(ids, "");
  std::string err;
  if (apath.empty()) {
    err = want_adapter_.empty()
              ? "no Bluetooth adapter (is the controller attached? dmesg | grep -i blue)"
              : "no Bluetooth adapter " + want_adapter_;
  }
  if (err != logged_error_) {
    if (!err.empty()) LOG_WARN("bluetooth: {}", err);
    logged_error_ = err;
  }

  {
    std::lock_guard<std::mutex> lk(m_);
    objs_ = objs;
    ids_ = ids;
    adapter_path_ = apath;
    available_ = !apath.empty();
    error_ = err;
    stale_ = false;
    // A device that has gone (removed, or a temporary one expired) takes its busy/error with it.
    for (auto it = dev_state_.begin(); it != dev_state_.end();)
      it = objs->contains(it->first) ? std::next(it) : dev_state_.erase(it);
  }

  // The advertisements and the local GATT applications follow the primary: a change of primary
  // takes them off the old one and registers them with the new one.
  if (apath != ready_adapter_) {
    if (!ready_adapter_.empty())
      for (BusUser* u : users_) u->bluez_gone();
    ready_adapter_ = apath;
    if (!apath.empty()) {
      LOG_INFO("bluetooth: primary adapter {} of {}", adapter_from_path(apath), ids.size());
      for (BusUser* u : users_) u->bluez_ready(bus_, apath);
    }
  }

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

void BtManager::start_scan(const std::string& apath) {
  BtScanFilter f;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = scans_.find(apath);
    if (it == scans_.end()) return;
    f = it->second.filter;
  }
  if (!bus_ || apath.empty()) return;
  const std::string name = adapter_from_path(apath);
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
    const std::string why = name + ": discovery filter refused: " + friendly(&e);
    LOG_WARN("bluetooth: {}", why);
    sd_bus_error_free(&e);
    std::lock_guard<std::mutex> lk(m_);
    error_ = why;
    scans_.erase(apath);
    dirty_ = true;
    return;
  }
  sd_bus_error_free(&e);

  r = sd_bus_call_method(bus_, kBluez, apath.c_str(), kAdapterIface, "StartDiscovery", &e, nullptr, "");
  if (r < 0 && !is_error(&e, "org.bluez.Error.InProgress")) {
    const std::string why = name + ": cannot scan: " + friendly(&e);
    LOG_WARN("bluetooth: {}", why);
    std::lock_guard<std::mutex> lk(m_);
    error_ = why;
    scans_.erase(apath);
  } else {
    LOG_INFO("bluetooth: {} scanning ({})", name, f.transport);
  }
  sd_bus_error_free(&e);
  dirty_ = true;
}

void BtManager::stop_scan(const std::string& apath) {
  {
    std::lock_guard<std::mutex> lk(m_);
    scans_.erase(apath);
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

void BtManager::set_device_state(const std::string& path, std::string busy, std::string error,
                                 std::string busy_on) {
  {
    std::lock_guard<std::mutex> lk(m_);
    DevState& s = dev_state_[path];
    // A call's answer (nothing busy any more) keeps what the call was about with its error.
    s.error_on = busy.empty() ? s.busy_on : std::string{};
    s.busy = std::move(busy);
    s.busy_on = std::move(busy_on);
    s.error = std::move(error);
  }
  changed(false);
}

void BtManager::call_device(const std::string& path, const char* method, const std::string& uuid,
                            const char* busy, unsigned timeout_s, const char* iface,
                            const std::string& busy_on) {
  if (!bus_) return;
  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus_, &m, kBluez, path.c_str(), iface ? iface : kDeviceIface, method);
  if (r >= 0 && !uuid.empty()) r = sd_bus_message_append(m, "s", uuid.c_str());
  auto* op = new BtBus::AsyncOp{this, path, method, iface ? iface : ""};
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
  set_device_state(path, busy, "", busy_on);
}

void BtManager::on_reply(const std::string& path, const std::string& method, const std::string& iface,
                         sd_bus_message* reply) {
  const sd_bus_error* e = sd_bus_message_get_error(reply);
  // The BR/EDR bearer's Connect brings up the link only, and answers AlreadyConnected when it is
  // up — as it still is right after a pairing — without connecting a profile: A2DP and AVRCP
  // never came, and the idle link dropped seconds later. Device1's Connect connects the profiles
  // over the link that is there.
  if (e && method == "Connect" && iface == kBearerBredrIface && is_error(e, "org.bluez.Error.AlreadyConnected")) {
    LOG_INFO("bluetooth: {} link is up: connecting its profiles", address_from_path(path));
    call_device(path, "Connect", "", "connecting", kConnectTimeoutS);
    return;
  }
  // Nothing to disconnect is done, not failed: it is also how a bearer's Disconnect answers when
  // it only cancelled a Connect still in progress.
  if (e && method == "Disconnect" && is_error(e, "org.bluez.Error.NotConnected")) e = nullptr;
  const std::string addr = address_from_path(path);
  if (method == "Pair") {
    started_here_.erase(path);
    if (pending_ && pending_->dev_path == path) clear_request(false);
    // Paired, the link is up with only SDP having run on it, and the kernel drops an ACL without
    // channels seconds later. A phone connects the profiles at this point: so does the bench.
    if (!e) {
      LOG_INFO("bluetooth: Pair {} done: connecting its profiles", addr);
      after_pair_.insert(path);
      connect_profiles(path);
      dirty_ = true;
      return;
    }
  }
  // A device with nothing to connect (no profile the bench has, or already connected) is fine
  // right after its pairing: the Connect was the bench's idea, not a request that failed.
  if (method == "Connect" && after_pair_.erase(path) && e &&
      (is_error(e, "org.bluez.Error.AlreadyConnected") || is_error(e, "org.bluez.Error.NotAvailable") ||
       is_error(e, "org.bluez.Error.ProfileUnavailable"))) {
    LOG_INFO("bluetooth: {} paired, no profile to connect ({})", addr, friendly(e));
    e = nullptr;
  }
  if (e) LOG_WARN("bluetooth: {} {} failed: {} ({})", method, addr, friendly(e), e->name);
  else LOG_INFO("bluetooth: {} {} done", method, addr);
  set_device_state(path, "", e ? method + ": " + friendly(e) : "");
  dirty_ = true;
}

// Bus thread. A device's profiles: over BR/EDR's bearer only while that link is down: with it up
// (after a pairing), Device1's Connect is what connects the profiles (on_reply).
void BtManager::connect_profiles(const std::string& path) {
  bool bredr;
  {
    std::lock_guard<std::mutex> lk(m_);
    bredr = objs_->contains(path) && bt_connect_over_bredr((*objs_)[path]) &&
            !bool_of((*objs_)[path][kBearerBredrIface], "Connected");
  }
  call_device(path, "Connect", "", "connecting", kConnectTimeoutS, bredr ? kBearerBredrIface : nullptr);
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
  p->req.adapter = adapter_from_path(dev_path);
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

std::string BtManager::find_adapter_locked(const std::string& want) const {
  if (want.empty()) return adapter_path_;
  const std::string p = bt_pick_adapter(ids_, want);
  // Confined to one adapter (a PC): the desktop's others are not the bench's to use.
  if (!want_adapter_.empty() && p != adapter_path_) return {};
  return p;
}

std::string BtManager::find_adapter(const std::string& want) const {
  std::lock_guard<std::mutex> lk(m_);
  return find_adapter_locked(want);
}

json BtManager::state() const {
  std::shared_ptr<const json> objs;
  std::string apath, error;
  bool available, has_req, agent_reg, stale;
  BtRequest req;
  uint64_t deadline;
  std::map<std::string, DevState> ds;
  std::string policy, cap;
  std::map<std::string, Scan> scans;
  std::map<std::string, std::vector<std::string>> filters;
  std::vector<BtAdapterId> ids;
  {
    std::lock_guard<std::mutex> lk(m_);
    objs = objs_;
    apath = adapter_path_;
    available = available_;
    error = error_;
    stale = stale_;
    has_req = has_request_;
    req = request_;
    deadline = request_deadline_ns_;
    ds = dev_state_;
    policy = agent_policy_;
    cap = agent_capability_;
    scans = scans_;
    filters = discovery_filters_;
    ids = ids_;
    agent_reg = agent_registered_shared_;
    if (!running_.load()) {
      available = false;
      error = not_running_reason_.empty() ? "Bluetooth is not running" : not_running_reason_;
    }
  }
  const uint64_t now = mono_ns();
  json adapters = model_adapters(*objs);
  json adapter = nullptr;
  json discovery = nullptr;
  for (json& a : adapters) {
    const std::string p = a["path"].get<std::string>();
    const auto id = std::find_if(ids.begin(), ids.end(), [&](const BtAdapterId& x) { return x.path == p; });
    const auto sc = scans.find(p);
    const auto fl = filters.find(p);
    a["primary"] = p == apath;
    a["usb"] = id != ids.end() && id->usb;
    a["discovery"] = json{
        {"on", a.value("discovering", false)},
        {"ours", sc != scans.end()},
        {"filter", bt_scan_filter_json(sc != scans.end() ? sc->second.filter : BtScanFilter{})},
        // The keys the adapter's BlueZ takes (GetDiscoveryFilters): UUIDs, RSSI, Pathloss, ...
        {"filters", fl != filters.end() ? json(fl->second) : json::array()},
        {"until_s", sc != scans.end() && sc->second.until_ns > now ? json((sc->second.until_ns - now) / 1000000000ull)
                                                                   : json(nullptr)}};
    if (p == apath) {
      adapter = a;
      discovery = a["discovery"];
    }
  }
  if (discovery.is_null())
    discovery = json{{"on", false}, {"ours", false}, {"filter", bt_scan_filter_json(BtScanFilter{})},
                     {"filters", json::array()}, {"until_s", nullptr}};
  json devices = model_devices(*objs, want_adapter_.empty() ? std::string{} : apath);
  for (json& d : devices) {
    const auto it = ds.find(d["path"].get<std::string>());
    d["busy"] = it == ds.end() ? "" : it->second.busy;
    d["busy_on"] = it == ds.end() ? "" : it->second.busy_on;
    d["error"] = it == ds.end() ? "" : it->second.error;
    d["error_on"] = it == ds.end() ? "" : it->second.error_on;
    d["last_disconnect"] = it == ds.end() || it->second.last_disconnect.is_null() ? json(nullptr)
                                                                                 : it->second.last_disconnect;
  }
  if (has_req) req.expires_s = deadline > now ? static_cast<unsigned>((deadline - now) / 1000000000ull) : 0;
  return json{{"available", available},
              {"error", error},
              // BlueZ did not answer the last read in time: the devices are as it last said.
              {"stale", stale},
              {"adapter", adapter},
              {"adapters", adapters},
              {"devices", devices},
              {"request", has_req ? bt_request_json(req) : json(nullptr)},
              {"agent", {{"policy", policy}, {"capability", cap}, {"registered", agent_reg}}},
              {"discovery", discovery}};
}

std::string BtManager::device_path(const std::string& address, const std::string& adapter) const {
  if (!bt_address_ok(address)) return {};
  std::shared_ptr<const json> objs;
  std::string apath;
  std::vector<BtAdapterId> ids;
  {
    std::lock_guard<std::mutex> lk(m_);
    objs = objs_;
    apath = find_adapter_locked(adapter);
    ids = ids_;
  }
  const auto has = [&](const std::string& p) { return objs->contains(p) && (*objs)[p].contains(kDeviceIface); };
  // An adapter named: its object of the device, or none.
  if (!adapter.empty()) {
    const std::string p = apath.empty() ? std::string{} : bt_device_path(apath, address);
    return !p.empty() && has(p) ? p : std::string{};
  }
  // The primary adapter's first; then any adapter's, unless the bench is confined to one.
  if (!apath.empty()) {
    const std::string p = bt_device_path(apath, address);
    if (has(p)) return p;
  }
  if (!want_adapter_.empty()) return {};
  for (const BtAdapterId& a : ids) {
    const std::string p = bt_device_path(a.path, address);
    if (has(p)) return p;
  }
  return {};
}

json BtManager::device(const std::string& address, const std::string& adapter) const {
  const std::string path = device_path(address, adapter);
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
  d["busy_on"] = st.busy_on;
  d["error"] = st.error;
  d["error_on"] = st.error_on;
  d["last_disconnect"] = st.last_disconnect.is_null() ? json(nullptr) : st.last_disconnect;
  // How much of it there is beyond the device itself: a resolved GATT database, A2DP endpoints.
  size_t services = 0, endpoints = 0, transports = 0;
  const std::string prefix = path + "/";
  std::vector<const json*> children;
  for (auto it = objs->begin(); it != objs->end(); ++it) {
    if (!starts_with(it.key(), prefix)) continue;
    children.push_back(&*it);
    if (it->contains("org.bluez.GattService1")) ++services;
    if (it->contains("org.bluez.MediaEndpoint1")) ++endpoints;
    if (it->contains("org.bluez.MediaTransport1")) ++transports;
  }
  d["gatt_services"] = services;
  d["media_endpoints"] = endpoints;
  d["media_transports"] = transports;
  d["profiles"] = d["connected"].get<bool>() ? model_profiles((*objs)[path], children) : json::array();
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
  BtAdapterId id;
  {
    std::lock_guard<std::mutex> lk(m_);
    path = find_adapter_locked(adapter);
    if (path.empty() || !objs_->contains(path)) {
      *err = adapter.empty() ? "no adapter" : "no such adapter";
      return false;
    }
    for (const BtAdapterId& a : ids_)
      if (a.path == path) id = a;
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
      // Empty: back to the default — the board's name, or BlueZ's (its Name) on a PC.
      const std::string host = a.empty() && default_alias_ ? default_alias(id) : std::string{};
      props.push_back({"Alias", DVar::str(host.empty() ? a : host)});
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
    const std::string name = id.address.empty() ? path.substr(path.rfind('/') + 1) : id.address;
    uint32_t timeout = 0;
    if (j.contains("discoverable_timeout_s")) timeout = j["discoverable_timeout_s"].get<uint32_t>();
    else if (objs_->contains(path) && (*objs_)[path].contains(kAdapterIface))
      timeout = static_cast<uint32_t>(num_of((*objs_)[path][kAdapterIface], "DiscoverableTimeout"));
    kept_adapters_[name] = bt_keep_adapter_settings(kept_adapters_.value(name, json::object()), j, timeout);
    // An alias cleared is not kept: the default applies again at the next start.
    if (j.contains("alias") && j["alias"].get<std::string>().empty()) kept_adapters_[name].erase("alias");
    save_state_locked();
  }
  touch();
  return true;
}

bool BtManager::set_primary(const std::string& want, std::string* err) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  std::string address;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!want_adapter_.empty()) {
      *err = "the bench runs on " + want_adapter_ + " only (--bt-adapter)";
      return false;
    }
    if (!want.empty()) {
      const std::string p = bt_pick_adapter(ids_, want);
      for (const BtAdapterId& a : ids_)
        if (a.path == p) address = a.address;
      if (address.empty()) {
        *err = "no such adapter";
        return false;
      }
    }
    primary_ = address;
    save_state_locked();
    // Shown at once; the next read moves the advertisements and the GATT applications over.
    const std::string p = bt_pick_adapter(ids_, address);
    adapter_path_ = p.empty() ? bt_pick_adapter(ids_, "") : p;
  }
  LOG_INFO("bluetooth: primary adapter: {}", address.empty() ? std::string("the board's own") : address);
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

bool BtManager::scan(bool on, const std::string& adapter, const BtScanFilter& f, unsigned seconds,
                     std::string* err) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  std::vector<std::string> paths;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (on || !adapter.empty()) {
      const std::string p = find_adapter_locked(adapter);
      if (p.empty()) {
        *err = adapter.empty() ? "no adapter" : "no such adapter";
        return false;
      }
      paths.push_back(p);
    } else {
      // Stop with no adapter named: every scan started here, or the primary's (one started elsewhere).
      for (const auto& kv : scans_) paths.push_back(kv.first);
      if (paths.empty() && !adapter_path_.empty()) paths.push_back(adapter_path_);
    }
    if (on) scans_[paths[0]] = Scan{f, seconds ? mono_ns() + s_to_ns(seconds) : 0};
  }
  post([this, on, paths](sd_bus*) {
    for (const std::string& p : paths) {
      if (on) start_scan(p);
      else stop_scan(p);
    }
  });
  return true;
}

bool BtManager::device_action(const std::string& address, Action action, const std::string& arg,
                              std::string* err, const std::string& adapter) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  const std::string path = device_path(address, adapter);
  if (path.empty()) {
    *err = "no such device";
    return false;
  }
  {
    // Shown at once, so a console that reads the state straight back sees its request running.
    std::lock_guard<std::mutex> lk(m_);
    DevState& s = dev_state_[path];
    s.error.clear();
    s.error_on.clear();
    if (action == Action::Pair) s.busy = "pairing";
    if (action == Action::Connect) s.busy = "connecting";
    if (action == Action::Disconnect || action == Action::DisconnectBearer) s.busy = "disconnecting";
    if (action == Action::ConnectBearer) s.busy = "connecting";
    s.busy_on = action == Action::ConnectBearer || action == Action::DisconnectBearer ? arg
                : action == Action::Pair || action == Action::Remove ? std::string{} : uuid_full(arg);
  }
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
      case Action::Connect:
        if (uuid.empty()) connect_profiles(path);
        else call_device(path, "ConnectProfile", uuid, "connecting", kConnectTimeoutS, nullptr, uuid);
        break;
      case Action::Disconnect:
        call_device(path, uuid.empty() ? "Disconnect" : "DisconnectProfile", uuid, "disconnecting",
                    kConnectTimeoutS, nullptr, uuid);
        break;
      case Action::ConnectBearer:
      case Action::DisconnectBearer:
        // A device seen on one transport only has no bearer object for the other: BlueZ answers
        // UnknownObject, shown on the device like any refusal.
        call_device(path, action == Action::ConnectBearer ? "Connect" : "Disconnect", "",
                    action == Action::ConnectBearer ? "connecting" : "disconnecting", kConnectTimeoutS,
                    arg == "le" ? kBearerLeIface : kBearerBredrIface, arg);
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

bool BtManager::set_device_props(const std::string& address, const json& j, std::string* err,
                                 const std::string& adapter) {
  if (!running_.load()) {
    *err = "Bluetooth is not running";
    return false;
  }
  const std::string path = device_path(address, adapter);
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
