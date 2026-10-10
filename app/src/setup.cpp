#include "setup.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "system.h"
#include "util/exec.h"
#include "util/log.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb {

namespace {

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool put_file(const std::string& path, const std::string& text, std::string* err) {
  const std::string tmp = path + ".new";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << text;
    f.flush();
    if (!f) {
      *err = "cannot write " + tmp;
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    *err = "cannot replace " + path;
    return false;
  }
  return true;
}

// ---- the tables ------------------------------------------------------------------------------------

struct Setting {
  // Section.Key in main.conf; for WirePlumber, Monitor.<prop> (monitor.bluez.properties),
  // Device.<prop> (a monitor.bluez.rules update for every Bluetooth device) or Settings.<name>
  // (wireplumber.settings).
  const char* key;
  const char* label;
  const char* help;
  const char* type;   // enum | bool | number | list (a set of the options)
  std::vector<std::pair<const char*, const char*>> options;  // enum: value, label
  double min = 0, max = 0;
  const char* unit = "";  // what the API takes
  double scale = 1;       // BlueZ's unit in the API's: raw = value / scale
};

const std::vector<Setting>& settings_table() {
  static const std::vector<Setting> t = {
      {"General.ControllerMode", "Radio", "Which radios the adapter runs. LE only takes away A2DP, HFP, classic HID and OBEX; classic only takes away BLE, LE Audio and the HID remote.",
       "enum", {{"dual", "Classic and LE"}, {"bredr", "Classic (BR/EDR) only"}, {"le", "LE only"}}},
      {"General.SecureConnections", "Secure Connections", "LE Secure Connections and BR/EDR Secure Connections pairing. Only refuses devices that cannot do it.",
       "enum", {{"on", "On"}, {"off", "Off (legacy pairing)"}, {"only", "Only (Secure Connections Only mode)"}}},
      {"General.Privacy", "Privacy", "Resolvable private addresses for the board. device: a new address for everyone; network: only towards unknown devices.",
       "enum", {{"off", "Off"}, {"device", "Device"}, {"limited-device", "Limited device"}, {"network", "Network"}, {"limited-network", "Limited network"}}},
      {"General.JustWorksRepairing", "Re-pairing without confirmation", "What happens when a bonded device pairs again with Just Works.",
       "enum", {{"never", "Refuse"}, {"confirm", "Ask the agent"}, {"always", "Accept"}}},
      {"General.FastConnectable", "Fast connectable", "Faster page scanning (classic): devices connect sooner, the radio uses more power. BlueZ only ever switches it on: back to Default or Off, it stays on until the adapter is powered off and on (or the board reboots).", "bool", {}},
      {"General.AlwaysPairable", "Always pairable", "Pairable without a timeout, even with no agent asked.", "bool", {}},
      {"General.NameResolving", "Resolve names", "Ask discovered classic devices for their names.", "bool", {}},
      {"General.Experimental", "Experimental D-Bus interfaces", "BlueZ's experimental interfaces (some LE Audio, Bearer).", "bool", {}},
      {"General.KernelExperimental", "Kernel experimental features", "The kernel's experimental Bluetooth features (ISO sockets, LL privacy, …).", "bool", {}},
      {"BR.PageTimeout", "Page timeout", "How long a classic connection attempt pages before it gives up.", "number", {}, 1, 40959, "ms", 0.625},
      {"BR.LinkSupervisionTimeout", "Link supervision timeout", "Classic: how long a silent link lives before it is dropped.", "number", {}, 1, 40959, "ms", 0.625},
      {"LE.MinConnectionInterval", "Connection interval, min", "The shortest LE connection interval the board asks for as central.", "number", {}, 7.5, 4000, "ms", 1.25},
      {"LE.MaxConnectionInterval", "Connection interval, max", "The longest LE connection interval the board asks for as central.", "number", {}, 7.5, 4000, "ms", 1.25},
      {"LE.ConnectionLatency", "Peripheral latency", "Connection events a peripheral may skip.", "number", {}, 0, 499, "events", 1},
      {"LE.ConnectionSupervisionTimeout", "Supervision timeout", "How long a silent LE link lives before it is dropped.", "number", {}, 100, 32000, "ms", 10},
      {"LE.MinAdvertisementInterval", "Advertising interval, min", "For BlueZ's own and the API's advertisements without an interval of their own.", "number", {}, 20, 10240, "ms", 0.625},
      {"LE.MaxAdvertisementInterval", "Advertising interval, max", "", "number", {}, 20, 10240, "ms", 0.625},
      {"GATT.Cache", "Remote database cache", "Whether BlueZ keeps what it discovered of a device's GATT database.",
       "enum", {{"always", "Always"}, {"yes", "Bonded devices"}, {"no", "Never"}}},
      {"GATT.KeySize", "Minimum key size", "The encryption key size BlueZ accepts (0: any, 7..16 bytes).", "number", {}, 0, 16, "bytes", 1},
      {"GATT.ExchangeMTU", "ATT MTU", "The MTU BlueZ offers as GATT client.", "number", {}, 23, 517, "bytes", 1},
      {"GATT.Channels", "ATT channels (EATT)", "1 is plain ATT; more opens Enhanced ATT bearers.", "number", {}, 1, 6, "channels", 1},
      {"GATT.Client", "GATT client", "BlueZ's GATT client: discovering the services of connected devices.", "bool", {}},
      {"Policy.ReconnectAttempts", "Reconnect attempts", "How often the policy plugin reconnects a device that dropped (0: never).", "number", {}, 0, 100, "attempts", 1},
      {"Policy.AutoEnable", "Power on adapters", "The policy plugin powers adapters on when they appear.", "bool", {}},
      {"Policy.ResumeDelay", "Resume delay", "After a system resume, wait this long before reconnecting.", "number", {}, 0, 60, "s", 1},
      // WirePlumber (PipeWire's Bluetooth): written to the profiles fragment, not main.conf.
      {"Monitor.bluez5.codecs", "Codecs offered", "The codecs PipeWire registers and offers; one missing is never used. Those this build lacks are ignored.",
       "list", {{"sbc", "SBC"}, {"sbc_xq", "SBC-XQ"}, {"aac", "AAC"}, {"aac_eld", "AAC-ELD"}, {"aptx", "aptX"}, {"aptx_hd", "aptX HD"},
                {"aptx_ll", "aptX LL"}, {"aptx_ll_duplex", "aptX LL duplex"}, {"ldac", "LDAC"}, {"faststream", "FastStream"},
                {"faststream_duplex", "FastStream duplex"}, {"lc3plus_hr", "LC3plus HR"}, {"opus_05", "Opus"}, {"opus_05_51", "Opus 5.1"},
                {"opus_05_71", "Opus 7.1"}, {"opus_05_duplex", "Opus duplex"}, {"opus_05_pro", "Opus Pro"}, {"opus_g", "Opus G"},
                {"lc3", "LC3 (LE Audio)"}, {"cvsd", "CVSD (HFP)"}, {"msbc", "mSBC (HFP wideband)"}, {"lc3_swb", "LC3-SWB (HFP)"},
                {"lc3_a127", "LC3 A127 (HFP)"}, {"g722", "G.722 (ASHA)"}}},
      {"Monitor.bluez5.enable-sbc-xq", "SBC-XQ", "Offer SBC at high bitpool (SBC-XQ) to devices that take it.", "bool", {}},
      {"Monitor.bluez5.enable-msbc", "mSBC", "Wideband speech on HFP.", "bool", {}},
      {"Monitor.bluez5.enable-hw-volume", "Hardware volume", "Use the device's own volume (AVRCP absolute volume, HFP gain) instead of scaling samples.", "bool", {}},
      {"Monitor.bluez5.hfphsp-backend", "HFP/HSP backend", "Who does HFP: PipeWire itself (native), oFono, or nobody.",
       "enum", {{"native", "PipeWire (native)"}, {"ofono", "oFono"}, {"none", "None"}}},
      {"Monitor.bluez5.default.rate", "Default rate", "The sample rate Bluetooth nodes run at.", "enum", {{"44100", "44.1 kHz"}, {"48000", "48 kHz"}}},
      {"Device.bluez5.a2dp.ldac.quality", "LDAC quality", "LDAC's bitrate: adaptive, or fixed high (990), standard (660), mobile (330 kbit/s).",
       "enum", {{"auto", "Adaptive"}, {"hq", "High"}, {"sq", "Standard"}, {"mq", "Mobile"}}},
      {"Device.bluez5.a2dp.aac.bitratemode", "AAC bitrate mode", "0: constant bitrate; 1..5: variable, from lowest to highest quality.", "number", {}, 0, 5, "", 1},
      {"Settings.bluetooth.autoswitch-to-headset-profile", "Switch to headset when recording", "A desktop's behaviour: recording from a Bluetooth source turns the device to HFP. Off on the bench (it would break A2DP measurements).", "bool", {}},
      {"Settings.bluetooth.use-persistent-storage", "Remember profiles", "Keep each device's last profile (codec) across reconnects.", "bool", {}},
  };
  return t;
}

struct Plugin {
  const char* name;
  const char* label;
  const char* help;
};

// BlueZ 5.87's built-in plugins (BLUETOOTH_PLUGIN_DEFINE in its tree).
const std::vector<Plugin>& plugins_table() {
  static const std::vector<Plugin> t = {
      {"policy", "Policy", "Connects a device's other profiles when one connects, reconnects devices that dropped, powers adapters on. Off: only what is asked for connects."},
      {"autopair", "Auto-pair", "Tries the usual fixed PINs (0000, 1234) for legacy devices without a keypad."},
      {"a2dp", "A2DP", "Stream endpoints for PipeWire (needed for any A2DP music)."},
      {"avrcp", "AVRCP", "Remote control, track info, absolute volume, cover art."},
      {"hfp", "BlueZ HFP", "BlueZ's own Hands-Free: fights PipeWire's native HFP for the same role."},
      {"input", "HID (classic)", "Classic Bluetooth keyboards, mice, game pads."},
      {"hog", "HID over GATT", "BLE keyboards, mice and remotes."},
      {"scanparam", "Scan Parameters", "The Scan Parameters service HID over GATT devices ask for."},
      {"network", "PAN", "Bluetooth networking (PANU, NAP, GN)."},
      {"battery", "Battery", "Reads devices' battery level (Battery Service)."},
      {"deviceinfo", "Device Information", "Reads devices' PnP ID (Device Information Service)."},
      {"gap", "GAP client", "Reads devices' name and appearance over GATT."},
      {"hostname", "Hostname", "Names the adapter after the hostname."},
      {"bap", "BAP", "LE Audio streams (Basic Audio Profile)."},
      {"bass", "BASS", "LE Audio broadcast assistant."},
      {"vcp", "VCP", "LE Audio volume control."},
      {"micp", "MICP", "LE Audio microphone control."},
      {"mcp", "MCP", "LE Audio media control."},
      {"ccp", "CCP", "LE Audio call control."},
      {"csip", "CSIP", "Coordinated sets (earbud pairs)."},
      {"tmap", "TMAP", "Telephony and Media Audio Profile."},
      {"gmap", "GMAP", "Gaming Audio Profile."},
      {"asha", "ASHA", "Audio Streaming for Hearing Aids."},
      {"fastpair", "Fast Pair", "Google Fast Pair."},
      {"rap", "RAP", "Ranging (channel sounding)."},
  };
  return t;
}

// Every Bluetooth role PipeWire's bluez5 monitor implements (bluez5.roles); the image turns on all
// but HSP and the LE Audio broadcast roles (wireplumber 50-btbench.conf).
const std::vector<std::string>& roles_table() {
  static const std::vector<std::string> t = {"a2dp_sink", "a2dp_source", "bap_sink", "bap_source", "bap_bcast_sink",
                                             "bap_bcast_source", "hfp_hf", "hfp_ag", "hsp_hs", "hsp_ag"};
  return t;
}
// The roles setup.json files written before it kept roles_on knew (they kept the roles off).
const std::vector<std::string> kRolesV1 = {"a2dp_sink", "a2dp_source", "bap_sink", "bap_source", "hfp_hf", "hfp_ag"};

struct Profile {
  const char* id;
  const char* label;
  const char* group;
  const char* help;
  std::vector<std::string> plugins, roles, obex;
  bool remote = false;
};

const std::vector<Profile>& profiles_table() {
  static const std::vector<Profile> t = {
      {"a2dp_sink", "A2DP sink", "Audio", "Receive music: the board as a speaker for phones.", {}, {"a2dp_sink"}, {}},
      {"a2dp_source", "A2DP source", "Audio", "Send music to headphones and speakers.", {}, {"a2dp_source"}, {}},
      {"hfp_hf", "Hands-Free (HFP HF)", "Audio", "Be a headset in a phone's calls.", {}, {"hfp_hf"}, {}},
      {"hfp_ag", "Audio Gateway (HFP AG)", "Audio", "Be the phone for headsets.", {}, {"hfp_ag"}, {}},
      {"hsp_hs", "Headset (HSP HS)", "Audio", "The older headset profile, headset side. Off by default: some headsets misbehave with HSP and HFP offered together.", {}, {"hsp_hs"}, {}},
      {"hsp_ag", "Headset gateway (HSP AG)", "Audio", "The older headset profile, phone side.", {}, {"hsp_ag"}, {}},
      {"bap_sink", "LE Audio unicast sink", "LE Audio", "Receive LE Audio (BAP unicast server, sink).", {}, {"bap_sink"}, {}},
      {"bap_source", "LE Audio unicast source", "LE Audio", "Send LE Audio to earbuds (BAP unicast client, source).", {}, {"bap_source"}, {}},
      {"bap_bcast_sink", "LE Audio broadcast sink", "LE Audio", "Listen to Auracast broadcasts (BAP broadcast sink). Needs ISO: a controller with LE Audio.", {}, {"bap_bcast_sink"}, {}},
      {"bap_bcast_source", "LE Audio broadcast source", "LE Audio", "Broadcast audio for anyone to hear (BAP broadcast source). Needs ISO.", {}, {"bap_bcast_source"}, {}},
      {"avrcp", "AVRCP", "Control", "Players, track info, absolute volume and cover art.", {"avrcp"}, {}, {}},
      {"hid_host", "HID host (classic)", "Input", "Use classic keyboards, mice and game pads.", {"input"}, {}, {}},
      {"hog_host", "HID over GATT host", "Input", "Use BLE keyboards, mice and remotes.", {"hog", "scanparam"}, {}, {}},
      {"hid_remote", "HID remote (HoGP device)", "Input", "Be a BLE media remote for phones, TVs and computers.", {}, {}, {}, true},
      {"pan", "PAN", "Network", "Bluetooth networking.", {"network"}, {}, {}},
      {"obex_opp", "Object Push server", "Files", "Devices send files to the board.", {}, {}, {"opp"}},
      {"obex_ftp", "File Transfer server", "Files", "Devices browse the board's files.", {}, {}, {"ftp", "pcsuite"}},
      {"obex_pbap", "Phonebook server", "Files", "The board offers a phonebook (PBAP PSE).", {}, {}, {"pbap"}},
      {"obex_map", "Message Access server", "Files", "The board offers messages (MAP MSE).", {}, {}, {"mas"}},
      {"battery", "Battery levels", "Device info", "Show connected devices' battery level.", {"battery"}, {}, {}},
      {"bluez_hfp", "BlueZ's own HFP", "Audio", "BlueZ's HFP instead of PipeWire's: turn PipeWire's HFP roles off with it.", {"hfp"}, {}, {}},
  };
  return t;
}

bool for_wireplumber(const std::string& key) {
  return starts_with(key, "Monitor.") || starts_with(key, "Device.") || starts_with(key, "Settings.");
}

std::string format_number(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

// A value in SPA-JSON (WirePlumber's config syntax).
std::string spa_value(const json& v) {
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number()) return format_number(v.get<double>());
  if (v.is_array()) {
    std::string out = "[ ";
    for (const json& x : v) out += x.get<std::string>() + " ";
    return out + "]";
  }
  const std::string t = v.get<std::string>();
  // Numbers given as an enum ("48000") stay numbers.
  if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) return t;
  return "\"" + t + "\"";
}

const Setting* find_setting(const std::string& key) {
  for (const Setting& s : settings_table())
    if (key == s.key) return &s;
  return nullptr;
}

bool known_plugin(const std::string& n) {
  return std::any_of(plugins_table().begin(), plugins_table().end(), [&](const Plugin& p) { return n == p.name; });
}

// An API value as BlueZ writes it.
std::string conf_value(const Setting& s, const json& v) {
  if (std::string(s.type) == "bool") return v.get<bool>() ? "true" : "false";
  if (std::string(s.type) == "enum") return v.get<std::string>();
  const double raw = std::round(v.get<double>() / s.scale);
  return std::to_string(static_cast<long>(raw));
}

}  // namespace

json setup_schema() {
  json settings = json::array();
  for (const Setting& s : settings_table()) {
    json j{{"key", s.key}, {"label", s.label}, {"help", s.help}, {"type", s.type},
           {"file", for_wireplumber(s.key) ? "wireplumber" : "main.conf"}};
    if (!s.options.empty()) {
      json o = json::array();
      for (const auto& [v, l] : s.options) o.push_back(json{{"value", v}, {"label", l}});
      j["options"] = o;
    }
    if (std::string(s.type) == "number") j.update(json{{"min", s.min}, {"max", s.max}, {"unit", s.unit}, {"step", s.scale}});
    settings.push_back(std::move(j));
  }
  json plugins = json::array();
  for (const Plugin& p : plugins_table()) plugins.push_back(json{{"name", p.name}, {"label", p.label}, {"help", p.help}});
  json profiles = json::array();
  for (const Profile& p : profiles_table()) {
    json layers = json::array();
    if (!p.plugins.empty()) layers.push_back("bluetoothd");
    if (!p.roles.empty()) layers.push_back("pipewire");
    if (!p.obex.empty()) layers.push_back("obexd");
    if (p.remote) layers.push_back("btbenchd");
    profiles.push_back(json{{"id", p.id}, {"label", p.label}, {"group", p.group}, {"help", p.help}, {"layers", layers},
                            {"plugins", p.plugins}, {"roles", p.roles}, {"obex", p.obex}});
  }
  return json{{"settings", settings}, {"plugins", plugins}, {"profiles", profiles}, {"roles", roles_table()}};
}

bool setup_setting_ok(const std::string& key, const json& v, std::string* err) {
  const Setting* s = find_setting(key);
  if (!s) {
    *err = "no setting " + key;
    return false;
  }
  if (v.is_null()) return true;
  const std::string type = s->type;
  if (type == "bool" && !v.is_boolean()) {
    *err = key + " is true or false";
    return false;
  }
  if (type == "enum") {
    if (!v.is_string() || std::none_of(s->options.begin(), s->options.end(), [&](const auto& o) { return v.get<std::string>() == o.first; })) {
      *err = key + " is one of its options";
      return false;
    }
  }
  if (type == "list") {
    if (!v.is_array()) {
      *err = key + " is a list";
      return false;
    }
    for (const json& x : v)
      if (!x.is_string() || std::none_of(s->options.begin(), s->options.end(), [&](const auto& o) { return x.get<std::string>() == o.first; })) {
        *err = key + ": not one of its options";
        return false;
      }
  }
  if (type == "number") {
    if (!v.is_number() || v.get<double>() < s->min || v.get<double>() > s->max) {
      *err = key + " is " + format_number(s->min) + ".." + format_number(s->max) + " " + s->unit;
      return false;
    }
  }
  return true;
}

std::string main_conf_merge(const std::string& base, const std::map<std::string, json>& settings) {
  std::vector<std::string> lines;
  {
    std::istringstream in(base);
    for (std::string l; std::getline(in, l);) lines.push_back(l);
  }
  auto section_of = [](const std::string& l) -> std::string {
    const std::string t = trim(l);
    return t.size() > 2 && t.front() == '[' && t.back() == ']' ? t.substr(1, t.size() - 2) : "";
  };
  // "Key =", "Key=", "#Key = x", "# Key = x": the key's line, live or a commented example.
  auto is_key_line = [](const std::string& l, const std::string& key) {
    std::string t = trim(l);
    if (!t.empty() && t[0] == '#') t = trim(t.substr(1));
    if (t.compare(0, key.size(), key) != 0) return false;
    const std::string rest = trim(t.substr(key.size()));
    return !rest.empty() && rest[0] == '=';
  };
  for (const auto& [full, v] : settings) {
    if (v.is_null() || for_wireplumber(full)) continue;
    const Setting* s = find_setting(full);
    if (!s) continue;
    const std::string sec = full.substr(0, full.find('.'));
    const std::string key = full.substr(full.find('.') + 1);
    // The marker on a line of its own: GLib key files have no trailing comments, and
    // "FastConnectable = true  # ..." would be read as the value "true  # ...", which BlueZ
    // rejects and replaces with its default.
    const std::string line = "# set by btbenchd (Setup)\n" + key + " = " + conf_value(*s, v);
    std::string cur;
    int sec_start = -1, sec_end = -1, hit = -1, live = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string h = section_of(lines[i]);
      if (!h.empty()) {
        if (cur == sec && sec_end < 0) sec_end = static_cast<int>(i);
        cur = h;
        if (h == sec && sec_start < 0) sec_start = static_cast<int>(i);
        continue;
      }
      if (cur != sec || !is_key_line(lines[i], key)) continue;
      // A live line wins over a commented example; the first of each.
      if (trim(lines[i])[0] != '#' && live < 0) live = static_cast<int>(i);
      else if (hit < 0) hit = static_cast<int>(i);
    }
    if (cur == sec && sec_start >= 0 && sec_end < 0) sec_end = static_cast<int>(lines.size());
    if (live >= 0) lines[static_cast<size_t>(live)] = line;
    else if (hit >= 0) lines.insert(lines.begin() + hit + 1, line);
    else if (sec_start >= 0) lines.insert(lines.begin() + sec_end, line);
    else {
      lines.push_back("");
      lines.push_back("[" + sec + "]");
      lines.push_back(line);
    }
  }
  std::string out = "# Written by btbenchd (Setup) from the image's main.conf; edits here are replaced.\n";
  for (const std::string& l : lines) out += l + "\n";
  return out;
}

std::string bluetoothd_setup_args(const std::set<std::string>& plugins_off, const std::string& config_file) {
  std::string out;
  if (!plugins_off.empty()) {
    out = "--noplugin=";
    bool first = true;
    for (const std::string& p : plugins_off) {
      out += (first ? "" : ",") + p;
      first = false;
    }
  }
  if (!config_file.empty()) out += (out.empty() ? "" : " ") + std::string("--configfile=") + config_file;
  return out;
}

std::string wireplumber_roles_conf(const std::set<std::string>& roles_on, const std::map<std::string, json>& settings) {
  std::string roles;
  for (const std::string& r : roles_table())
    if (roles_on.count(r)) roles += r + " ";
  std::string monitor, device, wps;
  for (const auto& [k, v] : settings) {
    if (v.is_null()) continue;
    const std::string prop = k.substr(k.find('.') + 1);
    // A list replaces the image's (fragments would otherwise add to it).
    if (starts_with(k, "Monitor.")) monitor += "  " + std::string(v.is_array() ? "override." : "") + prop + " = " + spa_value(v) + "\n";
    else if (starts_with(k, "Device.")) device += "        " + prop + " = " + spa_value(v) + "\n";
    else if (starts_with(k, "Settings.")) wps += "  " + prop + " = " + spa_value(v) + "\n";
  }
  // "override." replaces the roles list instead of adding to the image's.
  std::string out = "# Written by btbenchd (Setup): the Bluetooth roles PipeWire registers, and the settings\n"
                    "# changed from the image's 50-btbench.conf. Edits here are replaced.\n"
                    "monitor.bluez.properties = {\n"
                    "  override.bluez5.roles = [ " + roles + "]\n" + monitor + "}\n";
  if (!device.empty())
    out += "monitor.bluez.rules = [\n  {\n    matches = [ { device.name = \"~bluez_card.*\" } ]\n"
           "    actions = {\n      update-props = {\n" + device + "      }\n    }\n  }\n]\n";
  if (!wps.empty()) out += "wireplumber.settings = {\n" + wps + "}\n";
  return out;
}

std::string obexd_noplugin(const std::set<std::string>& off) {
  std::string out;
  for (const std::string& p : off) out += (out.empty() ? "" : ",") + p;
  return out;
}

// ---- the service ----------------------------------------------------------------------------------

Setup::Setup(SetupOptions opt, RemoteState remote_on, RemoteSet remote_set, ObexApply obex_apply)
    : opt_(std::move(opt)), remote_on_(std::move(remote_on)), remote_set_(std::move(remote_set)), obex_apply_(std::move(obex_apply)) {
  load();
}

bool Setup::load() {
  const json j = json::parse(slurp(opt_.data_dir + "/setup.json"), nullptr, false);
  if (!j.is_object()) return false;
  auto set_of = [&](const char* k, std::set<std::string>* out) {
    if (!j.contains(k) || !j[k].is_array()) return;
    out->clear();
    for (const json& x : j[k])
      if (x.is_string()) out->insert(x.get<std::string>());
  };
  set_of("plugins_off", &st_.plugins_off);
  if (j.contains("roles_on")) {
    set_of("roles_on", &st_.roles_on);
  } else if (j.contains("roles_off")) {
    // An older file: the roles it knew, less those it had off.
    std::set<std::string> off;
    set_of("roles_off", &off);
    st_.roles_on.clear();
    for (const std::string& r : kRolesV1)
      if (!off.count(r)) st_.roles_on.insert(r);
  }
  set_of("obex_off", &st_.obex_off);
  if (j.contains("settings") && j["settings"].is_object())
    for (const auto& [k, v] : j["settings"].items()) {
      std::string e;
      if (setup_setting_ok(k, v, &e) && !v.is_null()) st_.settings[k] = v;
    }
  return true;
}

bool Setup::save(std::string* err) const {
  mkdir(opt_.data_dir.c_str(), 0755);
  json settings = json::object();
  for (const auto& [k, v] : st_.settings) settings[k] = v;
  const json j{{"plugins_off", st_.plugins_off}, {"roles_on", st_.roles_on}, {"obex_off", st_.obex_off}, {"settings", settings}};
  return put_file(opt_.data_dir + "/setup.json", j.dump(2) + "\n", err);
}

std::string Setup::obex_noplugin() const {
  std::lock_guard<std::mutex> lk(m_);
  return obexd_noplugin(st_.obex_off);
}

bool Setup::write_files(bool* bt_changed, bool* wp_changed, std::string* err) {
  // main.conf only when something is set: with nothing, bluetoothd reads the image's own.
  const std::string conf_path = opt_.data_dir + "/main.conf";
  std::string conf_arg;
  const bool any_main = std::any_of(st_.settings.begin(), st_.settings.end(), [](const auto& kv) { return !for_wireplumber(kv.first); });
  if (any_main) {
    const std::string text = main_conf_merge(slurp(opt_.base_main_conf), st_.settings);
    if (slurp(conf_path) != text) {
      if (!put_file(conf_path, text, err)) return false;
      *bt_changed = true;
    }
    conf_arg = conf_path;
  }
  const std::string args = bluetoothd_setup_args(st_.plugins_off, conf_arg);
  if (read_bluetoothd_noplugin(opt_.data_dir) != args) {
    if (!write_bluetoothd_env(opt_.data_dir, "BLUETOOTHD_NOPLUGIN", args, err)) return false;
    *bt_changed = true;
  }
  const std::string wp = wireplumber_roles_conf(st_.roles_on, st_.settings);
  if (slurp(opt_.wireplumber_conf) != wp) {
    if (!put_file(opt_.wireplumber_conf, wp, err)) return false;
    *wp_changed = true;
  }
  sync();
  return true;
}

json Setup::state_locked() const {
  json schema = setup_schema();
  const bool remote = remote_on_ ? remote_on_() : false;
  for (json& p : schema["profiles"]) {
    bool on = true;
    for (const json& x : p["plugins"]) on = on && !st_.plugins_off.count(x.get<std::string>());
    for (const json& x : p["roles"]) on = on && st_.roles_on.count(x.get<std::string>());
    for (const json& x : p["obex"]) on = on && !st_.obex_off.count(x.get<std::string>());
    if (p["id"] == "hid_remote") on = remote;
    p["on"] = on;
  }
  for (json& p : schema["plugins"]) p["on"] = !st_.plugins_off.count(p["name"].get<std::string>());
  for (json& s : schema["settings"]) {
    const auto it = st_.settings.find(s["key"].get<std::string>());
    s["value"] = it == st_.settings.end() ? json(nullptr) : it->second;
  }
  schema["apply"] = opt_.apply;
  const bool any_main = std::any_of(st_.settings.begin(), st_.settings.end(), [](const auto& kv) { return !for_wireplumber(kv.first); });
  schema["files"] = json{{"main_conf", any_main ? json(opt_.data_dir + "/main.conf") : json(nullptr)},
                         {"bluetoothd_env", opt_.data_dir + "/bluetoothd.env"},
                         {"wireplumber", opt_.wireplumber_conf}};
  return schema;
}

json Setup::state() const {
  std::lock_guard<std::mutex> lk(m_);
  return state_locked();
}

bool Setup::change(const json& body, json* out, std::string* err, int* status) {
  *status = 400;
  if (!body.is_object()) {
    *err = "the body is an object";
    return false;
  }
  std::unique_lock<std::mutex> lk(m_);
  SetupState next = st_;
  int remote_want = -1;
  // Named: items() of a temporary in a range-for would iterate a destroyed object.
  const json profiles = body.value("profiles", json::object());
  const json plugins = body.value("plugins", json::object());
  const json settings = body.value("settings", json::object());
  try {
    if (!profiles.is_object() || !plugins.is_object() || !settings.is_object())
      throw std::invalid_argument("profiles, plugins and settings are objects");
    for (const auto& [id, v] : profiles.items()) {
      const auto& t = profiles_table();
      const auto p = std::find_if(t.begin(), t.end(), [&](const Profile& x) { return id == x.id; });
      if (p == t.end()) throw std::invalid_argument("no profile " + id);
      const bool on = v.get<bool>();
      auto apply = [on](const std::vector<std::string>& parts, std::set<std::string>* off) {
        for (const std::string& x : parts) {
          if (on) off->erase(x);
          else off->insert(x);
        }
      };
      apply(p->plugins, &next.plugins_off);
      for (const std::string& r : p->roles) {
        if (on) next.roles_on.insert(r);
        else next.roles_on.erase(r);
      }
      apply(p->obex, &next.obex_off);
      if (p->remote) remote_want = on;
    }
    for (const auto& [name, v] : plugins.items()) {
      if (!known_plugin(name)) throw std::invalid_argument("no plugin " + name);
      if (v.get<bool>()) next.plugins_off.erase(name);
      else next.plugins_off.insert(name);
    }
    for (const auto& [key, v] : settings.items()) {
      std::string e;
      if (!setup_setting_ok(key, v, &e)) throw std::invalid_argument(e);
      if (v.is_null()) next.settings.erase(key);
      else next.settings[key] = v;
    }
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
  const auto mn = next.settings.find("LE.MinConnectionInterval"), mx = next.settings.find("LE.MaxConnectionInterval");
  if (mn != next.settings.end() && mx != next.settings.end() && mn->second.get<double>() > mx->second.get<double>()) {
    *err = "the minimum connection interval is above the maximum";
    return false;
  }
  const bool obex_changed = next.obex_off != st_.obex_off;
  st_ = std::move(next);
  bool bt = false, wp = false;
  *status = 500;
  if (!save(err) || !write_files(&bt, &wp, err)) return false;
  json restarted = json::array();
  const std::string obex = obexd_noplugin(st_.obex_off);
  lk.unlock();
  // obexd first: a running obexd registers its servers again with every bluetoothd that comes up,
  // so restarted after bluetoothd, the old one would have put back the servers just switched off
  // (and the new one be refused them: "UUID already registered").
  if (obex_changed && obex_apply_) {
    obex_apply_(obex);
    restarted.push_back("obexd");
  }
  // Then, on the board, what reads a file: WirePlumber before bluetoothd, so its Bluetooth monitor
  // is up when bluetoothd comes back and registers its endpoints at once.
  if (opt_.apply) {
    if (wp) {
      const ExecResult r = run_cmd({"systemctl", "restart", "wireplumber.service"}, 30000);
      if (!r.ok()) LOG_WARN("setup: restarting wireplumber: {}", r.reason());
      else restarted.push_back("wireplumber.service");
    }
    if (bt) {
      const ExecResult r = run_cmd({"systemctl", "restart", "bluetooth.service"}, 30000);
      if (!r.ok()) {
        *err = "saved, but restarting bluetooth failed: " + r.reason();
        return false;
      }
      restarted.push_back("bluetooth.service");
    }
  }
  json result = state();
  if (remote_want >= 0 && remote_set_) {
    std::string e;
    if (!remote_set_(remote_want == 1, &e)) {
      *status = 409;
      *err = "the HID remote: " + e;
      return false;
    }
    result = state();
  }
  LOG_INFO("setup: changed; restarted {}", restarted.dump());
  result["restarted"] = restarted;
  result["pending"] = !opt_.apply && (bt || wp);
  *out = std::move(result);
  return true;
}

}  // namespace btb
