// HoGP both ways. The remote: its report map, keys and report values, and a GATT application BlueZ
// would accept. The host: reading a sysfs tree of Bluetooth input devices, a report descriptor, and
// the reader thread end to end on a FIFO standing in for /dev/input/eventN.
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "check.h"
#include "gatt_server.h"
#include "hid_device.h"
#include "hid_host.h"
#include "uuids.h"

using namespace btb;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

void test_keys() {
  HidKey k;
  CHECK(hid_key("play_pause", &k));
  CHECK_EQ(k.report, HidReport::consumer);
  CHECK_EQ(k.usage, 0xCD);
  CHECK_EQ(hid_report_bytes(k, true), std::vector<uint8_t>({0xCD, 0x00}));
  CHECK_EQ(hid_report_bytes(k, false), std::vector<uint8_t>({0x00, 0x00}));
  CHECK(hid_key("home", &k));
  CHECK_EQ(hid_report_bytes(k, true), std::vector<uint8_t>({0x23, 0x02}));  // AC Home, 0x223
  CHECK(hid_key("enter", &k));
  CHECK_EQ(k.report, HidReport::keyboard);
  CHECK_EQ(hid_report_bytes(k, true), std::vector<uint8_t>({0, 0, 0x28, 0, 0, 0, 0, 0}));
  CHECK_EQ(hid_report_bytes(k, false), std::vector<uint8_t>(8, 0));
  CHECK(hid_key("consumer:0x221", &k));
  CHECK_EQ(k.usage, 0x221);
  CHECK(hid_key("keyboard:40", &k));
  CHECK_EQ(k.usage, 40);
  CHECK(!hid_key("consumer:0x400", &k));  // past the report map's logical maximum
  CHECK(!hid_key("teleport", &k));
  CHECK(!hid_key("consumer:", &k));

  // Typing: shift where the US layout needs it.
  CHECK(hid_char_key('a', &k));
  CHECK_EQ(hid_report_bytes(k, true), std::vector<uint8_t>({0, 0, 0x04, 0, 0, 0, 0, 0}));
  CHECK(hid_char_key('Z', &k));
  CHECK_EQ(hid_report_bytes(k, true), std::vector<uint8_t>({0x02, 0, 0x1D, 0, 0, 0, 0, 0}));
  CHECK(hid_char_key('0', &k) && k.usage == 0x27);
  CHECK(hid_char_key('!', &k) && k.usage == 0x1E && k.modifiers == 0x02);
  CHECK(hid_char_key(' ', &k) && k.usage == 0x2C && k.modifiers == 0);
  CHECK(hid_char_key('?', &k) && k.usage == 0x38 && k.modifiers == 0x02);
  CHECK(!hid_char_key('\x01', &k));
  CHECK(!hid_char_key('\0', &k));

  const json names = hid_key_names();
  CHECK(names["consumer"].size() > 10);
  CHECK(names["keyboard"].size() > 5);
}

void test_report_map() {
  // Our own map, read back by the host side's parser: two applications, two input reports.
  const json s = hid_rdesc_summary(hid_report_map());
  CHECK_EQ(s["applications"].size(), size_t(2));
  CHECK_EQ(s["applications"][0]["name"].get<std::string>(), std::string("Consumer Control"));
  CHECK_EQ(s["applications"][0]["report_ids"], json::array({1}));
  CHECK_EQ(s["applications"][1]["name"].get<std::string>(), std::string("Keyboard"));
  CHECK_EQ(s["applications"][1]["report_ids"], json::array({2}));
  CHECK_EQ(s["reports"].size(), size_t(2));
  CHECK_EQ(s["reports"][0]["id"].get<int>(), 1);
  CHECK_EQ(s["reports"][0]["input_bits"].get<int>(), 16);
  CHECK_EQ(s["reports"][1]["input_bits"].get<int>(), 64);  // the 8-byte boot keyboard
  // A truncated descriptor gives what was read, no more.
  std::vector<uint8_t> cut = hid_report_map();
  cut.resize(5);
  CHECK_EQ(hid_rdesc_summary(cut)["applications"].size(), size_t(0));
  // A mouse, with an extended (page in the high half) usage on the collection.
  const std::vector<uint8_t> mouse = {0x05, 0x01, 0x0B, 0x02, 0x00, 0x01, 0x00, 0xA1, 0x01, 0x09, 0x01,
                                      0xA1, 0x00, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};
  const json m = hid_rdesc_summary(mouse);
  CHECK_EQ(m["applications"][0]["name"].get<std::string>(), std::string("Mouse"));
  CHECK_EQ(m["reports"][0]["input_bits"].get<int>(), 24);
}

void test_app_def() {
  const GattAppDef d = hid_app_def();
  CHECK_EQ(d.services.size(), size_t(3));
  const GattServiceDef& hid = d.services[0];
  CHECK_EQ(uuid_short(hid.uuid), std::string("1812"));
  // The remote writes to chr4 and chr5 (hid_device.cpp's paths): both Reports, input, ids 1 and 2.
  CHECK_EQ(uuid_short(hid.characteristics[1].uuid), std::string("2a4b"));
  CHECK_EQ(hid.characteristics[1].value, hid_report_map());
  for (int i : {4, 5}) {
    const GattCharDef& r = hid.characteristics[i];
    CHECK_EQ(uuid_short(r.uuid), std::string("2a4d"));
    CHECK_EQ(r.descriptors.size(), size_t(1));
    CHECK_EQ(uuid_short(r.descriptors[0].uuid), std::string("2908"));
    CHECK_EQ(r.descriptors[0].value, std::vector<uint8_t>({static_cast<uint8_t>(i - 3), 0x01}));
  }
  CHECK_EQ(uuid_short(d.services[1].characteristics[0].uuid), std::string("2a19"));  // battery: svc1/chr0
  // It goes through the same check a definition from the API does: flags BlueZ knows, sizes.
  GattAppDef back;
  std::string err;
  CHECK(gatt_app_from_json(gatt_app_json(d), &back, &err));
  CHECK(err.empty());
  CHECK_EQ(back.services.size(), size_t(3));
}

void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << s << "\n";
}

// A sysfs tree as the kernel lays it out for a uhid device from bluetoothd: two input devices
// (consumer and keyboard collections) of one HID device, and a USB keyboard that is not ours.
fs::path make_tree(const fs::path& root) {
  const fs::path cls = root / "sys/class/input";
  auto input = [&](const std::string& ev, const std::string& name, const std::string& bus, const std::string& uniq) {
    const fs::path in = cls / ev / "device";
    write(in / "name", name);
    write(in / "uniq", uniq);
    write(in / "phys", "b8:27:eb:50:7b:22");
    write(in / "id/bustype", bus);
    write(in / "id/vendor", "1d6b");
    write(in / "id/product", "0246");
    write(in / "id/version", "0001");
    return in;
  };
  for (const char* ev : {"event10", "event2"}) {
    const fs::path in = input(ev, std::string("Remote ") + (ev[5] == '1' ? "Keyboard" : "Consumer Control"), "0005",
                              "c0:ff:ee:00:00:01");
    const std::vector<uint8_t> map = hid_report_map();
    fs::create_directories(in / "device/hidraw/hidraw0");
    std::ofstream(in / "device/report_descriptor", std::ios::binary).write(reinterpret_cast<const char*>(map.data()), map.size());
  }
  input("event0", "USB Keyboard", "0003", "");
  fs::create_directories(root / "dev/input");
  return root;
}

void test_scan() {
  const fs::path root = fs::temp_directory_path() / ("btb-hid-" + std::to_string(getpid()));
  fs::remove_all(root);
  make_tree(root);
  const auto nodes = hid_scan((root / "sys").string());
  CHECK_EQ(nodes.size(), size_t(2));  // the USB keyboard is not on the Bluetooth bus
  CHECK_EQ(nodes[0].event, std::string("event2"));  // natural order: event2 before event10
  CHECK_EQ(nodes[0].address, std::string("C0:FF:EE:00:00:01"));
  CHECK_EQ(nodes[0].phys, std::string("B8:27:EB:50:7B:22"));
  CHECK_EQ(nodes[0].vendor, 0x1d6b);
  CHECK_EQ(nodes[0].hidraw, std::string("hidraw0"));
  CHECK_EQ(nodes[0].report_descriptor, hid_report_map());

  // The host: one device with two nodes; a listener makes it read, and what it reads comes out
  // named, with the media-key hook called for keys.
  HidHostOptions o;
  o.sys_root = (root / "sys").string();
  o.dev_root = (root / "dev").string();
  o.state_file = (root / "hid.json").string();
  std::atomic<bool> listening{false};
  std::mutex m;
  std::vector<json> got;
  std::vector<std::pair<int, int>> hooked;
  // event2's node is a FIFO the test writes input_events into, like the kernel would.
  const std::string fifo = (root / "dev/input/event2").string();
  CHECK(mkfifo(fifo.c_str(), 0600) == 0);
  {
    HidHost host(o, [&](const std::string& topic, const json& j) {
      std::lock_guard<std::mutex> lk(m);
      if (topic == "hid.input") got.push_back(j);
    }, [&](const std::string& t) { return listening && t == "hid.input"; },
    [&](const std::string& addr, int code, int value) {
      std::lock_guard<std::mutex> lk(m);
      if (addr == "C0:FF:EE:00:00:01") hooked.emplace_back(code, value);
    });
    host.set_media_keys(false);
    json st = host.state();
    CHECK_EQ(st["devices"].size(), size_t(1));
    CHECK_EQ(st["devices"][0]["nodes"].size(), size_t(2));
    CHECK_EQ(st["devices"][0]["summary"]["applications"].size(), size_t(2));
    CHECK_EQ(st["media_keys"].get<bool>(), false);
    CHECK_EQ(st["reading"].get<bool>(), false);  // nobody listens: nothing opened

    listening = true;
    host.poke();
    // The reader opens the FIFO non-blocking; a writer opened after it delivers.
    int w = -1;
    for (int i = 0; i < 100 && w < 0; ++i) {
      w = open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
      if (w < 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    CHECK(w >= 0);
    host.set_media_keys(true);
    input_event ev[3] = {};
    ev[0].type = EV_KEY; ev[0].code = KEY_PLAYPAUSE; ev[0].value = 1;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    ev[2].type = EV_KEY; ev[2].code = KEY_PLAYPAUSE; ev[2].value = 0;
    if (w >= 0) CHECK(::write(w, ev, sizeof ev) == static_cast<ssize_t>(sizeof ev));
    for (int i = 0; i < 100; ++i) {
      {
        std::lock_guard<std::mutex> lk(m);
        if (got.size() >= 2) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    {
      std::lock_guard<std::mutex> lk(m);
      CHECK_EQ(got.size(), size_t(2));  // the SYN is not an event anyone wants
      if (got.size() == 2) {
        CHECK_EQ(got[0]["name"].get<std::string>(), std::string("KEY_PLAYPAUSE"));
        CHECK_EQ(got[0]["value"].get<int>(), 1);
        CHECK_EQ(got[0]["address"].get<std::string>(), std::string("C0:FF:EE:00:00:01"));
        CHECK_EQ(got[1]["value"].get<int>(), 0);
      }
      CHECK_EQ(hooked.size(), size_t(2));
    }
    CHECK_EQ(host.state()["recent"].size(), size_t(2));
    if (w >= 0) close(w);
  }
  // media_keys was kept.
  std::ifstream f(o.state_file);
  CHECK_EQ(json::parse(f)["media_keys"].get<bool>(), true);
  CHECK_EQ(evdev_key_name(KEY_VOLUMEUP), std::string("KEY_VOLUMEUP"));
  CHECK_EQ(evdev_key_name(0x2ff), std::string("KEY_767"));
  fs::remove_all(root);
}

}  // namespace

int main() {
  test_keys();
  test_report_map();
  test_app_def();
  test_scan();
  return report("test_hid");
}
