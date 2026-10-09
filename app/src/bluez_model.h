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
};
// `want` is an hci name ("hci1") or an address, for a machine whose other adapters belong to
// someone else (a PC's, to its desktop); "" is hci0, or the first. "" when none matches.
std::string bt_pick_adapter(const std::vector<BtAdapterId>& adapters, const std::string& want);
std::vector<BtAdapterId> model_adapter_ids(const nlohmann::json& objs);

// Every adapter: {name:"hci0", path, address, address_type, alias, powered, discoverable,
// discoverable_timeout_s, pairable, discovering, roles, uuids:[...], adv:{supported_instances,
// active_instances, supported_includes}|null}.
nlohmann::json model_adapters(const nlohmann::json& objs);

// One device from its Device1 properties: address, address_type, name, alias, adapter, icon,
// paired, bonded, trusted, blocked, connected, services_resolved, legacy_pairing, rssi|null,
// tx_power|null, class|null ("0x240404") and class_major, appearance|null and appearance_name,
// uuids:[{uuid:"110b", name:"Audio Sink"}], manufacturer_data:{"0x004c":"hex"},
// service_data:{"fe2c":"hex"}, advertising_flags:"hex"|null.
nlohmann::json model_device(const std::string& path, const nlohmann::json& dev);
// All devices, optionally only those of one adapter path. Sorted: connected, paired, then by
// name, so the list does not reshuffle as a scan adds devices.
nlohmann::json model_devices(const nlohmann::json& objs, const std::string& adapter_path = "");

// The device's GATT database as BlueZ resolved it: {services:[{path, handle, uuid, name, primary,
// characteristics:[{path, handle, uuid, name, flags:[...], value:"hex", text, notifying,
// descriptors:[{path, handle, uuid, name, value, text}]}]}]}, by handle.
nlohmann::json model_gatt(const nlohmann::json& objs, const std::string& dev_path);
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
// configuration:"hex", delay_ms|null, volume|null}], players:[{path, device, address, name,
// status, track:{title,artist,album,duration_ms}, position_ms}]}.
nlohmann::json model_media(const nlohmann::json& objs);

// Class of Device's major class ("audio/video", "phone", ...), and an appearance in words.
std::string class_major_name(uint32_t cod);
std::string appearance_name(uint16_t appearance);

}  // namespace btb
