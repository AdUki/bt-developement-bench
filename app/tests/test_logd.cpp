// btbench-logd: which file a journal entry goes to, and the native line it becomes — checked
// against clog's own patterns for those files (app/clog/clog.conf), so clog colours them.
#include <cstdlib>
#include <regex>

#include "check.h"
#include "logd.h"

using namespace btb::logd;

namespace {

// 2026-10-10 10:00:01.234567 UTC, shown in UTC.
constexpr uint64_t kT = 1791626401234567ull;

bool matches(const std::string& line, const char* re) { return std::regex_search(line, std::regex(re)); }

void test_route() {
  Entry e;
  e.transport = "kernel";
  CHECK_EQ(route(e).file, std::string("kernel.log"));
  e = Entry{};
  e.unit = "bluetooth.service";
  CHECK_EQ(route(e).file, std::string("bluez.log"));
  e.unit = "";
  e.ident = "pipewire";
  CHECK(route(e).format == Format::pipewire);
  e.ident = "wireplumber";
  CHECK_EQ(route(e).file, std::string("wireplumber.log"));
  e.ident = "wpa_supplicant";
  CHECK_EQ(route(e).file, std::string("wpasupplicant.log"));
  e.ident = "btbenchd";
  CHECK_EQ(route(e).file, std::string("btbenchd.log"));
  e.ident = "systemd";
  CHECK_EQ(route(e).file, std::string("system.log"));
  CHECK(route(e).prefix_ident);
}

void test_formats() {
  Entry e;
  e.realtime_us = kT;
  e.pid = 305;

  // bluez group: ^\S+ (\d{8} \d\d:\d\d:\d\d)\.\d\d\d (?:(DEBUG)|INFO|(WARN)|(ERROR)) (?:(\S+\.[ch]):)?
  e.ident = "bluetoothd";
  e.priority = 7;
  e.message = "src/adapter.c:adapter_service_insert() /org/bluez/hci0";
  const std::string bz = format(e, route(e));
  CHECK_EQ(bz, std::string("bluetoothd[305]: 20261010 10:00:01.234 DEBUG src/adapter.c:adapter_service_insert() /org/bluez/hci0"));
  CHECK(matches(bz, R"(^\S+ (\d{8} \d\d:\d\d:\d\d)\.\d\d\d (?:(DEBUG)|INFO|(WARN)|(ERROR)) (?:(\S+\.[ch]):)?)"));
  e.priority = 3;
  CHECK(format(e, route(e)).find(" ERROR ") != std::string::npos);

  // pipewire group.
  e = Entry{};
  e.realtime_us = kT;
  e.ident = "pipewire";
  e.priority = 4;
  e.message = "spa.alsa: front:0p: snd_pcm_mmap_commit error: Broken pipe";
  e.code_file = "/usr/src/debug/pipewire/1.6.9/spa/plugins/alsa/alsa-pcm.c";
  e.code_line = 3330;
  e.code_func = "alsa_write_frames";
  const std::string pw = format(e, route(e));
  CHECK_EQ(pw, std::string("[W][10:00:01.234567] spa.alsa     | [      alsa-pcm.c: 3330 alsa_write_frames()] front:0p: snd_pcm_mmap_commit error: Broken pipe"));
  CHECK(matches(pw, R"(^\[(?:-|(T|\*T\*)|(D)|I|(W)|(E))\]\[(\d?\d:\d\d:\d\d)\.\d{6}\]\s+(\S+)\s+\|\s+\[\s*([^:]+):)"));

  // wireplumber group.
  e = Entry{};
  e.realtime_us = kT;
  e.ident = "wireplumber";
  e.priority = 5;
  e.topic = "wp-device";
  e.message = "wp-device: SPA handle 'api.libcamera.enum.manager' could not be loaded";
  e.code_file = "/usr/src/debug/wireplumber/0.5.18/lib/wp/device.c";
  e.code_line = 1003;
  e.code_func = "wp_spa_device_new_from_spa_factory";
  const std::string wp = format(e, route(e));
  CHECK_EQ(wp, std::string("M 10:00:01.234567          wp-device device.c:1003:wp_spa_device_new_from_spa_factory: SPA handle 'api.libcamera.enum.manager' could not be loaded"));
  CHECK(matches(wp, R"(^(?:-|(T)|(D)|I|M|(W)|(E)|(C)|U) (\d{2}:\d{2}:\d{2})\.\d{6}\s+(\S+) [^:]+:[^:]+:[^:]+:)"));

  // wpasupplicant group: ^(\d{10}).\d{6}: (\S+:)?
  e = Entry{};
  e.realtime_us = kT;
  e.ident = "wpa_supplicant";
  e.message = "wlan0: CTRL-EVENT-CONNECTED";
  CHECK_EQ(format(e, route(e)), std::string("1791626401.234567: wlan0: CTRL-EVENT-CONNECTED"));

  // nsdk (the default group) for btbenchd, the kernel and the rest.
  e = Entry{};
  e.realtime_us = kT;
  e.ident = "btbenchd";
  e.pid = 6601;
  e.priority = 6;
  e.message = "scripts: btbench-btdebug on mgmt";
  const std::string bb = format(e, route(e));
  CHECK_EQ(bb, std::string("20261010 10:00:01.234 [6601.6601] INFO scripts: btbench-btdebug on mgmt"));
  CHECK(matches(bb, R"(^(\d{8} \d\d:\d\d:\d\d)\.\d\d\d \[\d+\.\d+\] (?:(TRACE)|(DEBUG)|INFO|(WARN(?:ING)?)|(ERROR)|(FATAL)) (?:([^:/]+:)|([^/:]+/)([^:]*:)))"));
  e.ident = "systemd";
  e.unit = "";
  e.pid = 1;
  e.message = "Started foo.service";
  CHECK_EQ(format(e, route(e)), std::string("20261010 10:00:01.234 [1.1] INFO systemd: Started foo.service"));
}

}  // namespace

int main() {
  setenv("TZ", "UTC", 1);
  test_route();
  test_formats();
  return report("test_logd");
}
