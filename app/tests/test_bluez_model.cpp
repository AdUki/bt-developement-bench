// The API's view of BlueZ's object tree, against synthetic GetManagedObjects replies shaped the
// way read_json() reads them ({path: {iface: {prop: value}}}).
#include "bluez_model.h"
#include "check.h"
#include "uuids.h"

using namespace btb;
using json = nlohmann::json;

namespace {

const char* kDev = "/org/bluez/hci0/dev_C0_FF_EE_00_00_01";

json tree() {
  return json::parse(R"({
    "/org/bluez": {"org.bluez.AgentManager1": {}},
    "/org/bluez/hci0": {
      "org.bluez.Adapter1": {"Address": "B8:27:EB:50:7B:22", "AddressType": "public", "Alias": "btbench",
                             "Powered": true, "Discoverable": false, "DiscoverableTimeout": 180,
                             "Pairable": true, "Discovering": true, "UUIDs": ["0000110b-0000-1000-8000-00805f9b34fb"]},
      "org.bluez.LEAdvertisingManager1": {"ActiveInstances": 1, "SupportedInstances": 4,
                                          "SupportedIncludes": ["tx-power", "appearance", "local-name"]}
    },
    "/org/bluez/hci1": {"org.bluez.Adapter1": {"Address": "00:1B:DC:08:4B:CC", "Alias": "vhci"}},
    "/org/bluez/hci0/dev_C0_FF_EE_00_00_01": {"org.bluez.Device1": {
      "Address": "c0:ff:ee:00:00:01", "AddressType": "random", "Name": "Bench HRM", "Alias": "Bench HRM",
      "Paired": false, "Connected": true, "ServicesResolved": true, "RSSI": -61, "TxPower": 4,
      "Appearance": 833, "UUIDs": ["0000180d-0000-1000-8000-00805f9b34fb"],
      "ManufacturerData": {"89": [1, 2, 255]}, "ServiceData": {"0000180d-0000-1000-8000-00805f9b34fb": [72]},
      "AdvertisingFlags": [6]}},
    "/org/bluez/hci0/dev_A0_B1_C2_D3_E4_F5": {"org.bluez.Device1": {
      "Address": "A0:B1:C2:D3:E4:F5", "Name": "Galaxy Buds", "Alias": "Galaxy Buds", "Paired": true,
      "Connected": false, "Class": 2360324}},
    "/org/bluez/hci0/dev_AA_00_00_00_00_01": {"org.bluez.Device1": {"Address": "AA:00:00:00:00:01", "Alias": "aardvark"}},
    "/org/bluez/hci1/dev_11_22_33_44_55_66": {"org.bluez.Device1": {"Address": "11:22:33:44:55:66", "Alias": "other"}},
    "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service000a": {"org.bluez.GattService1": {
      "UUID": "0000180d-0000-1000-8000-00805f9b34fb", "Primary": true, "Device": "/org/bluez/hci0/dev_C0_FF_EE_00_00_01"}},
    "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service000a/char000d": {"org.bluez.GattCharacteristic1": {
      "UUID": "00002a38-0000-1000-8000-00805f9b34fb", "Service": "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service000a",
      "Flags": ["read"], "Value": [1]}},
    "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service000a/char000b": {"org.bluez.GattCharacteristic1": {
      "UUID": "00002a37-0000-1000-8000-00805f9b34fb", "Service": "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service000a",
      "Flags": ["notify"], "Notifying": true, "Value": [0, 72], "Handle": 11}},
    "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service000a/char000b/desc000c": {"org.bluez.GattDescriptor1": {
      "UUID": "00002902-0000-1000-8000-00805f9b34fb",
      "Characteristic": "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service000a/char000b", "Value": [1, 0]}},
    "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service0010": {"org.bluez.GattService1": {
      "UUID": "0000180a-0000-1000-8000-00805f9b34fb", "Primary": true}},
    "/org/bluez/hci0/dev_C0_FF_EE_00_00_01/service0010/char0011": {"org.bluez.GattCharacteristic1": {
      "UUID": "00002a29-0000-1000-8000-00805f9b34fb", "Flags": ["read"], "Value": [98, 116, 98]}},
    "/org/bluez/hci0/dev_A0_B1_C2_D3_E4_F5/sep1": {"org.bluez.MediaEndpoint1": {
      "UUID": "0000110b-0000-1000-8000-00805f9b34fb", "Codec": 0, "Capabilities": [255, 255, 2, 53],
      "Device": "/org/bluez/hci0/dev_A0_B1_C2_D3_E4_F5", "DelayReporting": true}},
    "/org/bluez/hci0/dev_A0_B1_C2_D3_E4_F5/sep1/fd0": {"org.bluez.MediaTransport1": {
      "UUID": "0000110a-0000-1000-8000-00805f9b34fb", "Codec": 0, "Configuration": [17, 21, 2, 53],
      "Device": "/org/bluez/hci0/dev_A0_B1_C2_D3_E4_F5", "State": "idle", "Delay": 1500, "Volume": 100,
      "Endpoint": "/org/bluez/hci0/dev_A0_B1_C2_D3_E4_F5/sep1"}}
  })");
}

void test_addresses() {
  CHECK(bt_address_ok("5C:E9:1E:22:40:01"));
  CHECK(bt_address_ok("5c:e9:1e:22:40:01"));
  CHECK(!bt_address_ok("5C:E9:1E:22:40"));
  CHECK(!bt_address_ok("5C-E9-1E-22-40-01"));
  CHECK(!bt_address_ok("../../../etc/pass"));
  CHECK_EQ(bt_device_path("/org/bluez/hci0", "5c:e9:1e:22:40:01"),
           std::string("/org/bluez/hci0/dev_5C_E9_1E_22_40_01"));
}

void test_adapter_choice() {
  const std::vector<BtAdapterId> two = {{"/org/bluez/hci1", "00:1B:DC:08:4B:CC"}, {"/org/bluez/hci0", "44:A3:BB:36:5E:2E"}};
  CHECK_EQ(bt_pick_adapter(two, ""), std::string("/org/bluez/hci0"));
  CHECK_EQ(bt_pick_adapter(two, "hci1"), std::string("/org/bluez/hci1"));
  CHECK_EQ(bt_pick_adapter(two, "00:1b:dc:08:4b:cc"), std::string("/org/bluez/hci1"));
  CHECK_EQ(bt_pick_adapter(two, "hci2"), std::string());
  CHECK_EQ(bt_pick_adapter({}, ""), std::string());
  CHECK_EQ(model_adapter_ids(tree()).size(), size_t(2));
}

void test_adapters() {
  const json a = model_adapters(tree());
  CHECK_EQ(a.size(), size_t(2));
  CHECK_EQ(a[0]["name"].get<std::string>(), std::string("hci0"));
  CHECK(a[0]["discovering"].get<bool>());
  CHECK_EQ(a[0]["adv"]["supported_instances"].get<int>(), 4);
  CHECK_EQ(a[0]["uuids"][0]["uuid"].get<std::string>(), std::string("110b"));
  CHECK(a[1]["adv"].is_null());
}

void test_devices() {
  const json all = model_devices(tree());
  CHECK_EQ(all.size(), size_t(4));
  const json d = model_devices(tree(), "/org/bluez/hci0");
  CHECK_EQ(d.size(), size_t(3));
  // Connected first, then paired, then by name.
  CHECK_EQ(d[0]["alias"].get<std::string>(), std::string("Bench HRM"));
  CHECK_EQ(d[1]["alias"].get<std::string>(), std::string("Galaxy Buds"));
  CHECK_EQ(d[2]["alias"].get<std::string>(), std::string("aardvark"));
  const json& h = d[0];
  CHECK_EQ(h["address"].get<std::string>(), std::string("C0:FF:EE:00:00:01"));  // upper-cased
  CHECK_EQ(h["adapter"].get<std::string>(), std::string("hci0"));
  CHECK_EQ(h["rssi"].get<int>(), -61);
  CHECK_EQ(h["appearance"].get<std::string>(), std::string("0x0341"));
  CHECK_EQ(h["appearance_name"].get<std::string>(), std::string("Heart Rate Sensor"));
  CHECK_EQ(h["manufacturer_data"]["0x0059"].get<std::string>(), std::string("0102ff"));
  CHECK_EQ(h["service_data"]["180d"].get<std::string>(), std::string("48"));
  CHECK_EQ(h["advertising_flags"].get<std::string>(), std::string("06"));
  CHECK_EQ(h["uuids"][0]["name"].get<std::string>(), std::string("Heart Rate"));
  const json& b = d[1];
  CHECK_EQ(b["class"].get<std::string>(), std::string("0x240404"));
  CHECK_EQ(b["class_major"].get<std::string>(), std::string("audio/video"));
  CHECK(b["rssi"].is_null());
  CHECK(b["advertising_flags"].is_null());
}

void test_gatt() {
  const json g = model_gatt(tree(), kDev);
  CHECK_EQ(g["services"].size(), size_t(2));
  const json& hrs = g["services"][0];
  CHECK_EQ(hrs["handle"].get<int>(), 0x0a);
  CHECK_EQ(hrs["name"].get<std::string>(), std::string("Heart Rate"));
  CHECK_EQ(hrs["characteristics"].size(), size_t(2));
  // Sorted by handle, whatever order BlueZ listed them in.
  CHECK_EQ(hrs["characteristics"][0]["handle"].get<int>(), 0x0b);
  CHECK(hrs["characteristics"][0]["notifying"].get<bool>());
  CHECK_EQ(hrs["characteristics"][0]["value"].get<std::string>(), std::string("0048"));
  CHECK_EQ(hrs["characteristics"][0]["descriptors"].size(), size_t(1));
  CHECK_EQ(hrs["characteristics"][0]["descriptors"][0]["handle"].get<int>(), 0x0c);
  CHECK_EQ(hrs["characteristics"][1]["handle"].get<int>(), 0x0d);
  // A characteristic without a Service property is placed by its path.
  const json& dis = g["services"][1];
  CHECK_EQ(dis["characteristics"].size(), size_t(1));
  CHECK_EQ(dis["characteristics"][0]["text"].get<std::string>(), std::string("btb"));

  std::string path, iface;
  CHECK(gatt_find(tree(), kDev, 0x0b, &path, &iface));
  CHECK_EQ(iface, std::string("org.bluez.GattCharacteristic1"));
  CHECK(gatt_find(tree(), kDev, 0x0c, &path, &iface));
  CHECK_EQ(iface, std::string("org.bluez.GattDescriptor1"));
  CHECK(!gatt_find(tree(), kDev, 0x0a, &path, &iface));  // a service is not readable
  CHECK(!gatt_find(tree(), kDev, 0x99, &path, &iface));
  CHECK_EQ(gatt_handle("/x/char002a", json::object()), 0x2a);
  CHECK_EQ(gatt_handle("/x/char002a", json{{"Handle", 7}}), 7);
  CHECK_EQ(gatt_handle("/x/whatever", json::object()), 0);
}

void test_media() {
  const json m = model_media(tree());
  CHECK_EQ(m["endpoints"].size(), size_t(1));
  CHECK_EQ(m["transports"].size(), size_t(1));
  const json& e = m["endpoints"][0];
  CHECK_EQ(e["address"].get<std::string>(), std::string("A0:B1:C2:D3:E4:F5"));
  CHECK_EQ(e["role"].get<std::string>(), std::string("Audio Sink"));
  CHECK_EQ(e["codec"]["name"].get<std::string>(), std::string("SBC"));
  CHECK_EQ(e["capabilities"].get<std::string>(), std::string("ffff0235"));
  const json& t = m["transports"][0];
  CHECK_EQ(t["state"].get<std::string>(), std::string("idle"));
  CHECK_NEAR(t["delay_ms"].get<double>(), 150.0, 1e-9);
  CHECK_EQ(t["volume"].get<int>(), 100);
  CHECK_EQ(t["codec"]["rate"].get<int>(), 48000);
  CHECK_EQ(t["configuration"].get<std::string>(), std::string("11150235"));
}

void test_players() {
  const json objs = json::parse(R"({
    "/org/bluez/hci0/dev_5C_E9_1E_22_40_01": {"org.bluez.Device1": {"Address": "5C:E9:1E:22:40:01"}},
    "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/sep1/fd0": {"org.bluez.MediaTransport1": {
      "Device": "/org/bluez/hci0/dev_5C_E9_1E_22_40_01", "UUID": "0000110b-0000-1000-8000-00805f9b34fb",
      "Codec": 0, "Configuration": [17, 21, 2, 53], "State": "active", "Volume": 90}},
    "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/player0": {
      "org.bluez.MediaPlayer1": {"Device": "/org/bluez/hci0/dev_5C_E9_1E_22_40_01", "Name": "Music",
        "Status": "paused", "Position": 1500, "Repeat": "alltracks", "Browsable": true,
        "Track": {"Title": "T", "Artist": "A", "TrackNumber": 3, "NumberOfTracks": 4294967295,
                  "Duration": 4294967295}},
      "org.bluez.MediaFolder1": {"Name": "/NowPlaying", "NumberOfItems": 2}},
    "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/player0/NowPlaying/item10": {"org.bluez.MediaItem1": {
      "Player": "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/player0", "Name": "Ten", "Type": "audio", "Playable": true}},
    "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/player0/NowPlaying/item2": {"org.bluez.MediaItem1": {
      "Player": "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/player0", "Name": "Two", "Type": "audio",
      "Metadata": {"Title": "Two", "Duration": 1000}}}
  })");
  const json m = model_media(objs);
  CHECK_EQ(m["players"].size(), size_t(1));
  const json& p = m["players"][0];
  CHECK_EQ(p["status"].get<std::string>(), std::string("paused"));
  CHECK_EQ(p["repeat"].get<std::string>(), std::string("alltracks"));
  CHECK(p["shuffle"].is_null());  // not offered by this player
  CHECK(p["browsable"].get<bool>());
  CHECK_EQ(p["track"]["track_number"].get<int>(), 3);
  CHECK(p["track"]["number_of_tracks"].is_null());  // all ones: unknown
  CHECK_EQ(p["track"]["duration_ms"].get<int>(), 0);
  CHECK_EQ(p["transport"].get<std::string>(), std::string("/org/bluez/hci0/dev_5C_E9_1E_22_40_01/sep1/fd0"));
  CHECK_EQ(p["folder"]["items"].get<int>(), 2);
  // Items in listing order: item2 before item10.
  CHECK_EQ(p["items"].size(), size_t(2));
  CHECK_EQ(p["items"][0]["name"].get<std::string>(), std::string("Two"));
  CHECK_EQ(p["items"][0]["metadata"]["duration_ms"].get<int>(), 1000);
  CHECK(p["items"][1]["playable"].get<bool>());

  std::string method;
  int key = 0;
  CHECK(player_method("fast-forward", &method, &key));
  CHECK_EQ(method, std::string("FastForward"));
  CHECK_EQ(key, -1);
  CHECK(player_method("press:0x44", &method, &key));
  CHECK_EQ(method, std::string("Press"));
  CHECK_EQ(key, 0x44);
  CHECK(player_method("hold:75", &method, &key));
  CHECK_EQ(key, 75);
  CHECK(!player_method("press:0x80", &method, &key));
  CHECK(!player_method("eject", &method, &key));

  const json items = browse_items(json::parse(R"([{"/p/item1": {"Name": "One", "Type": "audio",
      "Playable": true, "Metadata": {"Title": "One", "Artist": "X", "Duration": 2000}}}])"));
  CHECK_EQ(items.size(), size_t(1));
  CHECK_EQ(items[0]["path"].get<std::string>(), std::string("/p/item1"));
  CHECK_EQ(items[0]["artist"].get<std::string>(), std::string("X"));
  CHECK(browse_items(json::array()).empty());
}

void test_names() {
  CHECK_EQ(class_major_name(0x5a020c), std::string("phone"));
  CHECK_EQ(class_major_name(0x1f00), std::string("uncategorized"));
  CHECK_EQ(appearance_name(0x03c1), std::string("HID"));
  CHECK_EQ(appearance_name(0xffc0), std::string());
}

}  // namespace

int main() {
  std::string err;
  uuid_names().load(std::string(BTB_WWW_DIR) + "/uuids.json", &err);
  test_addresses();
  test_adapter_choice();
  test_adapters();
  test_devices();
  test_gatt();
  test_media();
  test_players();
  test_names();
  return report("test_bluez_model");
}
