#include "hid_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "util/log.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb {

namespace {

constexpr int kBusBluetooth = 0x05;  // BUS_BLUETOOTH in linux/input.h
constexpr size_t kRecent = 100;      // events kept for a console that opens late

std::string read_line(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

std::vector<uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int hex_int(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 16)); }

std::vector<std::string> list_dir(const std::string& path, const std::string& prefix) {
  std::vector<std::string> out;
  DIR* d = opendir(path.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.compare(0, prefix.size(), prefix) == 0) out.push_back(n);
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return a.size() != b.size() ? a.size() < b.size() : a < b;  // event2 before event10
  });
  return out;
}

const char* app_name(unsigned page, unsigned usage) {
  if (page == 0x01) {
    switch (usage) {
      case 0x01: return "Pointer";
      case 0x02: return "Mouse";
      case 0x04: return "Joystick";
      case 0x05: return "Gamepad";
      case 0x06: return "Keyboard";
      case 0x07: return "Keypad";
      case 0x08: return "Multi-axis controller";
      case 0x80: return "System Control";
    }
  } else if (page == 0x0C && usage == 0x01) {
    return "Consumer Control";
  } else if (page == 0x0D) {
    switch (usage) {
      case 0x01: return "Digitizer";
      case 0x02: return "Pen";
      case 0x04: return "Touch Screen";
      case 0x05: return "Touch Pad";
    }
  } else if (page >= 0xFF00) {
    return "Vendor defined";
  }
  return nullptr;
}

// The names of the EV_KEY codes a remote, a keyboard's common keys, a mouse and a game pad send
// (linux/input-event-codes.h); the rest print as KEY_<n>.
const std::map<int, const char*>& key_names() {
  static const std::map<int, const char*> m = {
      {KEY_ESC, "KEY_ESC"}, {KEY_1, "KEY_1"}, {KEY_2, "KEY_2"}, {KEY_3, "KEY_3"}, {KEY_4, "KEY_4"},
      {KEY_5, "KEY_5"}, {KEY_6, "KEY_6"}, {KEY_7, "KEY_7"}, {KEY_8, "KEY_8"}, {KEY_9, "KEY_9"},
      {KEY_0, "KEY_0"}, {KEY_MINUS, "KEY_MINUS"}, {KEY_EQUAL, "KEY_EQUAL"}, {KEY_BACKSPACE, "KEY_BACKSPACE"},
      {KEY_TAB, "KEY_TAB"}, {KEY_Q, "KEY_Q"}, {KEY_W, "KEY_W"}, {KEY_E, "KEY_E"}, {KEY_R, "KEY_R"},
      {KEY_T, "KEY_T"}, {KEY_Y, "KEY_Y"}, {KEY_U, "KEY_U"}, {KEY_I, "KEY_I"}, {KEY_O, "KEY_O"},
      {KEY_P, "KEY_P"}, {KEY_LEFTBRACE, "KEY_LEFTBRACE"}, {KEY_RIGHTBRACE, "KEY_RIGHTBRACE"},
      {KEY_ENTER, "KEY_ENTER"}, {KEY_LEFTCTRL, "KEY_LEFTCTRL"}, {KEY_A, "KEY_A"}, {KEY_S, "KEY_S"},
      {KEY_D, "KEY_D"}, {KEY_F, "KEY_F"}, {KEY_G, "KEY_G"}, {KEY_H, "KEY_H"}, {KEY_J, "KEY_J"},
      {KEY_K, "KEY_K"}, {KEY_L, "KEY_L"}, {KEY_SEMICOLON, "KEY_SEMICOLON"}, {KEY_APOSTROPHE, "KEY_APOSTROPHE"},
      {KEY_GRAVE, "KEY_GRAVE"}, {KEY_LEFTSHIFT, "KEY_LEFTSHIFT"}, {KEY_BACKSLASH, "KEY_BACKSLASH"},
      {KEY_Z, "KEY_Z"}, {KEY_X, "KEY_X"}, {KEY_C, "KEY_C"}, {KEY_V, "KEY_V"}, {KEY_B, "KEY_B"},
      {KEY_N, "KEY_N"}, {KEY_M, "KEY_M"}, {KEY_COMMA, "KEY_COMMA"}, {KEY_DOT, "KEY_DOT"},
      {KEY_SLASH, "KEY_SLASH"}, {KEY_RIGHTSHIFT, "KEY_RIGHTSHIFT"}, {KEY_LEFTALT, "KEY_LEFTALT"},
      {KEY_SPACE, "KEY_SPACE"}, {KEY_CAPSLOCK, "KEY_CAPSLOCK"}, {KEY_F1, "KEY_F1"}, {KEY_F2, "KEY_F2"},
      {KEY_F3, "KEY_F3"}, {KEY_F4, "KEY_F4"}, {KEY_F5, "KEY_F5"}, {KEY_F6, "KEY_F6"}, {KEY_F7, "KEY_F7"},
      {KEY_F8, "KEY_F8"}, {KEY_F9, "KEY_F9"}, {KEY_F10, "KEY_F10"}, {KEY_F11, "KEY_F11"}, {KEY_F12, "KEY_F12"},
      {KEY_RIGHTCTRL, "KEY_RIGHTCTRL"}, {KEY_RIGHTALT, "KEY_RIGHTALT"}, {KEY_HOME, "KEY_HOME"},
      {KEY_UP, "KEY_UP"}, {KEY_PAGEUP, "KEY_PAGEUP"}, {KEY_LEFT, "KEY_LEFT"}, {KEY_RIGHT, "KEY_RIGHT"},
      {KEY_END, "KEY_END"}, {KEY_DOWN, "KEY_DOWN"}, {KEY_PAGEDOWN, "KEY_PAGEDOWN"}, {KEY_INSERT, "KEY_INSERT"},
      {KEY_DELETE, "KEY_DELETE"}, {KEY_MUTE, "KEY_MUTE"}, {KEY_VOLUMEDOWN, "KEY_VOLUMEDOWN"},
      {KEY_VOLUMEUP, "KEY_VOLUMEUP"}, {KEY_POWER, "KEY_POWER"}, {KEY_LEFTMETA, "KEY_LEFTMETA"},
      {KEY_RIGHTMETA, "KEY_RIGHTMETA"}, {KEY_COMPOSE, "KEY_COMPOSE"}, {KEY_STOP, "KEY_STOP"},
      {KEY_MENU, "KEY_MENU"}, {KEY_SLEEP, "KEY_SLEEP"}, {KEY_WAKEUP, "KEY_WAKEUP"}, {KEY_WWW, "KEY_WWW"},
      {KEY_BACK, "KEY_BACK"}, {KEY_FORWARD, "KEY_FORWARD"}, {KEY_EJECTCD, "KEY_EJECTCD"},
      {KEY_NEXTSONG, "KEY_NEXTSONG"}, {KEY_PLAYPAUSE, "KEY_PLAYPAUSE"}, {KEY_PREVIOUSSONG, "KEY_PREVIOUSSONG"},
      {KEY_STOPCD, "KEY_STOPCD"}, {KEY_RECORD, "KEY_RECORD"}, {KEY_REWIND, "KEY_REWIND"},
      {KEY_PHONE, "KEY_PHONE"}, {KEY_CONFIG, "KEY_CONFIG"}, {KEY_HOMEPAGE, "KEY_HOMEPAGE"},
      {KEY_PLAYCD, "KEY_PLAYCD"}, {KEY_PAUSECD, "KEY_PAUSECD"}, {KEY_PLAY, "KEY_PLAY"},
      {KEY_FASTFORWARD, "KEY_FASTFORWARD"}, {KEY_SEARCH, "KEY_SEARCH"}, {KEY_SELECT, "KEY_SELECT"},
      {KEY_CHANNELUP, "KEY_CHANNELUP"}, {KEY_CHANNELDOWN, "KEY_CHANNELDOWN"}, {KEY_MEDIA, "KEY_MEDIA"},
      {KEY_BRIGHTNESSDOWN, "KEY_BRIGHTNESSDOWN"}, {KEY_BRIGHTNESSUP, "KEY_BRIGHTNESSUP"},
      {BTN_LEFT, "BTN_LEFT"}, {BTN_RIGHT, "BTN_RIGHT"}, {BTN_MIDDLE, "BTN_MIDDLE"}, {BTN_SIDE, "BTN_SIDE"},
      {BTN_EXTRA, "BTN_EXTRA"}, {BTN_SOUTH, "BTN_SOUTH"}, {BTN_EAST, "BTN_EAST"}, {BTN_NORTH, "BTN_NORTH"},
      {BTN_WEST, "BTN_WEST"}, {BTN_TL, "BTN_TL"}, {BTN_TR, "BTN_TR"}, {BTN_SELECT, "BTN_SELECT"},
      {BTN_START, "BTN_START"}, {BTN_MODE, "BTN_MODE"}, {BTN_THUMBL, "BTN_THUMBL"}, {BTN_THUMBR, "BTN_THUMBR"},
      {BTN_TOUCH, "BTN_TOUCH"},
  };
  return m;
}

const char* rel_name(int code) {
  switch (code) {
    case REL_X: return "REL_X";
    case REL_Y: return "REL_Y";
    case REL_HWHEEL: return "REL_HWHEEL";
    case REL_WHEEL: return "REL_WHEEL";
  }
  return nullptr;
}

}  // namespace

std::string evdev_key_name(int code) {
  const auto& m = key_names();
  const auto it = m.find(code);
  return it != m.end() ? it->second : "KEY_" + std::to_string(code);
}

std::vector<HidInputNode> hid_scan(const std::string& sys_root) {
  std::vector<HidInputNode> out;
  const std::string cls = sys_root + "/class/input";
  for (const std::string& ev : list_dir(cls, "event")) {
    // eventN/device is the input device (name, uniq, phys, id/); its device/ is the HID device
    // (report_descriptor, hidraw/) when there is one.
    const std::string in = cls + "/" + ev + "/device";
    HidInputNode n;
    n.bustype = hex_int(read_line(in + "/id/bustype"));
    if (n.bustype != kBusBluetooth) continue;
    n.event = ev;
    n.name = read_line(in + "/name");
    n.address = upper(read_line(in + "/uniq"));
    n.phys = upper(read_line(in + "/phys"));
    n.vendor = hex_int(read_line(in + "/id/vendor"));
    n.product = hex_int(read_line(in + "/id/product"));
    n.version = hex_int(read_line(in + "/id/version"));
    const std::string hid = in + "/device";
    n.report_descriptor = read_bytes(hid + "/report_descriptor");
    if (!n.report_descriptor.empty()) {
      char real[4096];
      const ssize_t r = readlink(hid.c_str(), real, sizeof real - 1);
      std::string target = r > 0 ? std::string(real, static_cast<size_t>(r)) : hid;
      n.hid = target.substr(target.find_last_of('/') + 1);
      if (n.hid == "device") n.hid.clear();  // not a link (a test's tree): no name to show
      const auto raws = list_dir(hid + "/hidraw", "hidraw");
      if (!raws.empty()) n.hidraw = raws.front();
    }
    out.push_back(std::move(n));
  }
  return out;
}

json hid_rdesc_summary(const std::vector<uint8_t>& d) {
  json apps = json::array();
  std::map<int, std::array<unsigned, 3>> bits;  // report id → input, output, feature bits
  unsigned page = 0, size = 0, count = 0, id = 0, depth = 0, items = 0;
  std::vector<unsigned> usages;
  size_t i = 0;
  while (i < d.size()) {
    const uint8_t p = d[i++];
    if (p == 0xFE) {  // a long item: its data size, its tag, the data
      if (i + 2 > d.size()) break;
      i += 2 + d[i];
      continue;
    }
    const unsigned n = (p & 3) == 3 ? 4 : (p & 3);
    if (i + n > d.size()) break;
    uint32_t v = 0;
    for (unsigned k = 0; k < n; ++k) v |= static_cast<uint32_t>(d[i + k]) << (8 * k);
    i += n;
    ++items;
    const unsigned type = (p >> 2) & 3, tag = p >> 4;
    if (type == 1) {  // global
      if (tag == 0) page = v;
      else if (tag == 7) size = v;
      else if (tag == 8) {
        id = v;
        // Report IDs come inside the application collection they belong to.
        if (depth > 0 && !apps.empty()) {
          json& ids = apps.back()["report_ids"];
          if (!ids.is_array()) ids = json::array();
          if (std::find(ids.begin(), ids.end(), json(v)) == ids.end()) ids.push_back(v);
        }
      }
      else if (tag == 9) count = v;
    } else if (type == 2) {  // local
      if (tag == 0) usages.push_back(v);
    } else if (type == 0) {  // main
      if (tag == 10) {       // Collection
        if (depth == 0 && v == 1) {
          unsigned u = usages.empty() ? 0 : usages.front();
          unsigned pg = page;
          if (u > 0xFFFF) {  // an extended (4-byte) usage carries its page in the high half
            pg = u >> 16;
            u &= 0xFFFF;
          }
          json a{{"page", pg}, {"usage", u}};
          if (const char* nm = app_name(pg, u)) a["name"] = nm;
          apps.push_back(std::move(a));
        }
        ++depth;
      } else if (tag == 12) {
        if (depth) --depth;
      } else if (tag == 8 || tag == 9 || tag == 11) {
        bits[static_cast<int>(id)][tag == 8 ? 0 : tag == 9 ? 1 : 2] += size * count;
      }
      usages.clear();
    }
  }
  json reports = json::array();
  for (const auto& [rid, b] : bits)
    reports.push_back(json{{"id", rid}, {"input_bits", b[0]}, {"output_bits", b[1]}, {"feature_bits", b[2]}});
  return json{{"applications", apps}, {"reports", reports}, {"items", items}, {"bytes", d.size()}};
}

// ---- the host ------------------------------------------------------------------------------------

HidHost::HidHost(HidHostOptions opt, Publish publish, HasSubscribers has_subscribers, KeyHook media_hook)
    : opt_(std::move(opt)),
      publish_(std::move(publish)),
      has_subscribers_(std::move(has_subscribers)),
      hook_(std::move(media_hook)) {
  if (!opt_.state_file.empty()) {
    std::ifstream f(opt_.state_file);
    json j = json::parse(f, nullptr, false);
    if (j.is_object()) media_keys_ = j.value("media_keys", true);
  }
  thread_ = std::thread([this] { run(); });
}

HidHost::~HidHost() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void HidHost::set_media_keys(bool on) {
  media_keys_ = on;
  if (!opt_.state_file.empty()) {
    std::ofstream f(opt_.state_file + ".tmp");
    f << json{{"media_keys", on}}.dump() << "\n";
    f.close();
    if (f) std::rename((opt_.state_file + ".tmp").c_str(), opt_.state_file.c_str());
  }
  poke();
}

void HidHost::poke() {
  rescan_ = true;
  cv_.notify_all();
}

bool HidHost::wanted() const { return media_keys_ || has_subscribers_("hid.input"); }

json HidHost::state() {
  // Grouped by device: a remote with consumer keys and a keyboard is one device with two event
  // nodes and one report descriptor.
  json devices = json::array();
  std::map<std::string, size_t> at;
  for (const HidInputNode& n : hid_scan(opt_.sys_root)) {
    const std::string key = n.address.empty() ? n.hid : n.address;
    auto it = at.find(key);
    if (it == at.end()) {
      json d{{"address", n.address},
             {"name", n.name},
             {"adapter", n.phys},
             {"vendor", n.vendor},
             {"product", n.product},
             {"version", n.version},
             {"hid", n.hid},
             {"nodes", json::array()}};
      if (!n.report_descriptor.empty()) {
        d["report_descriptor"] = to_hex(n.report_descriptor);
        d["summary"] = hid_rdesc_summary(n.report_descriptor);
      }
      at[key] = devices.size();
      devices.push_back(std::move(d));
      it = at.find(key);
    }
    json& d = devices[it->second];
    // The device's name is the common prefix bluetoothd's devices share; each node keeps its own.
    d["nodes"].push_back(json{{"event", n.event}, {"name", n.name}, {"hidraw", n.hidraw}});
  }
  std::lock_guard<std::mutex> lk(m_);
  json recent = json::array();
  for (const json& e : recent_) recent.push_back(e);
  return json{{"media_keys", media_keys_.load()}, {"reading", reading_}, {"devices", devices}, {"recent", recent}};
}

void HidHost::run() {
  struct Open {
    int fd;
    HidInputNode node;
  };
  std::map<std::string, Open> open;  // by event node
  uint64_t next_scan = 0, last_scan = 0;
  auto close_all = [&] {
    for (auto& [ev, o] : open) ::close(o.fd);
    open.clear();
  };
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(m_);
      if (stop_) break;
      if (!wanted()) {
        if (reading_) {
          reading_ = false;
          lk.unlock();
          close_all();
          LOG_INFO("hid host: nobody listens, input devices closed");
          lk.lock();
        }
        // The hub has no callback for a new subscriber: a look once a second is the cost.
        cv_.wait_for(lk, std::chrono::seconds(1));
        continue;
      }
      reading_ = true;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    // A poke comes with every change BlueZ reports (RSSI during a scan, too): at most two looks a
    // second however many there are.
    if (now_ms >= next_scan || (rescan_ && now_ms >= last_scan + 500)) {
      rescan_ = false;
      last_scan = now_ms;
      // Devices come and go with their connections: look again every two seconds while one is
      // open, every five while none is (poke() brings a look forward).
      next_scan = now_ms + (open.empty() ? 5000 : 2000);
      std::map<std::string, HidInputNode> seen;
      for (HidInputNode& n : hid_scan(opt_.sys_root)) seen[n.event] = std::move(n);
      for (auto it = open.begin(); it != open.end();) {
        if (!seen.count(it->first)) {
          ::close(it->second.fd);
          it = open.erase(it);
        } else {
          ++it;
        }
      }
      for (auto& [ev, n] : seen) {
        if (open.count(ev)) continue;
        const std::string path = opt_.dev_root + "/input/" + ev;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
          LOG_DEBUG("hid host: cannot open {}: {}", path, strerror(errno));
          continue;
        }
        LOG_INFO("hid host: reading {} ({} {})", ev, n.name, n.address);
        open[ev] = Open{fd, std::move(n)};
      }
    }
    std::vector<pollfd> fds;
    std::vector<Open*> who;
    for (auto& [ev, o] : open) {
      fds.push_back(pollfd{o.fd, POLLIN, 0});
      who.push_back(&o);
    }
    if (fds.empty()) {
      std::unique_lock<std::mutex> lk(m_);
      // Until the next look: the periodic one, or a poke's once the half-second has passed (a
      // poke that came too early is waited out, not spun on).
      const bool poked = rescan_;
      const uint64_t until = poked ? std::min(next_scan, last_scan + 500) : next_scan;
      const auto wait = std::chrono::milliseconds(until > now_ms ? until - now_ms : 0);
      if (poked) cv_.wait_for(lk, wait, [this] { return stop_; });
      else cv_.wait_for(lk, wait, [this] { return stop_ || rescan_.load(); });
      continue;
    }
    if (::poll(fds.data(), fds.size(), 500) <= 0) continue;
    std::vector<std::string> gone;
    for (size_t k = 0; k < fds.size(); ++k) {
      if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        // Gone (the device disconnected): closed now, and opened again if a scan still finds it.
        gone.push_back(who[k]->node.event);
        continue;
      }
      if (!(fds[k].revents & POLLIN)) continue;
      input_event evs[32];
      const ssize_t r = ::read(fds[k].fd, evs, sizeof evs);
      if (r <= 0) continue;
      const size_t count = static_cast<size_t>(r) / sizeof(input_event);
      for (size_t e = 0; e < count; ++e) {
        const input_event& x = evs[e];
        if (x.type != EV_KEY && x.type != EV_REL && x.type != EV_ABS) continue;
        const Open& o = *who[k];
        if (x.type == EV_KEY && hook_ && media_keys_) hook_(o.node.address, x.code, x.value);
        const char* kind = x.type == EV_KEY ? "key" : x.type == EV_REL ? "rel" : "abs";
        std::string name = x.type == EV_KEY ? evdev_key_name(x.code)
                           : x.type == EV_REL && rel_name(x.code) ? rel_name(x.code)
                                                                   : std::string(kind) + "_" + std::to_string(x.code);
        json j{{"address", o.node.address},
               {"device", o.node.name},
               {"event", o.node.event},
               {"type", kind},
               {"code", x.code},
               {"name", name},
               {"value", x.value},
               {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
        {
          std::lock_guard<std::mutex> lk(m_);
          recent_.push_back(j);
          while (recent_.size() > kRecent) recent_.pop_front();
        }
        publish_("hid.input", j);
      }
    }
    for (const std::string& ev : gone) {
      auto it = open.find(ev);
      if (it == open.end()) continue;
      ::close(it->second.fd);
      open.erase(it);
      next_scan = 0;
    }
  }
  close_all();
}

}  // namespace btb
