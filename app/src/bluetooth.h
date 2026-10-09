#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "dbus_util.h"

typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_slot sd_bus_slot;

namespace btb {

// ---- The agent's decisions, exposed so they can be tested ----------------------------------------

// What a device asked the agent.
enum class BtAsk {
  Confirm,    // numeric comparison: both sides show a six-digit code
  Authorize,  // a just-works pairing the other side started
  Pin,        // a legacy device wants a PIN
  Passkey,    // type the code the other side shows
  Service,    // a device wants to use a profile
};
enum class BtVerdict { Accept, Ask };

// "auto": say yes to everything, the way a speaker with no screen does — every pairing and every
// profile — except a passkey to type, which cannot be guessed. "ask": every question goes to the
// console (or `bench agent`), for testing what a pairing actually asks.
BtVerdict bt_agent_policy(const std::string& policy, BtAsk ask);
// The IO capabilities BlueZ accepts for an agent; the pairing method follows from it.
bool bt_agent_capability_ok(const std::string& cap);

// A discovery filter as the API takes it, checked: transport auto|le|bredr, an RSSI floor
// (-127..20), duplicate data, UUIDs (any form uuid_full() takes), a name pattern.
struct BtScanFilter {
  std::string transport = "auto";
  bool has_rssi = false;
  int rssi = 0;
  bool duplicate_data = true;
  std::vector<std::string> uuids;
  std::string pattern;
};
bool bt_scan_filter_from_json(const nlohmann::json& j, BtScanFilter* f, std::string* err);
nlohmann::json bt_scan_filter_json(const BtScanFilter& f);

// A pairing question waiting for the operator.
struct BtRequest {
  uint64_t id = 0;
  std::string kind;  // confirm | authorize | service | pin | passkey | display
  std::string address;
  std::string name;
  std::string passkey;  // with confirm and display, zero-padded to six digits
  std::string uuid;     // with service: the profile asked for
  unsigned expires_s = 0;
};
nlohmann::json bt_request_json(const BtRequest& r);

// A D-Bus call made on the bus thread for a web handler, which waits for the reply: a GATT read
// over the air, a profile connect. `reply` is the reply's arguments as a JSON array.
struct BtCallResult {
  bool ok = false;
  bool timed_out = false;
  std::string error;       // friendly()
  std::string error_name;  // org.bluez.Error.*
  nlohmann::json reply = nlohmann::json::array();
};

// Something else that lives on the bus: the advertisements and the local GATT application export
// objects BlueZ calls back, and both must be registered again whenever BlueZ comes back. All of
// these run on the bus thread.
class BusUser {
 public:
  virtual ~BusUser() = default;
  // The connection is up (again): export objects on it.
  virtual void bus_opened(sd_bus*) {}
  // It is about to go: drop every slot on it.
  virtual void bus_closing() {}
  // BlueZ answers and has the adapter: (re)register with it. Called again after BlueZ restarts
  // or the adapter changes.
  virtual void bluez_ready(sd_bus*, const std::string& /*adapter_path*/) {}
  // BlueZ left the bus: registrations are void.
  virtual void bluez_gone() {}
  // Every pass of the bus loop, ~10 Hz.
  virtual void tick(sd_bus*, uint64_t /*now_ns*/) {}
};

// Runs Bluetooth over BlueZ's D-Bus API: adapters, discovery, devices and their operations, and the
// pairing agent, so that every question a pairing asks reaches the web console (or `bench agent`)
// instead of a terminal nobody has open.
//
// One thread owns the bus connection: sd-bus is not thread-safe, and an agent that answers late
// costs a pairing. Web handlers queue work and return; a long operation (pairing can take a minute
// while someone finds their phone) reports its outcome on the device, not to the caller. The object
// tree is re-read with GetManagedObjects when BlueZ signals a change (debounced) and kept as one
// immutable snapshot every reader shares: the GATT tree, the media endpoints and the device list
// are all computed from it (bluez_model.h). Nothing here is fatal: no bus, no BlueZ or no adapter
// is a status line, retried.
class BtManager {
 public:
  BtManager();
  ~BtManager();
  BtManager(const BtManager&) = delete;
  BtManager& operator=(const BtManager&) = delete;

  // Which adapter is "the" adapter (bt_pick_adapter's `want`); with one set, only its devices are
  // listed and the agent answers only for it — on a PC the others are the desktop's. Before start().
  void set_adapter(std::string want);
  void add_user(BusUser* u);  // before start()
  // Value changes of GATT characteristics (notifications, and reads), on the bus thread.
  using NotifyFn = std::function<void(const std::string& path, const std::vector<uint8_t>& value)>;
  void on_notify(NotifyFn fn);  // before start()
  // Called (on the bus thread, at most every `min_interval`) when the state or the pending request
  // changed: the publisher of the "bt" topic.
  void on_change(std::function<void(bool request_changed)> fn);  // before start()

  bool start();
  void stop();
  void set_not_running_reason(std::string reason);
  bool running() const { return running_.load(); }

  // The object tree as last read; never null (an empty object before the first read).
  std::shared_ptr<const nlohmann::json> objects() const;
  // The primary adapter's path ("" when there is none), and every adapter's.
  std::string adapter_path() const;

  // GET /api/bluetooth: {available, error, adapter (the primary), adapters, devices, request,
  // agent:{policy, capability, registered}, discovery:{on, filter, until_s}}.
  nlohmann::json state() const;
  // One device in full, with busy/error; null when unknown.
  nlohmann::json device(const std::string& address) const;
  // The device's object path ("" when unknown).
  std::string device_path(const std::string& address) const;
  bool has_request(BtRequest* out) const;

  // ---- Operations. All return at once; the bus thread carries them out. -----------------------

  // Adapter properties: any of powered, alias, discoverable, discoverable_timeout_s, pairable.
  // `adapter` "" is the primary. False with a reason on a bad body or no such adapter.
  bool set_adapter_props(const std::string& adapter, const nlohmann::json& j, std::string* err);
  // Agent settings: "auto"|"ask", and the IO capability (re-registers the agent).
  bool set_agent(const std::string& policy, const std::string& capability, std::string* err);
  // Discovery with a filter for `seconds` (0: until stopped).
  bool scan(bool on, const BtScanFilter& f, unsigned seconds, std::string* err);

  enum class Action { Pair, CancelPairing, Connect, Disconnect, Remove };
  // `uuid` with Connect/Disconnect: ConnectProfile/DisconnectProfile. `pin` with Pair: what to
  // answer a RequestPinCode with (legacy devices). False when there is no such device (a 404) or
  // the manager is not running.
  bool device_action(const std::string& address, Action action, const std::string& arg,
                     std::string* err);
  // Device properties: any of alias, trusted, blocked, wake_allowed.
  bool set_device_props(const std::string& address, const nlohmann::json& j, std::string* err);
  // Answers the request `id`. `value` is the PIN or the passkey for those kinds.
  bool answer(uint64_t id, bool accept, const std::string& value, std::string* err);

  // ---- For the other modules ------------------------------------------------------------------

  // Queues `fn` for the bus thread; it gets the bus, or nullptr while there is none.
  void post(std::function<void(sd_bus*)> fn);
  // A method call on org.bluez made from the bus thread, waited for here. `append` adds the
  // arguments (it runs on the bus thread: capture by value).
  BtCallResult call(const std::string& path, const std::string& iface, const std::string& method,
                    std::function<int(sd_bus_message*)> append, int timeout_ms);
  // Sets one property on an org.bluez object, waited for.
  BtCallResult set_property(const std::string& path, const std::string& iface,
                            const std::string& prop, const DVar& value, int timeout_ms);
  // Ask for a re-read soon (after a write that changes the tree).
  void touch();

 private:
  struct Pending;  // an agent request held open while the operator decides
  struct DevState {
    std::string busy;
    std::string error;
  };

  void run();
  bool open_bus();
  void close_bus();
  void drain_commands();
  void tick();
  void refresh();
  void register_agent();
  void unregister_agent();
  void start_scan();
  void stop_scan();
  void set_error(std::string e);
  void changed(bool request);

  void call_device(const std::string& path, const char* method, const std::string& uuid,
                   const char* busy, unsigned timeout_s);
  void on_reply(const std::string& path, const std::string& method, sd_bus_message* reply);
  void set_device_state(const std::string& path, std::string busy, std::string error);

  int agent_call(sd_bus_message* m, const char* member);
  void hold_request(sd_bus_message* m, BtAsk ask, const std::string& kind,
                    const std::string& dev_path, const std::string& passkey,
                    const std::string& uuid);
  void clear_request(bool reply_rejected);
  void expire_request(uint64_t now_ns);
  void on_signal(sd_bus_message* m);
  bool in_scope(const std::string& path) const;

  friend struct BtBus;

  // Shared with web handlers, under m_.
  mutable std::mutex m_;
  std::shared_ptr<const nlohmann::json> objs_;
  std::string adapter_path_;
  bool available_ = false;
  std::string error_;
  std::string not_running_reason_;
  std::map<std::string, DevState> dev_state_;  // by device path
  bool has_request_ = false;
  BtRequest request_;
  uint64_t request_deadline_ns_ = 0;
  std::string agent_policy_ = "auto";
  std::string agent_capability_ = "KeyboardDisplay";
  bool agent_registered_shared_ = false;
  bool scanning_ = false;
  BtScanFilter filter_;
  uint64_t scan_until_ns_ = 0;
  std::deque<std::function<void(sd_bus*)>> commands_;

  std::mutex life_m_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<int> wake_fd_{-1};

  // Set before start(), read on the bus thread.
  std::string want_adapter_;
  std::vector<BusUser*> users_;
  NotifyFn notify_;
  std::function<void(bool)> change_fn_;

  // Bus thread only.
  sd_bus* bus_ = nullptr;
  sd_bus_slot* agent_slot_ = nullptr;
  std::vector<sd_bus_slot*> match_slots_;
  std::string bluez_owner_;  // BlueZ's unique name: the only caller the agent answers
  bool agent_registered_ = false;
  std::string agent_cap_registered_;
  bool dirty_ = true;       // something changed: re-read soon
  bool scan_dirty_ = false;  // only advertising chatter (RSSI, adv data): re-read at the slower pace
  uint64_t last_refresh_ns_ = 0;
  std::string ready_adapter_;  // the adapter BusUsers were last told is ready
  std::map<std::string, std::string> started_here_;  // device path -> the PIN to offer
  Pending* pending_ = nullptr;
  uint64_t next_request_id_ = 1;
  bool change_pending_ = false;
  bool request_change_pending_ = false;
  uint64_t last_change_ns_ = 0;
  std::string logged_error_;
};

uint64_t mono_ns();

}  // namespace btb
