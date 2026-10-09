// What comes back from the target scripts and systemctl, and what is let through to them.
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "check.h"
#include "jobs.h"
#include "journal.h"
#include "scripts.h"
#include "system.h"

using namespace btb;
using json = nlohmann::json;

namespace {

void test_script_json() {
  json j;
  std::string err;
  CHECK(parse_script_json("{\"mode\":\"pipewire\",\"modes\":[\"pipewire\",\"bluealsa\",\"none\"]}\n", &j, &err));
  CHECK_EQ(j["mode"].get<std::string>(), std::string("pipewire"));
  CHECK(parse_script_json("[{\"ssid\":\"x\",\"signal_dbm\":-52}]", &j, &err));
  CHECK(j.is_array());
  CHECK(!parse_script_json("", &j, &err));
  CHECK(!parse_script_json("usage: btbench-wifi status|scan\n", &j, &err));
  CHECK(err.find("usage:") != std::string::npos);
  CHECK(!parse_script_json("\"just a string\"", &j, &err));
}

void test_inputs() {
  std::string err;
  CHECK(audio_mode_ok("bluealsa"));
  CHECK(!audio_mode_ok("pulse"));
  CHECK(wifi_mode_ok("auto") && wifi_mode_ok("off"));
  CHECK(!wifi_mode_ok("mesh"));
  CHECK(wifi_ssid_ok("home-ap", &err));
  CHECK(wifi_ssid_ok("a/b \"c\"", &err));  // legal: it is an argv element, not shell text
  CHECK(!wifi_ssid_ok("", &err));
  CHECK(!wifi_ssid_ok(std::string(33, 'x'), &err));
  CHECK(!wifi_ssid_ok("evil\nnetwork={", &err));
  CHECK(wifi_psk_ok("", &err));
  CHECK(wifi_psk_ok("password1", &err));
  CHECK(!wifi_psk_ok("short", &err));
  CHECK(wifi_psk_ok(std::string(64, 'a'), &err));
  CHECK(!wifi_psk_ok(std::string(64, 'z'), &err));
}

void test_systemctl() {
  const std::vector<std::string> units = {"bluetooth.service", "pipewire.service", "bluealsa.service"};
  const json j = parse_is_active(units, "active\ninactive\nfailed\n");
  CHECK_EQ(j["bluetooth.service"].get<std::string>(), std::string("active"));
  CHECK_EQ(j["bluealsa.service"].get<std::string>(), std::string("failed"));
  const json short_ = parse_is_active(units, "active\n");
  CHECK_EQ(short_["pipewire.service"].get<std::string>(), std::string("unknown"));
  CHECK(restartable_service("bluetooth.service"));
  CHECK(!restartable_service("btbenchd.service"));  // not itself: the request would die with it
  CHECK(!restartable_service("sshd.service"));
}

void test_versions() {
  CHECK_EQ(parse_version("5.84\n"), std::string("5.84"));
  CHECK_EQ(parse_version("bluetoothd 5.84-dirty\n"), std::string("5.84-dirty"));
  CHECK_EQ(parse_version("pipewire\nCompiled with libpipewire 1.4.2\nLinked with libpipewire 1.4.2\n"),
           std::string("1.4.2"));
  CHECK_EQ(parse_version("wireplumber\nCompiled with libwireplumber 0.5.10\n"), std::string("0.5.10"));
  CHECK_EQ(parse_version("v4.3.1\n"), std::string("4.3.1"));
}

void test_bluetoothd_args() {
  std::string err;
  CHECK(bluetoothd_args_ok("", &err));
  CHECK(bluetoothd_args_ok("-d -E", &err));
  CHECK(bluetoothd_args_ok("-d src/adapter.c --plugin=a2dp -K", &err));
  CHECK(!bluetoothd_args_ok("-d\n-E", &err));
  CHECK(!bluetoothd_args_ok("-d $(reboot)", &err));
  CHECK(!bluetoothd_args_ok("-d \"x\"", &err));
  CHECK(!bluetoothd_args_ok("debug", &err));
}

// Only BLUETOOTHD_ARGS is the API's: BLUETOOTHD_NOPLUGIN and comments in the same file survive.
void test_bluetoothd_env() {
  char dir[] = "/tmp/btbench-test-XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);
  const std::string d = dir;
  {
    std::ofstream f(d + "/bluetoothd.env");
    f << "# image default\nBLUETOOTHD_ARGS=\"-d\"\nBLUETOOTHD_NOPLUGIN=\"hfp\"\n";
  }
  std::string err;
  CHECK(write_bluetoothd_args(d, "-d -E -K", &err));
  CHECK_EQ(read_bluetoothd_args(d), std::string("-d -E -K"));
  CHECK_EQ(read_bluetoothd_noplugin(d), std::string("hfp"));
  std::ifstream f(d + "/bluetoothd.env");
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str().find("# image default") == 0);
  // A fresh data dir gets a file with just the arguments.
  unlink((d + "/bluetoothd.env").c_str());
  CHECK(write_bluetoothd_args(d, "-d", &err));
  CHECK_EQ(read_bluetoothd_args(d), std::string("-d"));
  CHECK_EQ(read_bluetoothd_noplugin(d), std::string());
  unlink((d + "/bluetoothd.env").c_str());
  rmdir(dir);
}

void test_names() {
  std::string u;
  CHECK(journal_unit_name("bluetooth", &u) && u == "bluetooth.service");
  CHECK(journal_unit_name("wpa_supplicant@wlan0.service", &u) && u == "wpa_supplicant@wlan0.service");
  CHECK(!journal_unit_name("a b", &u));
  CHECK(!journal_unit_name("x;reboot", &u));
  CHECK(!journal_unit_name("", &u));
  CHECK(job_allowed("l2ping"));
  CHECK(job_allowed("btgatt-client"));
  CHECK(!job_allowed("sh"));
  CHECK(!job_allowed("/usr/bin/l2ping"));  // a name, looked up in PATH, never a path
}

}  // namespace

int main() {
  test_script_json();
  test_inputs();
  test_systemctl();
  test_versions();
  test_bluetoothd_args();
  test_bluetoothd_env();
  test_names();
  return report("test_scripts");
}
