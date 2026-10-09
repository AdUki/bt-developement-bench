#include "hid_device.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "util/log.h"
#include "util/strings.h"
#include "uuids.h"

using json = nlohmann::json;

namespace btb {

namespace {

// Where each characteristic lands in hid_app_def(): the service is svc0, its characteristics
// chr0.. in definition order. The two Reports are the ones keys are written to.
constexpr const char* kConsumerReport = "/svc0/chr4";
constexpr const char* kKeyboardReport = "/svc0/chr5";
constexpr const char* kBatteryLevel = "/svc1/chr0";

constexpr uint16_t kHidServiceHandle = 0x0400;
constexpr uint8_t kShift = 0x02;  // left shift in the keyboard report's modifier byte
constexpr uint64_t kHoldLimitNs = 10ull * 1000000000ull;

struct Named {
  const char* name;
  uint16_t usage;
};

// Consumer page (0x0C) usages a media remote has, by the names the API takes.
const Named kConsumer[] = {
    {"play_pause", 0xCD}, {"play", 0xB0},         {"pause", 0xB1},       {"stop", 0xB7},
    {"next", 0xB5},       {"previous", 0xB6},     {"fast_forward", 0xB3}, {"rewind", 0xB4},
    {"volume_up", 0xE9},  {"volume_down", 0xEA},  {"mute", 0xE2},        {"power", 0x30},
    {"menu", 0x40},       {"home", 0x223},        {"back", 0x224},       {"search", 0x221},
    {"channel_up", 0x9C}, {"channel_down", 0x9D}, {"record", 0xB2},      {"eject", 0xB8},
};

// Keyboard page (0x07) keys a remote's d-pad and a little typing need.
const Named kKeyboard[] = {
    {"up", 0x52},    {"down", 0x51},      {"left", 0x50}, {"right", 0x4F}, {"enter", 0x28},
    {"escape", 0x29}, {"backspace", 0x2A}, {"tab", 0x2B},  {"space", 0x2C}, {"delete", 0x4C},
    {"page_up", 0x4B}, {"page_down", 0x4E}, {"f1", 0x3A},  {"f5", 0x3E},    {"f11", 0x44},
};

bool parse_raw(const std::string& name, HidKey* out) {
  const auto colon = name.find(':');
  if (colon == std::string::npos) return false;
  const std::string page = name.substr(0, colon);
  const std::string num = name.substr(colon + 1);
  if (num.empty()) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(num.c_str(), &end, 0);
  if (*end != '\0') return false;
  if (page == "consumer" && v <= 0x3FF) {
    *out = HidKey{HidReport::consumer, static_cast<uint16_t>(v), 0};
    return true;
  }
  if (page == "keyboard" && v <= 0xFF) {
    *out = HidKey{HidReport::keyboard, static_cast<uint16_t>(v), 0};
    return true;
  }
  return false;
}

}  // namespace

std::vector<uint8_t> hid_report_map() {
  return {
      // Consumer Control, report 1: one usage of the consumer page, 16 bits, as an array (the
      // value is the usage pressed, 0 for none).
      0x05, 0x0C,        // Usage Page (Consumer)
      0x09, 0x01,        // Usage (Consumer Control)
      0xA1, 0x01,        // Collection (Application)
      0x85, 0x01,        //   Report ID (1)
      0x15, 0x00,        //   Logical Minimum (0)
      0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
      0x19, 0x00,        //   Usage Minimum (0)
      0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
      0x75, 0x10,        //   Report Size (16)
      0x95, 0x01,        //   Report Count (1)
      0x81, 0x00,        //   Input (Data, Array, Absolute)
      0xC0,              // End Collection
      // Keyboard, report 2: the boot layout without the LED output report (a remote has none).
      0x05, 0x01,        // Usage Page (Generic Desktop)
      0x09, 0x06,        // Usage (Keyboard)
      0xA1, 0x01,        // Collection (Application)
      0x85, 0x02,        //   Report ID (2)
      0x05, 0x07,        //   Usage Page (Keyboard)
      0x19, 0xE0,        //   Usage Minimum (Left Control)
      0x29, 0xE7,        //   Usage Maximum (Right GUI)
      0x15, 0x00,        //   Logical Minimum (0)
      0x25, 0x01,        //   Logical Maximum (1)
      0x75, 0x01,        //   Report Size (1)
      0x95, 0x08,        //   Report Count (8)
      0x81, 0x02,        //   Input (Data, Variable, Absolute): the modifiers
      0x95, 0x01,        //   Report Count (1)
      0x75, 0x08,        //   Report Size (8)
      0x81, 0x01,        //   Input (Constant): reserved
      0x95, 0x06,        //   Report Count (6)
      0x75, 0x08,        //   Report Size (8)
      0x15, 0x00,        //   Logical Minimum (0)
      0x26, 0xFF, 0x00,  //   Logical Maximum (255)
      0x05, 0x07,        //   Usage Page (Keyboard)
      0x19, 0x00,        //   Usage Minimum (0)
      0x2A, 0xFF, 0x00,  //   Usage Maximum (255)
      0x81, 0x00,        //   Input (Data, Array, Absolute): the keys
      0xC0,              // End Collection
  };
}

GattAppDef hid_app_def() {
  // HID wants an encrypted link for everything a host reads (HOGP 1.0 §4.4): asking for it makes
  // the host pair before it gets the report map, which is what every HID host expects to do.
  const std::vector<std::string> enc_read = {"read", "encrypt-read"};
  const std::vector<std::string> report = {"read", "encrypt-read", "notify"};
  // Fixed handles (gatt_server.h): a HID host caches the report map and the report handles at
  // bonding, so they must be where they were after every restart. Each service starts on its own
  // 0x10 boundary well above bluetoothd's own services; a characteristic's handle is its value
  // (its declaration the one before), a notifying one's CCC the one after.
  auto chr = [](const char* uuid, std::vector<std::string> flags, std::vector<uint8_t> value, uint16_t handle) {
    GattCharDef c;
    c.uuid = uuid_full(uuid);
    c.handle = handle;
    c.flags = std::move(flags);
    c.value = std::move(value);
    return c;
  };
  auto report_ref = [](uint8_t id, uint16_t handle) {
    GattDescDef d;
    d.handle = handle;
    d.uuid = uuid_full("2908");  // Report Reference: report ID, type 1 = input
    d.flags = {"read", "encrypt-read"};
    d.value = {id, 0x01};
    return d;
  };
  GattServiceDef hid;
  hid.uuid = uuid_full("1812");
  hid.handle = kHidServiceHandle;
  // bcdHID 1.11, country 0, flags: RemoteWake | NormallyConnectable.
  hid.characteristics.push_back(chr("2a4a", enc_read, {0x11, 0x01, 0x00, 0x03}, 0x0402));
  hid.characteristics.push_back(chr("2a4b", enc_read, hid_report_map(), 0x0404));
  hid.characteristics.push_back(chr("2a4c", {"write-without-response"}, {0x00}, 0x0406));  // Control Point
  hid.characteristics.push_back(chr("2a4e", {"read", "encrypt-read", "write-without-response"}, {0x01}, 0x0408));  // Report mode
  GattCharDef consumer = chr("2a4d", report, {0x00, 0x00}, 0x040a);  // CCC 0x040b
  consumer.descriptors.push_back(report_ref(1, 0x040c));
  hid.characteristics.push_back(std::move(consumer));
  GattCharDef keyboard = chr("2a4d", report, std::vector<uint8_t>(8, 0), 0x040e);  // CCC 0x040f
  keyboard.descriptors.push_back(report_ref(2, 0x0410));
  hid.characteristics.push_back(std::move(keyboard));

  GattServiceDef battery;
  battery.uuid = uuid_full("180f");
  battery.handle = 0x0420;
  battery.characteristics.push_back(chr("2a19", {"read", "notify"}, {100}, 0x0422));

  // PnP ID: vendor ID source 2 (USB-IF), the Linux Foundation's vendor ID 0x1d6b, product 0x0246,
  // version 1.0. Windows and Android read it before they load a HID driver.
  GattServiceDef dis;
  dis.uuid = uuid_full("180a");
  dis.handle = 0x0430;
  dis.characteristics.push_back(chr("2a50", {"read"}, {0x02, 0x6b, 0x1d, 0x46, 0x02, 0x00, 0x01}, 0x0432));
  const std::string maker = "btbench";
  dis.characteristics.push_back(chr("2a29", {"read"}, std::vector<uint8_t>(maker.begin(), maker.end()), 0x0434));

  GattAppDef d;
  d.services = {std::move(hid), std::move(battery), std::move(dis)};
  return d;
}

bool hid_key(const std::string& name, HidKey* out) {
  for (const Named& n : kConsumer)
    if (name == n.name) {
      *out = HidKey{HidReport::consumer, n.usage, 0};
      return true;
    }
  for (const Named& n : kKeyboard)
    if (name == n.name) {
      *out = HidKey{HidReport::keyboard, n.usage, 0};
      return true;
    }
  if (name.size() == 1) return hid_char_key(name[0], out);
  return parse_raw(name, out);
}

bool hid_char_key(char c, HidKey* out) {
  HidKey k{HidReport::keyboard, 0, 0};
  if (c >= 'a' && c <= 'z') {
    k.usage = static_cast<uint16_t>(0x04 + (c - 'a'));
  } else if (c >= 'A' && c <= 'Z') {
    k.usage = static_cast<uint16_t>(0x04 + (c - 'A'));
    k.modifiers = kShift;
  } else if (c >= '1' && c <= '9') {
    k.usage = static_cast<uint16_t>(0x1E + (c - '1'));
  } else if (c == '0') {
    k.usage = 0x27;
  } else {
    // The US layout's punctuation: unshifted, then the shifted ones on the same keys.
    static const char* plain = "\n\t -=[]\\;'`,./";
    static const uint16_t plain_u[] = {0x28, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    static const char* shifted = "!@#$%^&*()_+{}|:\"~<>?";
    static const uint16_t shifted_u[] = {0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D,
                                         0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    if (const char* p = std::strchr(plain, c); p && c) {
      k.usage = plain_u[p - plain];
    } else if (const char* q = std::strchr(shifted, c); q && c) {
      k.usage = shifted_u[q - shifted];
      k.modifiers = kShift;
    } else {
      return false;
    }
  }
  *out = k;
  return true;
}

std::vector<uint8_t> hid_report_bytes(const HidKey& k, bool down) {
  if (k.report == HidReport::consumer) {
    const uint16_t u = down ? k.usage : 0;
    return {static_cast<uint8_t>(u & 0xFF), static_cast<uint8_t>(u >> 8)};
  }
  std::vector<uint8_t> v(8, 0);
  if (down) {
    v[0] = k.modifiers;
    v[2] = static_cast<uint8_t>(k.usage);
  }
  return v;
}

json hid_key_names() {
  json c = json::array(), k = json::array();
  for (const Named& n : kConsumer) c.push_back(n.name);
  for (const Named& n : kKeyboard) k.push_back(n.name);
  return json{{"consumer", c}, {"keyboard", k}};
}

// ---- the remote ----------------------------------------------------------------------------------

HidRemote::HidRemote(BtManager& bt, AdvManager& adv, GattServer& gatt) : bt_(bt), adv_(adv), gatt_(gatt) {}

HidRemote::~HidRemote() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::string HidRemote::report_path(HidReport r) const {
  return gatt_.root() + (r == HidReport::consumer ? kConsumerReport : kKeyboardReport);
}

json HidRemote::state() const {
  const json g = gatt_.state();
  bool sub_c = false, sub_k = false;
  for (const json& v : g.value("values", json::array())) {
    const std::string p = v.value("path", std::string{});
    if (p == report_path(HidReport::consumer)) sub_c = v.value("notifying", false);
    if (p == report_path(HidReport::keyboard)) sub_k = v.value("notifying", false);
  }
  std::lock_guard<std::mutex> lk(m_);
  json adv = nullptr;
  if (adv_id_) {
    for (const json& in : adv_.list().value("instances", json::array()))
      if (in.value("id", 0) == adv_id_)
        adv = json{{"id", adv_id_}, {"state", in.value("state", std::string{})}, {"error", in.value("error", std::string{})}};
  }
  return json{{"enabled", enabled_},
              {"name", name_},
              {"gatt", json{{"state", g.value("state", std::string{})}, {"error", g.value("error", std::string{})}}},
              {"advertisement", adv},
              {"subscribed", json{{"consumer", sub_c}, {"keyboard", sub_k}}},
              {"battery", battery_},
              {"last", last_.is_null() ? json(nullptr) : last_},
              {"keys", hid_key_names()}};
}

bool HidRemote::enable(const std::string& name, std::string* err) {
  const std::string n = name.empty() ? "btbench remote" : name;
  if (n.size() > 29) {
    *err = "the name is at most 29 bytes (it has to fit the advertisement)";
    return false;
  }
  if (!gatt_.set(hid_app_def(), err)) return false;
  const AdvSpec spec = advert(n);
  int old = 0;
  {
    std::lock_guard<std::mutex> lk(m_);
    old = adv_id_;
    adv_id_ = 0;
  }
  if (old) adv_.remove(old, err);
  json out;
  if (!adv_.add(spec, &out, err)) {
    gatt_.clear();
    return false;
  }
  if (out.value("state", std::string{}) == "failed") {
    *err = "advertising refused: " + out.value("error", std::string("no reason given"));
    adv_.remove(out.value("id", 0), nullptr);
    gatt_.clear();
    return false;
  }
  std::lock_guard<std::mutex> lk(m_);
  enabled_ = true;
  name_ = n;
  adv_id_ = out.value("id", 0);
  battery_ = 100;
  if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
  LOG_INFO("hid remote: enabled as \"{}\"", n);
  return true;
}

void HidRemote::disable() {
  int id = 0;
  {
    std::lock_guard<std::mutex> lk(m_);
    enabled_ = false;
    steps_.clear();
    id = adv_id_;
    adv_id_ = 0;
    for (bool& h : held_) h = false;
  }
  std::string err;
  if (id) adv_.remove(id, &err);
  gatt_.clear();
  LOG_INFO("hid remote: disabled");
}

bool HidRemote::key(const std::string& name, const std::string& action, unsigned hold_ms, std::string* err) {
  HidKey k;
  if (!hid_key(name, &k)) {
    *err = "unknown key \"" + name + "\" (see keys in GET /api/le/hid, or consumer:0xNN / keyboard:0xNN)";
    return false;
  }
  if (action != "tap" && action != "down" && action != "up") {
    *err = "action is tap, down or up";
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!enabled_) {
      *err = "the remote is off";
      return false;
    }
    last_ = json{{"key", name}, {"action", action},
                 {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
  }
  const unsigned hold = std::clamp(hold_ms ? hold_ms : 80u, 20u, 5000u);
  if (action == "down") queue({{0, k.report, hid_report_bytes(k, true)}});
  else if (action == "up") queue({{0, k.report, hid_report_bytes(k, false)}});
  else queue({{0, k.report, hid_report_bytes(k, true)}, {hold, k.report, hid_report_bytes(k, false)}});
  return true;
}

bool HidRemote::type(const std::string& text, std::string* err) {
  if (text.size() > 256) {
    *err = "at most 256 characters at a time";
    return false;
  }
  std::vector<Step> steps;
  for (char c : text) {
    HidKey k;
    if (!hid_char_key(c, &k)) {
      *err = std::string("cannot type '") + c + "': printable ASCII only";
      return false;
    }
    // 20 ms down, 20 ms up: fast enough to read as typing, slow enough for a host's connection
    // interval (7.5–30 ms) to carry each notification.
    steps.push_back({20, k.report, hid_report_bytes(k, true)});
    steps.push_back({20, k.report, hid_report_bytes(k, false)});
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!enabled_) {
      *err = "the remote is off";
      return false;
    }
  }
  queue(std::move(steps));
  return true;
}

bool HidRemote::set_battery(int percent, std::string* err) {
  if (percent < 0 || percent > 100) {
    *err = "battery is 0..100";
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!enabled_) {
      *err = "the remote is off";
      return false;
    }
    battery_ = percent;
  }
  gatt_.set_value(gatt_.root() + kBatteryLevel, {static_cast<uint8_t>(percent)});
  return true;
}

void HidRemote::queue(std::vector<Step> steps) {
  {
    std::lock_guard<std::mutex> lk(m_);
    for (Step& s : steps) steps_.push_back(std::move(s));
  }
  cv_.notify_all();
}

// BlueZ drops an advertisement when the adapter powers off (and releases it); a remote that went
// quiet that way would never be found again. Advertised again while it is on.
void HidRemote::keep_advertising() {
  int id;
  std::string name;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!enabled_) return;
    id = adv_id_;
    name = name_;
  }
  std::string state;
  for (const json& in : adv_.list().value("instances", json::array()))
    if (in.value("id", 0) == id) state = in.value("state", std::string{});
  if (state == "active" || state == "pending" || state == "registering") return;
  std::string err;
  if (id) adv_.remove(id, &err);
  AdvSpec spec = advert(name);
  json out;
  if (!adv_.add(spec, &out, &err)) return;
  LOG_INFO("hid remote: advertising again ({})", state.empty() ? "gone" : state);
  std::lock_guard<std::mutex> lk(m_);
  if (enabled_) adv_id_ = out.value("id", 0);
  else adv_.remove(out.value("id", 0), &err);
}

AdvSpec HidRemote::advert(const std::string& name) {
  AdvSpec spec;
  spec.type = "peripheral";
  spec.local_name = name;
  spec.service_uuids = {uuid_full("1812")};
  spec.appearance = 0x0180;  // Generic Remote Control
  spec.discoverable = true;
  spec.has_discoverable = true;
  return spec;
}

void HidRemote::run() {
  std::unique_lock<std::mutex> lk(m_);
  uint64_t next_adv_check = 0;
  while (!stop_) {
    // Idle, it wakes once a second only to release a key held too long, and every five to see
    // that the advertisement is still up.
    cv_.wait_for(lk, std::chrono::seconds(1), [this] { return stop_ || !steps_.empty(); });
    if (stop_) break;
    const uint64_t now = mono_ns();
    if (now >= next_adv_check && steps_.empty()) {
      next_adv_check = now + 5ull * 1000000000ull;
      lk.unlock();
      keep_advertising();
      lk.lock();
    }
    for (int r = 1; r <= 2; ++r) {
      if (held_[r] && now - held_since_ns_[r] > kHoldLimitNs) {
        LOG_INFO("hid remote: releasing report {} held over 10 s", r);
        steps_.push_front({0, static_cast<HidReport>(r), hid_report_bytes(HidKey{static_cast<HidReport>(r), 0, 0}, false)});
      }
    }
    while (!steps_.empty() && !stop_) {
      Step s = std::move(steps_.front());
      steps_.pop_front();
      if (s.delay_ms) {
        cv_.wait_for(lk, std::chrono::milliseconds(s.delay_ms), [this] { return stop_; });
        if (stop_) break;
      }
      const int r = static_cast<int>(s.report);
      held_[r] = std::any_of(s.value.begin(), s.value.end(), [](uint8_t b) { return b != 0; });
      held_since_ns_[r] = mono_ns();
      const std::string path = report_path(s.report);
      lk.unlock();
      gatt_.set_value(path, std::move(s.value));
      lk.lock();
    }
  }
}

}  // namespace btb
