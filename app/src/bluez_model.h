#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// The API's view of BlueZ's object tree, computed from one GetManagedObjects reply (a{oa{sa{sv}}}
// read into JSON by read_json: {path: {iface: {prop: value}}}). Pure functions of that snapshot —
// no bus, no locks — so the shapes the API serves are unit-tested against synthetic trees.
namespace btb {

// "AA:BB:CC:DD:EE:FF", hex in either case. What the API accepts in a URL.
bool bt_address_ok(const std::string& a);
// BlueZ's object for a device on an adapter: /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF.
std::string bt_device_path(const std::string& adapter_path, const std::string& address);

// One adapter as BlueZ lists it.
struct BtAdapterId {
  std::string path;
  std::string address;
  // On USB: a dongle or an M.2 card on an adapter, not the board's own controller. Not in BlueZ's
  // tree (the caller fills it in from sysfs).
  bool usb = false;
};
// `want` is an hci name ("hci1") or an address. "" is the board's own controller: the first not on
// USB, hci0 among those first — a dongle can probe before the board's UART controller and take
// hci0 at boot. "" when none matches.
std::string bt_pick_adapter(const std::vector<BtAdapterId>& adapters, const std::string& want);
std::vector<BtAdapterId> model_adapter_ids(const nlohmann::json& objs);

// Every adapter: {name:"hci0", path, address, address_type, alias, powered, discoverable,
// discoverable_timeout_s, pairable, discovering, roles, uuids:[...], adv:{supported_instances,
// active_instances, supported_includes}|null, manufacturer:2|null, manufacturer_name:"Intel",
// version:"5.4"|null (the controller's HCI version), system_name, connectable|null,
// power_state|null ("on", "off-enabling", ...), pairable_timeout_s, class|null and class_major,
// modalias|null and modalias_name, experimental:[{uuid, name}] (bluetoothd's enabled features)}.
nlohmann::json model_adapters(const nlohmann::json& objs);
// A Bluetooth company identifier's name, for the controller makers a bench meets ("" for others),
// and an HCI/LMP version number as the spec's version ("5.4"; "" when unknown).
std::string bt_company_name(int id);
std::string bt_version_name(int v);
// A Device ID modalias ("usb:v1D6Bp0246d0557") as who made it: "BlueZ 5.87" for BlueZ's own, the
// company of a bluetooth: source; "" for others. An experimental feature's UUID as its name.
std::string bt_modalias_name(const std::string& modalias);
std::string bt_experimental_name(const std::string& uuid);

// One device from its Device1 properties: address, address_type, name, alias, adapter, icon,
// paired, bonded, trusted, blocked, connected, services_resolved, legacy_pairing, cable_pairing,
// modalias|null and modalias_name, security_level|null, encryption_type|null, rssi|null,
// tx_power|null, class|null ("0x240404") and class_major, appearance|null and appearance_name,
// uuids:[{uuid:"110b", name:"Audio Sink"}], manufacturer_data:{"0x004c":"hex"},
// service_data:{"fe2c":"hex"}, advertising_flags:"hex"|null, set|null (the DeviceSet1 path of
// the coordinated set it is in) and set_rank|null.
// `obj`, the device's whole object (all its interfaces), adds its bearers: BlueZ's
// org.bluez.Bearer.BREDR1 and .LE1 (experimental), one per transport the device was seen on, each
// {connected, paired, bonded, connectable|null}; null for a transport it has no bearer on.
nlohmann::json model_device(const std::string& path, const nlohmann::json& dev,
                            const nlohmann::json* obj = nullptr);
// What a connected device has up, by profile, from its object and its child objects (transports,
// players, GATT services): [{id: a2dp|le-audio|avrcp|hid|pan|gatt|battery, name, detail, state}].
// HFP is not there: PipeWire's own HFP does not go through BlueZ.
nlohmann::json model_profiles(const nlohmann::json& dev_obj, const std::vector<const nlohmann::json*>& children);
// All devices, each with its profiles while connected, optionally only those of one adapter path. Sorted: connected, paired, then by
// name, so the list does not reshuffle as a scan adds devices.
nlohmann::json model_devices(const nlohmann::json& objs, const std::string& adapter_path = "");

// The coordinated sets (CSIP, org.bluez.DeviceSet1), optionally only those of one adapter path:
// [{path, adapter, size, auto_connect, devices:["AA:BB:CC:DD:EE:FF", ...]}].
nlohmann::json model_sets(const nlohmann::json& objs, const std::string& adapter_path = "");

// The device's GATT database as BlueZ resolved it: {services:[{path, handle, uuid, name, primary,
// characteristics:[{path, handle, uuid, name, flags:[...], value:"hex", text, notifying,
// descriptors:[{path, handle, uuid, name, value, text}]}]}]}, by handle.
nlohmann::json model_gatt(const nlohmann::json& objs, const std::string& dev_path);
// A characteristic's or descriptor's value as text: of one whose format the spec says is a UTF-8
// string (Device Name, the Device Information strings, a track title, a User Description), every
// byte of it; of any other, printable()'s guess, which leaves out a lone byte.
std::string gatt_text(const std::string& uuid, const std::vector<uint8_t>& value);
// A characteristic's or descriptor's handle: its Handle property, or the hex BlueZ puts at the end
// of the object's name (char002a), which is the same number.
uint16_t gatt_handle(const std::string& path, const nlohmann::json& props);
// The characteristic (or descriptor) object with that handle under the device. "iface" is
// org.bluez.GattCharacteristic1 or org.bluez.GattDescriptor1. False when there is none.
bool gatt_find(const nlohmann::json& objs, const std::string& dev_path, uint16_t handle,
               std::string* path, std::string* iface);

// Remote stream endpoints, transports and AVRCP players: {endpoints:[{path, device, address,
// uuid, role, codec:{id, name, summary, ...decoded}, capabilities:"hex", delay_reporting}],
// transports:[{path, device, address, uuid, profile, endpoint, state, codec:{...},
// configuration:"hex", delay_ms|null, volume|null}], players:[{path, device, address, name, type,
// subtype, status, track:{title, artist, album, genre, track_number, number_of_tracks,
// duration_ms, img_handle}, position_ms, repeat|null, shuffle|null, equalizer|null, scan|null,
// browsable, searchable, obex_port|null (BIP cover art), transport|null, folder?:{name, items}, items:[{path, name, type,
// folder_type, playable, metadata:{...as track}}]}]}.
nlohmann::json model_media(const nlohmann::json& objs);

// An AVRCP action as the API takes it ("play", "fast-forward", "press:0x44", ...) → the
// MediaPlayer1 method, and for Press/Hold the key (else -1). False for anything else.
bool player_method(const std::string& action, std::string* method, int* key);
// MediaFolder1.ListItems' reply (as BtManager::call gives it: [[path, props], ...] in the
// listing's order) → [{path, name, type, folder_type, playable, title, artist, album,
// duration_ms}].
nlohmann::json browse_items(const nlohmann::json& reply);

// Class of Device's major class ("audio/video", "phone", ...), and an appearance in words.
std::string class_major_name(uint32_t cod);
std::string appearance_name(uint16_t appearance);

// btmgmt conn-info's answer (mgmt Get Connection Information): the link's RSSI and TX power as the
// controller measures them, for a connected device (BlueZ's RSSI property is from discovery
// only). {rssi, tx_power, max_tx_power}, each null where the controller does not know (127);
// null when the output has no such line (not connected, a refusal).
nlohmann::json parse_conn_info(const std::string& out);

}  // namespace btb
