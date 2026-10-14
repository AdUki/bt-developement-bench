// Setup: what btbenchd writes for bluetoothd (main.conf, --noplugin) and WirePlumber, from the
// profiles, plugins and settings the API takes — checked as text, without restarting anything.
#include <unistd.h>

#include <filesystem>
#include <set>
#include <fstream>
#include <sstream>

#include "check.h"
#include "mgmt.h"
#include "setup.h"
#include "util/spa_json.h"

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
  const std::string out = main_conf_merge(setup_schema(), base, {{"General.ControllerMode", "le"},
                                                {"LE.MinConnectionInterval", 7.5},
                                                {"LE.ConnectionSupervisionTimeout", 420},
                                                {"Policy.AutoEnable", false},
                                                {"GATT.Channels", 3},
                                                {"Monitor.bluez5.enable-msbc", true}});
  // A commented example gets the live line after it; the doc comment is not a key line.
  CHECK(has(out, "#ControllerMode = dual\n# set by btbenchd (Setup)\nControllerMode = le"));
  CHECK(has(out, "# Possible values: dual, bredr, le\n"));
  // A live line is replaced in place, ms turned into BlueZ's 1.25 ms units.
  CHECK(has(out, "# set by btbenchd (Setup)\nMinConnectionInterval = 6\n"));
  CHECK(!has(out, "MinConnectionInterval = 24"));
  // A missing key goes to the end of its section (10 ms units), a missing section to the end.
  CHECK(has(out, "ConnectionSupervisionTimeout = 42\n[Policy]"));
  // No value carries a trailing comment: a key file would take it as part of the value.
  {
    std::istringstream in(out);
    for (std::string l; std::getline(in, l);)
      if (!l.empty() && l[0] != '#' && l.find('=') != std::string::npos) CHECK(l.find('#') == std::string::npos);
  }
  CHECK(has(out, "AutoEnable = false"));
  CHECK(has(out, "\n[GATT]\n# set by btbenchd (Setup)\nChannels = 3"));
  // WirePlumber settings never reach main.conf.
  CHECK(!has(out, "msbc"));
  CHECK(has(out, "Name = btbench"));
}

void test_args_and_wireplumber() {
  CHECK_EQ(bluetoothd_setup_args({"hfp", "policy"}, ""), std::string("--noplugin=hfp,policy"));
  CHECK_EQ(bluetoothd_setup_args({}, "/data/btbench/main.conf"), std::string("--configfile=/data/btbench/main.conf"));
  CHECK_EQ(bluetoothd_setup_args({"hfp"}, "/x"), std::string("--noplugin=hfp --configfile=/x"));
  CHECK_EQ(bluetoothd_setup_args({}, ""), std::string(""));
  const std::string wp = wireplumber_roles_conf({"a2dp_sink", "bap_source", "hfp_hf", "hfp_ag"},
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
  CHECK(!has(wp, "bluez_input"));  // no node rule without a Node.* setting
  // Node.* settings: a rule for every Bluetooth node, after the device one in the same list.
  const std::string wn = wireplumber_roles_conf({"a2dp_sink"},
      {{"Node.bluez5.media-source-role", "input"}, {"Node.bluez5.decode-buffer.latency", 4096},
       {"Device.bluez5.auto-connect", json::array({"a2dp_sink", "hfp_hf"})}});
  CHECK(has(wn, "matches = [ { node.name = \"~bluez_input.*\" } { node.name = \"~bluez_output.*\" } ]"));
  CHECK(has(wn, "bluez5.media-source-role = \"input\"\n"));
  CHECK(has(wn, "bluez5.decode-buffer.latency = 4096\n"));
  CHECK(has(wn, "bluez5.auto-connect = [ a2dp_sink hfp_hf ]\n"));
  CHECK_EQ(wn.find("monitor.bluez.rules"), wn.rfind("monitor.bluez.rules"));
  CHECK_EQ(obexd_noplugin({"ftp", "pcsuite"}), std::string("ftp,pcsuite"));
}

void test_validation() {
  std::string err;
  const json sc = setup_schema();
  CHECK(setup_setting_ok(sc, "General.ControllerMode", "bredr", &err));
  CHECK(!setup_setting_ok(sc, "General.ControllerMode", "wifi", &err));
  CHECK(setup_setting_ok(sc, "LE.MinConnectionInterval", 7.5, &err));
  CHECK(!setup_setting_ok(sc, "LE.MinConnectionInterval", 5, &err));
  CHECK(!setup_setting_ok(sc, "LE.MinConnectionInterval", "7.5", &err));
  CHECK(setup_setting_ok(sc, "GATT.Cache", nullptr, &err));  // back to BlueZ's default
  CHECK(setup_setting_ok(sc, "Node.bluez5.media-source-role", "input", &err));
  CHECK(!setup_setting_ok(sc, "Node.bluez5.media-source-role", "speaker", &err));
  CHECK(setup_setting_ok(sc, "Device.bluez5.auto-connect", json::array({"a2dp_sink"}), &err));
  CHECK(!setup_setting_ok(sc, "Device.bluez5.auto-connect", json::array({"a2dp_speaker"}), &err));
  CHECK(!setup_setting_ok(sc, "General.Nonsense", true, &err));
  CHECK(setup_setting_ok(sc, "Monitor.bluez5.codecs", json::array({"sbc", "aac"}), &err));
  CHECK(!setup_setting_ok(sc, "Monitor.bluez5.codecs", json::array({"mp3"}), &err));
  CHECK(!setup_setting_ok(sc, "General.FastConnectable", 1, &err));
  CHECK(sc["profiles"].size() > 10);
  CHECK(sc["plugins"].size() > 20);
}

void test_spa_json() {
  const json j = spa_json_parse(
      "# a comment\n"
      "a.b = { x = 1, y: \"two # not a comment\" z = [ sbc \"ldac\" 3.5 ] }\n"
      "flag = true\n"
      "\"quoted key\" = null\n");
  CHECK(j.is_object());
  CHECK_EQ(j["a.b"]["x"].get<int>(), 1);
  CHECK_EQ(j["a.b"]["y"].get<std::string>(), std::string("two # not a comment"));
  CHECK_EQ(j["a.b"]["z"].dump(), std::string(R"(["sbc","ldac",3.5])"));
  CHECK_EQ(j["flag"].get<bool>(), true);
  CHECK(j["quoted key"].is_null());
  CHECK(spa_json_parse("a = { b = 1").is_discarded());
  const uint8_t iso[16] = {0x3e, 0xe0, 0xb4, 0xfd, 0xdd, 0xd6, 0x85, 0x98, 0x6a, 0x49, 0xe0, 0x05, 0x88, 0xf1, 0xba, 0x6f};
  CHECK_EQ(mgmt_uuid_str(iso), std::string("6fbaf188-05e0-496a-9885-d6ddfdb4e03e"));
  CHECK_EQ(mgmt_settings_names((1u << 7) | (1u << 9) | (1u << 18)).size(), 3u);
  CHECK_EQ(mgmt_settings_names(1u << 19)[0], std::string("cis-peripheral"));
}

const char* kIso = "6fbaf188-05e0-496a-9885-d6ddfdb4e03e";
const char* kSimult = "671b10b5-42c0-4696-9227-eb28d1b049d6";
const char* kBqr = "330859bc-7506-492d-9370-9a6f0614037f";
const char* kMesh = "2ce463d7-7a03-4d8d-bf05-5f24e8f36e76";

const std::string kMainConf =
    "[General]\n"
    "# Default adapter name\n"
    "#Name = BlueZ\n"
    "\n"
    "# Possible values: dual, bredr, le\n"
    "#ControllerMode = dual\n"
    "\n"
    "#JustWorksRepairing = never\n"
    "JustWorksRepairing = confirm\n"
    "\n"
    "# Enables kernel experimental features, alternatively a list of UUIDs\n"
    "# Possible UUIDS:\n"
    "# 671b10b5-42c0-4696-9227-eb28d1b049d6 (BlueZ Experimental Simultaneous Central and Peripheral)\n"
    "# 330859bc-7506-492d-9370-9a6f0614037f (BlueZ Experimental Bluetooth Quality Report)\n"
    "# 6fbaf188-05e0-496a-9885-d6ddfdb4e03e (BlueZ Experimental ISO socket)\n"
    "# Defaults to false.\n"
    "KernelExperimental = 6fbaf188-05e0-496a-9885-d6ddfdb4e03e\n"
    "\n"
    "[BR]\n"
    "# Shared by the keys below.\n"
    "#PageScanType=\n"
    "#PageScanInterval=\n"
    "\n"
    "[GATT]\n"
    "#ExchangeMTU = 517\n"
    "[AVDTP]\n"
    "# basic or ertm\n"
    "#SessionMode = basic\n";

const json* setting(const json& sc, const std::string& key) {
  for (const json& s : sc["settings"])
    if (s["key"] == key) return &s;
  return nullptr;
}
const json* option(const json& s, const std::string& v) {
  for (const json& o : s["options"])
    if (o["value"] == v) return &o;
  return nullptr;
}

// What the installed stack says about itself, from fixtures: main.conf's keys, the kernel's
// experimental features, bluetoothd's plugins, PipeWire's codec plugins, WirePlumber's schema.
void test_sources() {
  const fs::path dir = fs::temp_directory_path() / ("btb-sources-" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir / "spa" );
  fs::create_directories(dir / "wp.d");
  for (const char* lib : {"sbc", "aptx", "lc3", "hfp-cvsd", "hfp-msbc", "warp"})
    std::ofstream(dir / "spa" / ("libspa-codec-bluez5-" + std::string(lib) + ".so"));
  std::ofstream(dir / "spa" / "libspa-bluez5.so");
  std::ofstream(dir / "plugins") << "policy\na2dp\navrcp\nbap\nnewthing\n";
  std::ofstream(dir / "wireplumber.conf") << R"(
wireplumber.settings.schema = {
  bluetooth.autoswitch-to-headset-profile = { name = "Auto-switch" description = "d" type = "bool" default = true }
  bluetooth.profile-preference = { name = "Bluetooth profile preference" description = "quality or latency" type = "string" default = "quality" }
  bluetooth.level = { name = "Level" description = "" type = "float" default = 0.5 min = 0.0 max = 1.0 }
  device.restore-routes = { name = "Restore routes" type = "bool" default = true }
}
monitor.bluez.properties = { }
)";
  std::ofstream(dir / "wp.d" / "50-image.conf") << R"(
monitor.bluez.properties = { bluez5.codecs = [ sbc aptx lc3 ] bluez5.default.rate = 48000 }
wireplumber.settings = { bluetooth.autoswitch-to-headset-profile = false }
)";
  std::ofstream(dir / "wp.d" / "60-own.conf") << "wireplumber.settings = { bluetooth.autoswitch-to-headset-profile = true }\n";
  SetupSources src;
  src.bluez_plugin_list = (dir / "plugins").string();
  src.spa_bluez5_dir = (dir / "spa").string();
  src.wireplumber_conf = (dir / "wireplumber.conf").string();
  src.wireplumber_fragment_dirs = {(dir / "wp.d").string()};
  src.wireplumber_own = (dir / "wp.d" / "60-own.conf").string();
  src.controllers = [](json* out, std::string*) {
    *out = json{{"controllers", json::array({
                    json{{"index", 0}, {"address", "44:A3:BB:36:5E:2E"}, {"supported", {"br/edr", "le", "cis-central", "cis-peripheral"}},
                         {"experimental", json::array({json{{"uuid", kSimult}, {"flags", 0}}, json{{"uuid", kIso}, {"flags", 1}},
                                                       json{{"uuid", kMesh}, {"flags", 0}}})}},
                    json{{"index", 1}, {"address", "10:2C:6B:A2:0A:0F"}, {"supported", {"br/edr", "le"}},
                         {"experimental", json::array({json{{"uuid", kIso}, {"flags", 1}}})}}})},
                {"experimental", json::array({json{{"uuid", kIso}, {"flags", 1}}})}};
    return true;
  };
  const json sc = setup_schema(src, kMainConf);
  std::string err;

  // main.conf: the curated keys get BlueZ's default and the image's value; the others are text.
  const json* jw = setting(sc, "General.JustWorksRepairing");
  CHECK_EQ((*jw)["default"].get<std::string>(), std::string("never"));
  CHECK_EQ((*jw)["image"].get<std::string>(), std::string("confirm"));
  CHECK_EQ((*setting(sc, "General.ControllerMode"))["default"].get<std::string>(), std::string("dual"));
  CHECK_EQ((*setting(sc, "GATT.ExchangeMTU"))["default"].get<double>(), 517.0);
  const json* name = setting(sc, "General.Name");
  CHECK(name && (*name)["type"] == "text" && (*name)["more"] == true);
  CHECK_EQ((*name)["help"].get<std::string>(), std::string("Default adapter name"));
  CHECK_EQ((*setting(sc, "BR.PageScanInterval"))["help"].get<std::string>(), std::string("Shared by the keys below."));
  CHECK(setting(sc, "AVDTP.SessionMode"));
  CHECK(setup_setting_ok(sc, "AVDTP.SessionMode", "ertm", &err));
  CHECK(!setup_setting_ok(sc, "AVDTP.SessionMode", "a\nb", &err));

  // Kernel experimental features: what bluetoothd takes (main.conf), where the kernel offers them.
  const json* ke = setting(sc, "General.KernelExperimental");
  CHECK_EQ((*ke)["type"].get<std::string>(), std::string("list"));
  CHECK_EQ((*option(*ke, kIso))["label"].get<std::string>(), std::string("ISO socket"));
  CHECK(has((*option(*ke, kIso))["note"].get<std::string>(), "global, hci0, hci1"));
  CHECK(has((*option(*ke, kBqr))["note"].get<std::string>(), "not offered"));
  CHECK_EQ((*option(*ke, kMesh))["disabled"].get<bool>(), true);  // the kernel's, not bluetoothd's
  CHECK_EQ((*ke)["image"].dump(), json::array({kIso}).dump());
  CHECK(setup_setting_ok(sc, "General.KernelExperimental", json::array({kIso, kSimult}), &err));
  CHECK(!setup_setting_ok(sc, "General.KernelExperimental", json::array({kMesh}), &err));
  const std::string conf = main_conf_merge(sc, kMainConf, {{"General.KernelExperimental", json::array({kSimult, kIso})}});
  CHECK(has(conf, std::string("KernelExperimental = ") + kSimult + "," + kIso + "\n"));
  CHECK(has(main_conf_merge(sc, kMainConf, {{"General.KernelExperimental", json::array()}}), "KernelExperimental = false\n"));

  // Plugins: the build's list; a table plugin the build lacks is gone, one the table lacks added.
  std::set<std::string> plugins;
  for (const json& p : sc["plugins"]) plugins.insert(p["name"].get<std::string>());
  CHECK(plugins.count("policy") && plugins.count("newthing") && !plugins.count("hog"));
  for (const json& p : sc["profiles"]) {
    if (p["id"] == "hog_host") CHECK_EQ(p["available"].get<bool>(), false);
    if (p["id"] == "avrcp") CHECK(!p.contains("available"));
    // LE Audio: hci0 has CIS, nobody has broadcast.
    if (p["id"] == "bap_source") CHECK(!p.contains("note"));
    if (p["id"] == "bap_bcast_sink") CHECK(has(p["note"].get<std::string>(), "sync-receiver"));
  }

  // Codecs: those without their plugin are missing, a plugin the table does not know is told.
  const json* codecs = setting(sc, "Monitor.bluez5.codecs");
  CHECK(!option(*codecs, "aptx_hd")->contains("missing"));
  CHECK_EQ((*option(*codecs, "ldac"))["missing"].get<bool>(), true);
  CHECK(has((*codecs)["note"].get<std::string>(), "libspa-codec-bluez5-warp.so"));
  CHECK_EQ((*setting(sc, "Device.bluez5.a2dp.ldac.quality"))["available"].get<bool>(), false);
  CHECK(!setting(sc, "Monitor.bluez5.enable-msbc")->contains("available"));
  CHECK(setup_setting_ok(sc, "Monitor.bluez5.codecs", json::array({"ldac"}), &err));  // kept if set before
  // The image's fragments (not btbenchd's own): its codecs and settings.
  CHECK_EQ((*codecs)["image"].dump(), std::string(R"(["sbc","aptx","lc3"])"));
  CHECK_EQ((*setting(sc, "Monitor.bluez5.default.rate"))["image"].get<std::string>(), std::string("48000"));

  // WirePlumber's schema: the Bluetooth ones, curated or new, with their defaults.
  const json* as = setting(sc, "Settings.bluetooth.autoswitch-to-headset-profile");
  CHECK_EQ((*as)["default"].get<bool>(), true);
  CHECK_EQ((*as)["image"].get<bool>(), false);
  const json* pref = setting(sc, "Settings.bluetooth.profile-preference");
  CHECK(pref && (*pref)["type"] == "text" && (*pref)["default"] == "quality");
  CHECK_EQ((*setting(sc, "Settings.bluetooth.level"))["max"].get<double>(), 1.0);
  CHECK(!setting(sc, "Settings.device.restore-routes"));
  CHECK(setup_setting_ok(sc, "Settings.bluetooth.profile-preference", "latency", &err));
  CHECK(has(wireplumber_roles_conf({}, {{"Settings.bluetooth.profile-preference", "latency"}}), "bluetooth.profile-preference = \"latency\""));

  // Without the sources: the tables as built.
  const json bare = setup_schema();
  CHECK(bare["controllers"].is_null());
  CHECK((*setting(bare, "General.KernelExperimental"))["options"].empty());
  fs::remove_all(dir);
}

void test_setup() {
  const fs::path dir = fs::temp_directory_path() / ("btb-setup-" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "main.conf.base") << kMainConf;
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
    // The default: everything on but BlueZ's own HFP, HSP and the remote.
    const std::set<std::string> off = {"bluez_hfp", "hid_remote", "hsp_hs", "hsp_ag"};
    for (const json& p : st["profiles"]) CHECK_EQ(p["on"].get<bool>(), !off.count(p["id"].get<std::string>()));
    json out;
    std::string err;
    int status = 0;
    CHECK(s.change(json::parse(R"({"profiles": {"a2dp_source": false, "obex_ftp": false, "bap_sink": false, "bap_source": false, "bap_bcast_source": false, "hid_remote": true},
        "plugins": {"policy": false, "bap": false, "bass": false, "vcp": false, "micp": false, "mcp": false, "ccp": false, "csip": false, "tmap": false, "gmap": false}, "settings": {"General.ControllerMode": "le", "Monitor.bluez5.enable-msbc": false}})"), &out, &err, &status));
    CHECK(err.empty());
    CHECK(remote);
    CHECK_EQ(obex_got, std::string("ftp,pcsuite"));
    CHECK_EQ(out["pending"].get<bool>(), true);  // written, not applied (apply is off)
    const std::string env = slurp(dir / "bluetoothd.env");
    CHECK(has(env, "BLUETOOTHD_ARGS=\"-d\""));  // the System page's line is kept
    CHECK(has(env, "--noplugin=bap,bass,ccp,csip,gmap,hfp,mcp,micp,policy,tmap,vcp"));
    CHECK(has(env, "--configfile=" + (dir / "main.conf").string()));
    CHECK(has(slurp(dir / "main.conf"), "ControllerMode = le"));
    CHECK(has(slurp(dir / "wp.conf"), "override.bluez5.roles = [ a2dp_sink bap_bcast_sink asha_sink hfp_hf hfp_ag ]"));
    CHECK(has(slurp(dir / "wp.conf"), "bluez5.enable-msbc = false"));
    for (const json& p : out["profiles"]) {
      if (p["id"] == "a2dp_source" || p["id"] == "bap_sink" || p["id"] == "bap_bcast_source") CHECK_EQ(p["on"].get<bool>(), false);
      if (p["id"] == "a2dp_sink" || p["id"] == "hid_remote" || p["id"] == "bap_bcast_sink") CHECK_EQ(p["on"].get<bool>(), true);
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
  // A file from before KernelExperimental was a list: on is every feature bluetoothd takes.
  std::ofstream(dir / "setup.json") << R"({"settings": {"General.KernelExperimental": true}})";
  for (const json& st : Setup(o, nullptr, nullptr, nullptr).state()["settings"])
    if (st["key"] == "General.KernelExperimental") CHECK_EQ(st["value"].size(), 3u);
  // A file from before roles_on: its roles off become the roles on, the new roles stay off.
  std::ofstream(dir / "setup.json") << R"({"plugins_off": ["hfp"], "roles_off": ["hfp_ag"], "obex_off": [], "settings": {}})";
  Setup old(o, nullptr, nullptr, nullptr);
  for (const json& p : old.state()["profiles"]) {
    if (p["id"] == "hfp_ag" || p["id"] == "bap_bcast_source") CHECK_EQ(p["on"].get<bool>(), false);
    if (p["id"] == "bap_sink") CHECK_EQ(p["on"].get<bool>(), true);
  }
  fs::remove_all(dir);
}

}  // namespace

int main() {
  test_merge();
  test_args_and_wireplumber();
  test_validation();
  test_spa_json();
  test_sources();
  test_setup();
  return report("test_setup");
}
