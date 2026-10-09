#include "bluez_model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "a2dp_codec.h"
#include "dbus_util.h"
#include "util/strings.h"
#include "uuids.h"

using json = nlohmann::json;

namespace btb {

namespace {

constexpr const char* kAdapter = "org.bluez.Adapter1";
constexpr const char* kDevice = "org.bluez.Device1";
constexpr const char* kAdvMgr = "org.bluez.LEAdvertisingManager1";
constexpr const char* kService = "org.bluez.GattService1";
constexpr const char* kChar = "org.bluez.GattCharacteristic1";
constexpr const char* kDesc = "org.bluez.GattDescriptor1";
constexpr const char* kEndpoint = "org.bluez.MediaEndpoint1";
constexpr const char* kTransport = "org.bluez.MediaTransport1";
constexpr const char* kPlayer = "org.bluez.MediaPlayer1";
constexpr const char* kFolder = "org.bluez.MediaFolder1";
constexpr const char* kItem = "org.bluez.MediaItem1";

std::string hexn(unsigned v, int width) {
  char b[16];
  snprintf(b, sizeof(b), "0x%0*x", width, v);
  return b;
}

json uuid_entry(const std::string& u) {
  return json{{"uuid", uuid_short(u)}, {"name", uuid_names().name(u)}};
}

json uuid_list(const std::vector<std::string>& v) {
  json a = json::array();
  for (const std::string& u : v) a.push_back(uuid_entry(u));
  return a;
}

std::string text_of(const std::vector<uint8_t>& b) { return printable(b); }

// AVRCP track metadata (MediaPlayer1.Track, MediaItem1.Metadata). Numbers the device does not
// know are all ones, in whichever width it happens to use, and come out as 0 / null.
json player_track(const json& tr) {
  auto known = [&tr](const char* k) {
    const long long v = num_of(tr, k);
    return v > 0 && v < 0x7fffffff ? json(v) : json(nullptr);
  };
  const json dur = known("Duration");
  return json{{"title", str_of(tr, "Title")},
              {"artist", str_of(tr, "Artist")},
              {"album", str_of(tr, "Album")},
              {"genre", str_of(tr, "Genre")},
              {"track_number", known("TrackNumber")},
              {"number_of_tracks", known("NumberOfTracks")},
              {"duration_ms", dur.is_null() ? json(0) : dur},
              {"img_handle", str_of(tr, "ImgHandle")}};
}

// By what the list shows: the alias, which BlueZ fills with the address for a device that never
// said its name.
bool name_less(const json& x, const json& y) {
  return lower(x.value("alias", std::string{})) < lower(y.value("alias", std::string{}));
}

}  // namespace

bool bt_address_ok(const std::string& a) {
  if (a.size() != 17) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (i % 3 == 2) {
      if (a[i] != ':') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(a[i]))) {
      return false;
    }
  }
  return true;
}

std::string bt_device_path(const std::string& adapter_path, const std::string& address) {
  std::string a = upper(address);
  std::replace(a.begin(), a.end(), ':', '_');
  return adapter_path + "/dev_" + a;
}

std::string bt_pick_adapter(const std::vector<BtAdapterId>& adapters, const std::string& want) {
  if (want.empty()) {
    std::string path;
    for (const BtAdapterId& a : adapters)
      if (path.empty() || a.path == "/org/bluez/hci0") path = a.path;
    return path;
  }
  const bool by_address = bt_address_ok(want);
  for (const BtAdapterId& a : adapters) {
    if (by_address ? upper(a.address) == upper(want) : a.path == "/org/bluez/" + want) return a.path;
  }
  return {};
}

std::vector<BtAdapterId> model_adapter_ids(const json& objs) {
  std::vector<BtAdapterId> out;
  for (auto it = objs.begin(); it != objs.end(); ++it)
    if (it->contains(kAdapter)) out.push_back({it.key(), str_of((*it)[kAdapter], "Address")});
  return out;
}

json model_adapters(const json& objs) {
  json out = json::array();
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!it->contains(kAdapter)) continue;
    const json& a = (*it)[kAdapter];
    json j{{"name", adapter_from_path(it.key())},
           {"path", it.key()},
           {"address", str_of(a, "Address")},
           {"address_type", str_of(a, "AddressType")},
           {"alias", str_of(a, "Alias")},
           {"powered", bool_of(a, "Powered")},
           {"discoverable", bool_of(a, "Discoverable")},
           {"discoverable_timeout_s", num_of(a, "DiscoverableTimeout")},
           {"pairable", bool_of(a, "Pairable")},
           {"discovering", bool_of(a, "Discovering")},
           {"roles", strv_of(a, "Roles")},
           {"uuids", uuid_list(strv_of(a, "UUIDs"))},
           {"adv", nullptr}};
    if (it->contains(kAdvMgr)) {
      const json& m = (*it)[kAdvMgr];
      j["adv"] = json{{"supported_instances", num_of(m, "SupportedInstances")},
                      {"active_instances", num_of(m, "ActiveInstances")},
                      {"supported_includes", strv_of(m, "SupportedIncludes")},
                      {"supported_secondary_channels", strv_of(m, "SupportedSecondaryChannels")}};
    }
    out.push_back(std::move(j));
  }
  std::sort(out.begin(), out.end(), [](const json& x, const json& y) {
    return x["name"].get<std::string>() < y["name"].get<std::string>();
  });
  return out;
}

json model_device(const std::string& path, const json& d) {
  json j{{"path", path},
         {"address", upper(str_of(d, "Address"))},
         {"address_type", str_of(d, "AddressType")},
         {"name", str_of(d, "Name")},
         {"alias", str_of(d, "Alias")},
         {"adapter", adapter_from_path(path)},
         {"icon", str_of(d, "Icon")},
         {"paired", bool_of(d, "Paired")},
         {"bonded", bool_of(d, "Bonded")},
         {"trusted", bool_of(d, "Trusted")},
         {"blocked", bool_of(d, "Blocked")},
         {"connected", bool_of(d, "Connected")},
         {"services_resolved", bool_of(d, "ServicesResolved")},
         {"legacy_pairing", bool_of(d, "LegacyPairing")},
         {"rssi", d.contains("RSSI") ? json(num_of(d, "RSSI")) : json(nullptr)},
         {"tx_power", d.contains("TxPower") ? json(num_of(d, "TxPower")) : json(nullptr)},
         {"uuids", uuid_list(strv_of(d, "UUIDs"))}};
  if (d.contains("Class")) {
    const uint32_t cod = static_cast<uint32_t>(num_of(d, "Class"));
    j["class"] = hexn(cod, 6);
    j["class_major"] = class_major_name(cod);
  } else {
    j["class"] = nullptr;
    j["class_major"] = "";
  }
  if (d.contains("Appearance")) {
    const uint16_t ap = static_cast<uint16_t>(num_of(d, "Appearance"));
    j["appearance"] = hexn(ap, 4);
    j["appearance_name"] = appearance_name(ap);
  } else {
    j["appearance"] = nullptr;
    j["appearance_name"] = "";
  }
  // ManufacturerData a{qv}: read_json keys a uint16 as its decimal text.
  json md = json::object();
  if (d.contains("ManufacturerData") && d["ManufacturerData"].is_object()) {
    for (auto it = d["ManufacturerData"].begin(); it != d["ManufacturerData"].end(); ++it) {
      const unsigned id = static_cast<unsigned>(std::strtoul(it.key().c_str(), nullptr, 10));
      md[hexn(id, 4)] = to_hex(bytes_of(json{{"v", *it}}, "v"));
    }
  }
  j["manufacturer_data"] = md;
  json sd = json::object();
  if (d.contains("ServiceData") && d["ServiceData"].is_object()) {
    for (auto it = d["ServiceData"].begin(); it != d["ServiceData"].end(); ++it)
      sd[uuid_short(it.key())] = to_hex(bytes_of(json{{"v", *it}}, "v"));
  }
  j["service_data"] = sd;
  j["advertising_flags"] = d.contains("AdvertisingFlags") ? json(to_hex(bytes_of(d, "AdvertisingFlags")))
                                                          : json(nullptr);
  return j;
}

json model_devices(const json& objs, const std::string& adapter_path) {
  json out = json::array();
  const std::string prefix = adapter_path.empty() ? std::string{} : adapter_path + "/";
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!it->contains(kDevice)) continue;
    if (!prefix.empty() && !starts_with(it.key(), prefix)) continue;
    out.push_back(model_device(it.key(), (*it)[kDevice]));
  }
  std::stable_sort(out.begin(), out.end(), [](const json& x, const json& y) {
    if (x["connected"] != y["connected"]) return x["connected"].get<bool>();
    if (x["paired"] != y["paired"]) return x["paired"].get<bool>();
    return name_less(x, y);
  });
  return out;
}

uint16_t gatt_handle(const std::string& path, const json& props) {
  if (props.contains("Handle") && props["Handle"].is_number()) {
    const long long h = props["Handle"].get<long long>();
    if (h > 0 && h <= 0xffff) return static_cast<uint16_t>(h);
  }
  // service000a, char000b, desc000d: the hex digits after the last letter run.
  const size_t slash = path.rfind('/');
  const std::string leaf = path.substr(slash == std::string::npos ? 0 : slash + 1);
  size_t i = 0;
  while (i < leaf.size() && std::isalpha(static_cast<unsigned char>(leaf[i]))) ++i;
  const std::string digits = leaf.substr(i);
  if (digits.size() != 4) return 0;
  uint64_t v = 0;
  if (!parse_uint("0x" + digits, 0xffff, &v)) return 0;
  return static_cast<uint16_t>(v);
}

json model_gatt(const json& objs, const std::string& dev_path) {
  const std::string prefix = dev_path + "/";
  json services = json::array();
  // Three passes over the tree, but it is one device's few dozen objects, and the object names
  // alone say which characteristic sits under which service.
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!starts_with(it.key(), prefix) || !it->contains(kService)) continue;
    const json& s = (*it)[kService];
    const std::string uuid = str_of(s, "UUID");
    services.push_back(json{{"path", it.key()},
                            {"handle", gatt_handle(it.key(), s)},
                            {"uuid", uuid_short(uuid)},
                            {"name", uuid_names().name(uuid)},
                            {"primary", bool_of(s, "Primary")},
                            {"characteristics", json::array()}});
  }
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!starts_with(it.key(), prefix) || !it->contains(kChar)) continue;
    const json& c = (*it)[kChar];
    const std::string svc = str_of(c, "Service");
    const std::string uuid = str_of(c, "UUID");
    const std::vector<uint8_t> v = bytes_of(c, "Value");
    json cj{{"path", it.key()},
            {"handle", gatt_handle(it.key(), c)},
            {"uuid", uuid_short(uuid)},
            {"name", uuid_names().name(uuid)},
            {"flags", strv_of(c, "Flags")},
            {"value", c.contains("Value") ? json(to_hex(v)) : json(nullptr)},
            {"text", text_of(v)},
            {"notifying", bool_of(c, "Notifying")},
            {"mtu", c.contains("MTU") ? json(num_of(c, "MTU")) : json(nullptr)},
            {"descriptors", json::array()}};
    for (json& s : services) {
      if (s["path"] == svc || starts_with(it.key(), s["path"].get<std::string>() + "/")) {
        s["characteristics"].push_back(std::move(cj));
        break;
      }
    }
  }
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!starts_with(it.key(), prefix) || !it->contains(kDesc)) continue;
    const json& d = (*it)[kDesc];
    const std::string uuid = str_of(d, "UUID");
    const std::string ch = str_of(d, "Characteristic");
    const std::vector<uint8_t> v = bytes_of(d, "Value");
    json dj{{"path", it.key()},
            {"handle", gatt_handle(it.key(), d)},
            {"uuid", uuid_short(uuid)},
            {"name", uuid_names().name(uuid)},
            {"flags", strv_of(d, "Flags")},
            {"value", d.contains("Value") ? json(to_hex(v)) : json(nullptr)},
            {"text", text_of(v)}};
    bool placed = false;
    for (json& s : services) {
      for (json& c : s["characteristics"]) {
        if (c["path"] == ch || starts_with(it.key(), c["path"].get<std::string>() + "/")) {
          c["descriptors"].push_back(std::move(dj));
          placed = true;
          break;
        }
      }
      if (placed) break;
    }
  }
  auto by_handle = [](const json& a, const json& b) { return a["handle"] < b["handle"]; };
  std::sort(services.begin(), services.end(), by_handle);
  for (json& s : services) {
    std::sort(s["characteristics"].begin(), s["characteristics"].end(), by_handle);
    for (json& c : s["characteristics"])
      std::sort(c["descriptors"].begin(), c["descriptors"].end(), by_handle);
  }
  return json{{"services", services}};
}

bool gatt_find(const json& objs, const std::string& dev_path, uint16_t handle, std::string* path,
               std::string* iface) {
  const std::string prefix = dev_path + "/";
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!starts_with(it.key(), prefix)) continue;
    for (const char* i : {kChar, kDesc}) {
      if (!it->contains(i)) continue;
      if (gatt_handle(it.key(), (*it)[i]) != handle) continue;
      *path = it.key();
      *iface = i;
      return true;
    }
  }
  return false;
}

json model_media(const json& objs) {
  json endpoints = json::array(), transports = json::array(), players = json::array();
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (it->contains(kEndpoint)) {
      const json& e = (*it)[kEndpoint];
      const uint8_t codec = static_cast<uint8_t>(num_of(e, "Codec"));
      const std::vector<uint8_t> caps = bytes_of(e, "Capabilities");
      json c = decode_codec(codec, caps, true);
      c["id"] = codec;
      const std::string dev = str_of(e, "Device");
      endpoints.push_back(json{{"path", it.key()},
                               {"device", dev},
                               {"address", address_from_path(dev.empty() ? it.key() : dev)},
                               {"uuid", uuid_short(str_of(e, "UUID"))},
                               {"role", uuid_names().name(str_of(e, "UUID"))},
                               {"codec", c},
                               {"capabilities", to_hex(caps)},
                               {"delay_reporting", bool_of(e, "DelayReporting")}});
    }
    if (it->contains(kTransport)) {
      const json& t = (*it)[kTransport];
      const uint8_t codec = static_cast<uint8_t>(num_of(t, "Codec"));
      const std::vector<uint8_t> cfg = bytes_of(t, "Configuration");
      json c = decode_codec(codec, cfg, false);
      c["id"] = codec;
      const std::string dev = str_of(t, "Device");
      // Delay is in 1/10 ms, and only there when the sink reports one (A2DP delay reporting).
      transports.push_back(json{
          {"path", it.key()},
          {"device", dev},
          {"address", address_from_path(dev.empty() ? it.key() : dev)},
          {"uuid", uuid_short(str_of(t, "UUID"))},
          {"profile", uuid_names().name(str_of(t, "UUID"))},
          {"endpoint", str_of(t, "Endpoint")},
          {"state", str_of(t, "State")},
          {"codec", c},
          {"configuration", to_hex(cfg)},
          {"delay_ms", t.contains("Delay") ? json(num_of(t, "Delay") / 10.0) : json(nullptr)},
          {"volume", t.contains("Volume") ? json(num_of(t, "Volume")) : json(nullptr)}});
    }
    if (it->contains(kPlayer)) {
      const json& p = (*it)[kPlayer];
      const std::string dev = str_of(p, "Device");
      // Settings a player does not support are absent; null says so, rather than "off".
      auto opt = [&p](const char* k) { return p.contains(k) ? json(str_of(p, k)) : json(nullptr); };
      json pl{{"path", it.key()},
              {"device", dev},
              {"address", address_from_path(dev.empty() ? it.key() : dev)},
              {"name", str_of(p, "Name")},
              {"type", str_of(p, "Type")},
              {"subtype", str_of(p, "Subtype")},
              {"status", str_of(p, "Status")},
              {"track", player_track(p.contains("Track") ? p["Track"] : json::object())},
              {"position_ms", num_of(p, "Position")},
              {"repeat", opt("Repeat")},
              {"shuffle", opt("Shuffle")},
              {"equalizer", opt("Equalizer")},
              {"scan", opt("Scan")},
              {"browsable", bool_of(p, "Browsable")},
              {"searchable", bool_of(p, "Searchable")},
              {"items", json::array()}};
      // The transport of the same device, for the volume next to the transport controls.
      pl["transport"] = nullptr;
      for (auto t = objs.begin(); t != objs.end(); ++t) {
        if (t->contains(kTransport) && str_of((*t)[kTransport], "Device") == dev && !dev.empty()) {
          pl["transport"] = t.key();
          break;
        }
      }
      if (it->contains(kFolder))
        pl["folder"] = json{{"name", str_of((*it)[kFolder], "Name")},
                            {"items", num_of((*it)[kFolder], "NumberOfItems")}};
      players.push_back(std::move(pl));
    }
  }
  // MediaItem1 objects (what browsing a player listed: the now-playing list, a folder) under
  // their player, in the order of their object names (item1, item2, ...: BlueZ's listing order).
  std::vector<std::pair<std::string, json>> items;
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!it->contains(kItem)) continue;
    const json& m = (*it)[kItem];
    items.emplace_back(str_of(m, "Player"),
                       json{{"path", it.key()},
                            {"name", str_of(m, "Name")},
                            {"type", str_of(m, "Type")},
                            {"folder_type", str_of(m, "FolderType")},
                            {"playable", bool_of(m, "Playable")},
                            {"metadata", player_track(m.contains("Metadata") ? m["Metadata"] : json::object())}});
  }
  std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
    const std::string& x = a.second["path"].template get_ref<const std::string&>();
    const std::string& y = b.second["path"].template get_ref<const std::string&>();
    return x.size() != y.size() ? x.size() < y.size() : x < y;
  });
  for (auto& [player, item] : items) {
    for (json& pl : players) {
      // A player's items live under its path; Player says which when they do not.
      if (pl["path"] == player || (player.empty() && starts_with(item["path"].get<std::string>(),
                                                                 pl["path"].get<std::string>() + "/"))) {
        pl["items"].push_back(item);
        break;
      }
    }
  }
  return json{{"endpoints", endpoints}, {"transports", transports}, {"players", players}};
}

bool player_method(const std::string& action, std::string* method, int* key) {
  static const std::pair<const char*, const char*> kMethods[] = {
      {"play", "Play"},       {"pause", "Pause"},          {"stop", "Stop"},      {"next", "Next"},
      {"previous", "Previous"}, {"fast-forward", "FastForward"}, {"rewind", "Rewind"},
      {"release", "Release"}};
  *key = -1;
  for (const auto& [a, m] : kMethods) {
    if (action == a) {
      *method = m;
      return true;
    }
  }
  // press:<key>, hold:<key> — the AV/C pass-through operation id (0x44 play, 0x4b forward, ...).
  for (const char* p : {"press:", "hold:"}) {
    if (!starts_with(action, p)) continue;
    uint64_t v = 0;
    if (!parse_uint(action.substr(std::string(p).size()), 0x7f, &v)) return false;
    *method = p[0] == 'p' ? "Press" : "Hold";
    *key = static_cast<int>(v);
    return true;
  }
  return false;
}

json browse_items(const json& reply) {
  // ListItems answers a{oa{sv}}: read_json makes that one object keyed by item path.
  json out = json::array();
  const json& list = reply.is_array() && !reply.empty() ? reply[0] : json::object();
  if (!list.is_object()) return out;
  for (auto it = list.begin(); it != list.end(); ++it) {
    const json& m = it.value();
    const json md = m.contains("Metadata") ? m["Metadata"] : json::object();
    out.push_back(json{{"path", it.key()},
                       {"name", str_of(m, "Name")},
                       {"type", str_of(m, "Type")},
                       {"folder_type", str_of(m, "FolderType")},
                       {"playable", bool_of(m, "Playable")},
                       {"title", str_of(md, "Title")},
                       {"artist", str_of(md, "Artist")},
                       {"album", str_of(md, "Album")},
                       {"duration_ms", num_of(md, "Duration")}});
  }
  return out;
}

std::string class_major_name(uint32_t cod) {
  static const char* const kMajor[] = {"miscellaneous", "computer", "phone", "network",
                                       "audio/video", "peripheral", "imaging", "wearable",
                                       "toy", "health"};
  const unsigned major = (cod >> 8) & 0x1f;
  if (major < sizeof(kMajor) / sizeof(kMajor[0])) return kMajor[major];
  return major == 0x1f ? "uncategorized" : "reserved";
}

std::string appearance_name(uint16_t ap) {
  // The category is the top ten bits; the commonly seen ones, from the SIG's Assigned Numbers.
  struct Cat {
    uint16_t cat;
    const char* name;
  };
  static const Cat kCats[] = {
      {0x000, "Unknown"},         {0x001, "Phone"},          {0x002, "Computer"},
      {0x003, "Watch"},           {0x004, "Clock"},          {0x005, "Display"},
      {0x006, "Remote Control"},  {0x007, "Eye-glasses"},    {0x008, "Tag"},
      {0x009, "Keyring"},         {0x00a, "Media Player"},   {0x00b, "Barcode Scanner"},
      {0x00c, "Thermometer"},     {0x00d, "Heart Rate Sensor"},
      {0x00e, "Blood Pressure"},  {0x00f, "HID"},            {0x010, "Glucose Meter"},
      {0x011, "Running Walking Sensor"},                     {0x012, "Cycling"},
      {0x013, "Control Device"},  {0x014, "Network Device"}, {0x015, "Sensor"},
      {0x016, "Light Fixtures"},  {0x017, "Fan"},            {0x018, "HVAC"},
      {0x019, "Air Conditioning"}, {0x01a, "Humidifier"},    {0x01b, "Heating"},
      {0x01c, "Access Control"},  {0x01d, "Motorized Device"}, {0x01e, "Power Device"},
      {0x01f, "Light Source"},    {0x020, "Window Covering"}, {0x021, "Audio Sink"},
      {0x022, "Audio Source"},    {0x023, "Motorized Vehicle"}, {0x024, "Domestic Appliance"},
      {0x025, "Wearable Audio Device"}, {0x026, "Aircraft"}, {0x027, "AV Equipment"},
      {0x028, "Display Equipment"}, {0x029, "Hearing aid"},  {0x02a, "Gaming"},
      {0x02b, "Signage"},         {0x031, "Pulse Oximeter"}, {0x032, "Weight Scale"},
      {0x033, "Personal Mobility Device"}, {0x034, "Continuous Glucose Monitor"},
      {0x035, "Insulin Pump"},    {0x036, "Medication Delivery"}, {0x037, "Spirometer"},
      {0x051, "Outdoor Sports Activity"},
  };
  const uint16_t cat = ap >> 6;
  for (const Cat& c : kCats)
    if (c.cat == cat) return c.name;
  return "";
}

}  // namespace btb
