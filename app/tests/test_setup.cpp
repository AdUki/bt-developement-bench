// Setup: what btbenchd writes for bluetoothd (main.conf, --noplugin) and WirePlumber, from the
// profiles, plugins and settings the API takes — checked as text, without restarting anything.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.h"
#include "setup.h"

using namespace btb;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }

void test_merge() {
  const std::string base =
      "[General]\n"
      "# Possible values: dual, bredr, le\n"
      "#ControllerMode = dual\n"
      "Name = btbench\n"
      "\n"
      "[LE]\n"
      "MinConnectionInterval = 24\n"
      "[Policy]\n"
      "#AutoEnable=true\n";
  const std::string out = main_conf_merge(base, {{"General.ControllerMode", "le"},
                                                {"LE.MinConnectionInterval", 7.5},
                                                {"LE.ConnectionSupervisionTimeout", 420},
                                                {"Policy.AutoEnable", false},
                                                {"GATT.Channels", 3},
                                                {"Monitor.bluez5.enable-msbc", true}});
  // A commented example gets the live line after it; the doc comment is not a key line.
  CHECK(has(out, "#ControllerMode = dual\nControllerMode = le"));
  CHECK(has(out, "# Possible values: dual, bredr, le\n"));
  // A live line is replaced in place, ms turned into BlueZ's 1.25 ms units.
  CHECK(has(out, "MinConnectionInterval = 6  # btbenchd setup"));
  CHECK(!has(out, "MinConnectionInterval = 24"));
  // A missing key goes to the end of its section (10 ms units), a missing section to the end.
  CHECK(has(out, "ConnectionSupervisionTimeout = 42  # btbenchd setup\n[Policy]"));
  CHECK(has(out, "AutoEnable = false"));
  CHECK(has(out, "\n[GATT]\nChannels = 3"));
  // WirePlumber settings never reach main.conf.
  CHECK(!has(out, "msbc"));
  CHECK(has(out, "Name = btbench"));
}

void test_args_and_wireplumber() {
  CHECK_EQ(bluetoothd_setup_args({"hfp", "policy"}, ""), std::string("--noplugin=hfp,policy"));
  CHECK_EQ(bluetoothd_setup_args({}, "/data/btbench/main.conf"), std::string("--configfile=/data/btbench/main.conf"));
  CHECK_EQ(bluetoothd_setup_args({"hfp"}, "/x"), std::string("--noplugin=hfp --configfile=/x"));
  CHECK_EQ(bluetoothd_setup_args({}, ""), std::string(""));
  const std::string wp = wireplumber_roles_conf({"a2dp_source", "bap_sink"},
      {{"Monitor.bluez5.codecs", json::array({"sbc", "ldac"})}, {"Monitor.bluez5.enable-hw-volume", false},
       {"Monitor.bluez5.default.rate", "48000"}, {"Device.bluez5.a2dp.ldac.quality", "hq"},
       {"Settings.bluetooth.autoswitch-to-headset-profile", false}, {"LE.ConnectionLatency", 2}});
  CHECK(has(wp, "override.bluez5.roles = [ a2dp_sink bap_source hfp_hf hfp_ag ]"));
  CHECK(has(wp, "override.bluez5.codecs = [ sbc ldac ]"));
  CHECK(has(wp, "  bluez5.enable-hw-volume = false\n"));
  CHECK(has(wp, "bluez5.default.rate = 48000\n"));  // a number, not "48000"
  CHECK(has(wp, "device.name = \"~bluez_card.*\""));
  CHECK(has(wp, "bluez5.a2dp.ldac.quality = \"hq\""));
  CHECK(has(wp, "wireplumber.settings = {\n  bluetooth.autoswitch-to-headset-profile = false\n}"));
  CHECK(!has(wp, "ConnectionLatency"));
  CHECK_EQ(obexd_noplugin({"ftp", "pcsuite"}), std::string("ftp,pcsuite"));
}

void test_validation() {
  std::string err;
  CHECK(setup_setting_ok("General.ControllerMode", "bredr", &err));
  CHECK(!setup_setting_ok("General.ControllerMode", "wifi", &err));
  CHECK(setup_setting_ok("LE.MinConnectionInterval", 7.5, &err));
  CHECK(!setup_setting_ok("LE.MinConnectionInterval", 5, &err));
  CHECK(!setup_setting_ok("LE.MinConnectionInterval", "7.5", &err));
  CHECK(setup_setting_ok("GATT.Cache", nullptr, &err));  // back to BlueZ's default
  CHECK(!setup_setting_ok("General.Nonsense", true, &err));
  CHECK(setup_setting_ok("Monitor.bluez5.codecs", json::array({"sbc", "aac"}), &err));
  CHECK(!setup_setting_ok("Monitor.bluez5.codecs", json::array({"mp3"}), &err));
  CHECK(!setup_setting_ok("General.FastConnectable", 1, &err));
  const json schema = setup_schema();
  CHECK(schema["profiles"].size() > 10);
  CHECK(schema["plugins"].size() > 20);
}

void test_setup() {
  const fs::path dir = fs::temp_directory_path() / ("btb-setup-" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "main.conf.base") << "[General]\n#ControllerMode = dual\n";
  std::ofstream(dir / "bluetoothd.env") << "BLUETOOTHD_ARGS=\"-d\"\n";
  SetupOptions o;
  o.data_dir = dir.string();
  o.base_main_conf = (dir / "main.conf.base").string();
  o.wireplumber_conf = (dir / "wp.conf").string();
  o.apply = false;
  bool remote = false;
  std::string obex_got = "-";
  {
    Setup s(o, [&] { return remote; }, [&](bool on, std::string*) { remote = on; return true; },
            [&](const std::string& np) { obex_got = np; });
    json st = s.state();
    // The default: everything on but BlueZ's own HFP.
    for (const json& p : st["profiles"]) CHECK_EQ(p["on"].get<bool>(), p["id"] != "bluez_hfp" && p["id"] != "hid_remote");
    json out;
    std::string err;
    int status = 0;
    CHECK(s.change(json::parse(R"({"profiles": {"a2dp_source": false, "obex_ftp": false, "le_audio": false, "hid_remote": true},
        "plugins": {"policy": false}, "settings": {"General.ControllerMode": "le", "Monitor.bluez5.enable-msbc": false}})"), &out, &err, &status));
    CHECK(err.empty());
    CHECK(remote);
    CHECK_EQ(obex_got, std::string("ftp,pcsuite"));
    CHECK_EQ(out["pending"].get<bool>(), true);  // written, not applied (apply is off)
    const std::string env = slurp(dir / "bluetoothd.env");
    CHECK(has(env, "BLUETOOTHD_ARGS=\"-d\""));  // the System page's line is kept
    CHECK(has(env, "--noplugin=bap,bass,ccp,csip,gmap,hfp,mcp,micp,policy,tmap,vcp"));
    CHECK(has(env, "--configfile=" + (dir / "main.conf").string()));
    CHECK(has(slurp(dir / "main.conf"), "ControllerMode = le"));
    CHECK(has(slurp(dir / "wp.conf"), "override.bluez5.roles = [ a2dp_sink hfp_hf hfp_ag ]"));
    CHECK(has(slurp(dir / "wp.conf"), "bluez5.enable-msbc = false"));
    for (const json& p : out["profiles"]) {
      if (p["id"] == "a2dp_source" || p["id"] == "le_audio") CHECK_EQ(p["on"].get<bool>(), false);
      if (p["id"] == "a2dp_sink" || p["id"] == "hid_remote") CHECK_EQ(p["on"].get<bool>(), true);
    }
    // Refused as a whole: nothing changes.
    CHECK(!s.change(json::parse(R"({"profiles": {"a2dp_sink": false}, "plugins": {"warp": false}})"), &out, &err, &status));
    CHECK_EQ(status, 400);
    CHECK(!s.change(json::parse(R"({"settings": {"LE.MinConnectionInterval": 50, "LE.MaxConnectionInterval": 20}})"), &out, &err, &status));
    // Settings back to defaults: no main.conf argument any more.
    CHECK(s.change(json::parse(R"({"settings": {"General.ControllerMode": null}})"), &out, &err, &status));
    CHECK(!has(slurp(dir / "bluetoothd.env"), "--configfile"));
  }
  // Kept across a restart.
  Setup again(o, [&] { return remote; }, nullptr, nullptr);
  for (const json& p : again.state()["profiles"])
    if (p["id"] == "a2dp_source") CHECK_EQ(p["on"].get<bool>(), false);
  CHECK_EQ(again.obex_noplugin(), std::string("ftp,pcsuite"));
  fs::remove_all(dir);
}

}  // namespace

int main() {
  test_merge();
  test_args_and_wireplumber();
  test_validation();
  test_setup();
  return report("test_setup");
}
