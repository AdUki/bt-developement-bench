#include "audio/endpoints.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <cctype>
#include <cstdlib>

#include "util/strings.h"

using json = nlohmann::json;

namespace btb::audio {

namespace {

std::string prop(const json& p, const char* k) {
  if (!p.contains(k)) return {};
  const json& v = p[k];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  return {};
}

// "default.audio.sink" → node name, from the "default" metadata object (its value is a JSON object
// {"name": ...}, or in some versions that object as a string).
std::string default_name(const json& dump, const char* key) {
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Metadata") continue;
    if (!o.contains("props") || o["props"].value("metadata.name", "") != "default") continue;
    if (!o.contains("metadata") || !o["metadata"].is_array()) continue;
    for (const json& m : o["metadata"]) {
      if (m.value("key", "") != key) continue;
      json v = m.contains("value") ? m["value"] : json(nullptr);
      if (v.is_string()) {
        try {
          v = json::parse(v.get<std::string>());
        } catch (const std::exception&) {
          return {};
        }
      }
      if (v.is_object()) return v.value("name", "");
    }
  }
  return {};
}

// The adapter (hci1) of a BlueZ object path: /org/bluez/hci1/dev_AA_BB_... .
std::string bt_adapter(const std::string& path) {
  const std::string root = "/org/bluez/";
  if (path.compare(0, root.size(), root) != 0) return {};
  return path.substr(root.size(), path.find('/', root.size()) - root.size());
}

// A PipeWire Bluetooth device that stands for one LE Audio broadcast stream: PipeWire makes one
// per BIS, with the transport as its api.bluez5.path — the board's own (/org/bluez/hci0/
// pac_bcast0/fd1) or one received (/org/bluez/hci0/dev_.../sid0/bis1/fd0).
bool bt_broadcast_path(const std::string& path) {
  return path.find("/pac_bcast") != std::string::npos || path.find("/sid") != std::string::npos;
}

std::string bt_address(std::string a) {
  // PipeWire names carry it as AA_BB_..., the property as AA:BB:...
  for (char& c : a)
    if (c == '_') c = ':';
  return upper(a);
}

}  // namespace

json parse_pw_dump(const std::string& text) {
  std::istringstream in(text);
  std::map<long long, json> byid;
  std::vector<long long> order;
  for (;;) {
    in >> std::ws;
    if (in.peek() == EOF) break;
    json part;
    try {
      in >> part;
    } catch (const std::exception&) {
      break;  // a truncated tail: keep what parsed
    }
    if (!part.is_array()) continue;
    for (json& o : part) {
      if (!o.is_object() || !o.contains("id")) continue;
      const long long id = o["id"].get<long long>();
      if (!byid.count(id)) order.push_back(id);
      if (o.contains("info") && o["info"].is_null()) byid.erase(id);
      else byid[id] = std::move(o);
    }
  }
  json out = json::array();
  for (long long id : order)
    if (byid.count(id)) out.push_back(byid[id]);
  return out;
}

json endpoints_from_pw_dump(const json& dump) {
  json sinks = json::array(), sources = json::array();
  if (!dump.is_array()) return json{{"sinks", sinks}, {"sources", sources}};
  // "default.audio.sink" is what is in use now; "default.configured.*" what was chosen, which may
  // name a device that is not there. Only the former marks an endpoint.
  const std::string def_sink = default_name(dump, "default.audio.sink");
  const std::string def_source = default_name(dump, "default.audio.source");
  // A Bluetooth node's adapter is its device's (api.bluez5.path): a board with two adapters can have
  // a device connected on both, and then two nodes with one node.name.
  std::map<long long, std::string> path_of;
  for (const json& o : dump)
    if (o.value("type", "") == "PipeWire:Interface:Device" && o.contains("info") && o["info"].contains("props"))
      path_of[o.value("id", -1LL)] = prop(o["info"]["props"], "api.bluez5.path");
  int broadcasts = 0;
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Node") continue;
    if (!o.contains("info") || !o["info"].contains("props")) continue;
    const json& p = o["info"]["props"];
    const std::string cls = prop(p, "media.class");
    const std::string name = prop(p, "node.name");
    if (name.empty()) continue;
    const bool sink = cls == "Audio/Sink";
    // What a Bluetooth device sends over A2DP is not an Audio/Source in PipeWire but a stream
    // (Stream/Output/Audio, bluez_input.<addr>.<n>) that plays into the default sink, the way a
    // phone's music plays on a desktop's speakers. It is still where that audio is captured from.
    const bool bt_stream = cls == "Stream/Output/Audio" &&
                           (prop(p, "device.api") == "bluez5" || starts_with(name, "bluez_input."));
    const bool source = cls == "Audio/Source" || cls == "Audio/Source/Virtual" || bt_stream;
    if (!sink && !source) continue;
    std::string label = prop(p, "node.description");
    if (label.empty()) label = prop(p, "node.nick");
    if (label.empty()) label = name;
    const std::string api = prop(p, "device.api");
    std::string kind = "hardware";
    json address = nullptr;
    json adapter = nullptr;
    json transport = nullptr;
    if (api == "bluez5" || starts_with(name, "bluez_")) {
      const std::string dev = prop(p, "device.id");
      const auto dp = dev.empty() ? path_of.end() : path_of.find(std::atoll(dev.c_str()));
      const std::string path = dp != path_of.end() ? dp->second : std::string{};
      if (!bt_adapter(path).empty()) adapter = bt_adapter(path);
      kind = "bluetooth";
      std::string a = prop(p, "api.bluez5.address");
      if (a.empty()) {
        // bluez_output.AA_BB_CC_DD_EE_FF.1
        const std::vector<std::string> parts = split(name, '.');
        if (parts.size() >= 2) a = parts[1];
      }
      if (!a.empty()) address = bt_address(a);
      if (bt_broadcast_path(path)) {
        // PipeWire names these after a made-up device ("00 00 00 00 00 00.3986", the broadcaster's
        // address with a number): say what they are, and give the transport they play.
        transport = path;
        const size_t d = path.find("/dev_");
        if (d == std::string::npos) {
          address = nullptr;
          label = "Auracast broadcast" + (broadcasts++ ? " " + std::to_string(broadcasts) : std::string{});
        } else {
          address = bt_address(path.substr(d + 5, 17));
          label = "Auracast from " + address.get<std::string>();
        }
      }
    } else if (prop(p, "alsa.id") == "Loopback" || prop(p, "api.alsa.card.name") == "Loopback" ||
               name.find("Loopback") != std::string::npos) {
      kind = "loopback";
    } else if (api.empty()) {
      kind = "virtual";
    }
    json e{{"id", name},
           {"backend", "pipewire"},
           {"direction", sink ? "sink" : "source"},
           {"label", label},
           {"kind", kind},
           {"address", address},
           {"adapter", adapter},
           {"transport", transport},
           {"profile", prop(p, "api.bluez5.profile")},
           {"codec", prop(p, "api.bluez5.codec")},
           {"serial", prop(p, "object.serial")},
           {"state", o["info"].value("state", "")},
           {"default", name == (sink ? def_sink : def_source)},
           {"rate", p.contains("audio.rate") && p["audio.rate"].is_number() ? p["audio.rate"] : json(nullptr)},
           {"channels", p.contains("audio.channels") && p["audio.channels"].is_number() ? p["audio.channels"] : json(nullptr)}};
    (sink ? sinks : sources).push_back(std::move(e));
  }
  // Two nodes of one name (a device connected on two adapters) are told apart by their adapter, and
  // named as a target by their object.serial, which PipeWire takes as well.
  for (json* list : {&sinks, &sources}) {
    std::map<std::string, int> n;
    for (const json& e : *list) ++n[e["id"].get<std::string>()];
    for (json& e : *list)
      if (n[e["id"].get<std::string>()] > 1 && !e["serial"].get<std::string>().empty()) {
        e["id"] = e["serial"];
        if (e["adapter"].is_string()) e["label"] = e["label"].get<std::string>() + " (" + e["adapter"].get<std::string>() + ")";
      }
  }
  // Bluetooth first (they are what the bench is for), then by label.
  auto order = [](const json& a, const json& b) {
    const bool ba = a["kind"] == "bluetooth", bb = b["kind"] == "bluetooth";
    if (ba != bb) return ba;
    return lower(a["label"].get<std::string>()) < lower(b["label"].get<std::string>());
  };
  std::sort(sinks.begin(), sinks.end(), order);
  std::sort(sources.begin(), sources.end(), order);
  return json{{"sinks", sinks}, {"sources", sources}};
}

json endpoints_from_alsa(bool loopback) {
  json sinks = json::array(), sources = json::array();
  if (loopback) {
    auto lb = [](const char* id, const char* dir, const char* label) {
      return json{{"id", id},       {"backend", "alsa"}, {"direction", dir},  {"label", label},
                  {"kind", "loopback"}, {"address", nullptr}, {"profile", ""}, {"codec", ""},
                  {"serial", ""},   {"state", ""},        {"default", false}, {"rate", nullptr},
                  {"channels", nullptr}};
    };
    sinks.push_back(lb("hw:Loopback,0,0", "sink", "Loopback (play into side 0)"));
    sources.push_back(lb("hw:Loopback,1,0", "source", "Loopback (what was played into side 0)"));
  }
  return json{{"sinks", sinks}, {"sources", sources}};
}

std::string profile_codec(const std::string& d) {
  const size_t c = d.rfind("codec ");
  if (c == std::string::npos) return {};
  std::string out = d.substr(c + 6);
  const size_t e = out.find(')');
  if (e != std::string::npos) out.resize(e);
  return trim(out);
}

std::string profile_kind(const std::string& n) {
  if (n == "off") return "off";
  if (starts_with(n, "a2dp-duplex")) return "a2dp-duplex";
  if (starts_with(n, "a2dp-sink")) return "a2dp";
  if (starts_with(n, "audio-gateway")) return "gateway";
  if (starts_with(n, "headset-head-unit") || starts_with(n, "hsp-hs") || starts_with(n, "hfp-hf")) return "headset";
  if (starts_with(n, "bap")) return "bap";
  return "other";
}

json cards_from_pw_dump(const json& dump) {
  json devices = json::array();
  if (!dump.is_array()) return json{{"devices", devices}};
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Device" || !o.contains("info")) continue;
    const json& info = o["info"];
    if (!info.contains("props")) continue;
    const json& p = info["props"];
    if (prop(p, "device.api") != "bluez5") continue;
    // One broadcast stream each, with no codec to choose (GET /api/le-audio shows them).
    if (bt_broadcast_path(prop(p, "api.bluez5.path"))) continue;
    auto to_profile = [](const json& pr) {
      const std::string name = pr.value("name", "");
      const std::string desc = pr.value("description", "");
      json av = pr.contains("available") ? pr["available"] : json("unknown");
      return json{{"index", pr.value("index", -1)},
                  {"name", name},
                  {"description", desc},
                  {"available", av.is_string() ? av.get<std::string>() != "no" : av.is_boolean() ? av.get<bool>() : true},
                  {"kind", profile_kind(name)},
                  {"codec", profile_codec(desc)}};
    };
    json profiles = json::array(), active = nullptr;
    const json params = info.contains("params") ? info["params"] : json::object();
    if (params.contains("EnumProfile") && params["EnumProfile"].is_array())
      for (const json& pr : params["EnumProfile"])
        if (pr.is_object()) profiles.push_back(to_profile(pr));
    if (params.contains("Profile") && params["Profile"].is_array() && !params["Profile"].empty() &&
        params["Profile"][0].is_object())
      active = to_profile(params["Profile"][0]);
    std::string address = prop(p, "api.bluez5.address");
    if (address.empty()) {
      // bluez_card.AA_BB_CC_DD_EE_FF
      const std::string n = prop(p, "device.name");
      const size_t dot = n.find('.');
      if (dot != std::string::npos) address = bt_address(n.substr(dot + 1));
    }
    std::string label = prop(p, "device.description");
    if (label.empty()) label = prop(p, "device.alias");
    devices.push_back(json{{"id", o.value("id", -1)},
                           {"name", prop(p, "device.name")},
                           {"label", label.empty() ? address : label},
                           {"address", address},
                           {"active", active},
                           {"profiles", profiles}});
  }
  return json{{"devices", devices}};
}

bool endpoint_id_ok(const std::string& id) {
  if (id.empty() || id.size() > 200 || id[0] == '-') return false;
  for (const char c : id) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != ':' && c != ',' &&
        c != '=' && c != '-')
      return false;
  }
  return true;
}

}  // namespace btb::audio
