#pragma once

// Setup (docs/api.md "Setup"): what the stack offers, switched on and off as profiles, BlueZ's
// plugins one by one, and the main.conf settings worth turning — the radio (dual, BR/EDR only,
// LE only), Secure Connections, privacy, the LE intervals, the GATT client, the policy plugin's
// reconnects.
//
// Each switch lives in the layer that implements it:
//   - BlueZ plugins: bluetoothd's --noplugin list;
//   - main.conf: a file btbenchd writes (/data/btbench/main.conf: the image's /etc/bluetooth/
//     main.conf with these keys set), passed with --configfile — the image's file is never edited;
//     both go in BLUETOOTHD_NOPLUGIN of bluetoothd.env, which the bluetooth.service drop-in reads;
//   - audio roles (A2DP sink/source, HFP, LE Audio): WirePlumber's bluez5.roles, in a fragment
//     btbenchd writes (60-btbench-profiles.conf), PipeWire mode only;
//   - OBEX server profiles: obexd's --noplugin, when btbenchd starts it;
//   - the HID remote: btbenchd's own (hid_device.h).
// A change is applied by restarting what reads it (bluetooth, wireplumber, obexd) — on the board
// only: on a PC the files go to the data directory and nothing of the desktop's is touched.

#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace btb {

struct SetupOptions {
  std::string data_dir = "/data/btbench";
  std::string base_main_conf = "/etc/bluetooth/main.conf";
  // On the board /etc/wireplumber/wireplumber.conf.d/60-btbench-profiles.conf links to it.
  std::string wireplumber_conf = "/data/btbench/wireplumber-profiles.conf";
  bool apply = true;  // restart services (false on a PC)
};

// What is switched, as kept in /data/btbench/setup.json.
struct SetupState {
  std::set<std::string> plugins_off = {"hfp"};  // BlueZ's own HFP would fight PipeWire's
  // WirePlumber's bluez5.roles that are on: the image's set until changed. Kept as the roles on
  // (not off), so a role added to the table later starts off, as the image has it.
  std::set<std::string> roles_on = {"a2dp_sink", "a2dp_source", "bap_sink", "bap_source", "hfp_hf", "hfp_ag"};
  std::set<std::string> obex_off;               // obexd plugins
  std::map<std::string, nlohmann::json> settings;  // "LE.MinConnectionInterval" → 7.5 (in the API's unit)
};

// The tables the API and the console show.
nlohmann::json setup_schema();
// A settings change checked against the schema: a value in range (ms where the API says ms),
// or null to go back to BlueZ's default. False with a reason.
bool setup_setting_ok(const std::string& key, const nlohmann::json& v, std::string* err);
// The image's main.conf with `settings` set ("Section.Key" → the API value, turned into BlueZ's
// units): a key that is there (or commented out) is replaced in place, a missing one added to its
// section, a missing section added at the end.
std::string main_conf_merge(const std::string& base, const std::map<std::string, nlohmann::json>& settings);
// BLUETOOTHD_NOPLUGIN's value: "--noplugin=a,b --configfile=<path>" (either part may be absent).
std::string bluetoothd_setup_args(const std::set<std::string>& plugins_off, const std::string& config_file);
// The WirePlumber fragment: bluez5.roles set to the roles on (in the table's order), and the WirePlumber
// settings among `settings` (Monitor.*, Device.*, Settings.*).
std::string wireplumber_roles_conf(const std::set<std::string>& roles_on, const std::map<std::string, nlohmann::json>& settings);
// obexd's --noplugin value, "" for none.
std::string obexd_noplugin(const std::set<std::string>& off);

class Setup {
 public:
  // remote_on/remote_set: the HID remote's state, read and switched (it keeps its own file).
  using RemoteState = std::function<bool()>;
  using RemoteSet = std::function<bool(bool on, std::string* err)>;
  using ObexApply = std::function<void(const std::string& noplugin)>;
  Setup(SetupOptions opt, RemoteState remote_on, RemoteSet remote_set, ObexApply obex_apply);

  // {profiles:[{id, label, group, help, on, layers}], plugins:[{name, label, help, on}],
  //  settings:[{key, value, ...schema}], audio_roles:[...], apply, files:{...}}
  nlohmann::json state() const;
  // {"profiles": {id: bool}, "plugins": {name: bool}, "settings": {key: value|null}}; writes the
  // files and restarts what reads them. *out: the state plus "restarted": [...].
  bool change(const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  // obexd's --noplugin value now (ObexManager reads it when it starts obexd).
  std::string obex_noplugin() const;

 private:
  bool load();
  bool save(std::string* err) const;
  bool write_files(bool* bt_changed, bool* wp_changed, std::string* err);
  nlohmann::json state_locked() const;

  const SetupOptions opt_;
  const RemoteState remote_on_;
  const RemoteSet remote_set_;
  const ObexApply obex_apply_;
  mutable std::mutex m_;
  SetupState st_;
};

}  // namespace btb
