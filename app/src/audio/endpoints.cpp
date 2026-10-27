#include "audio/endpoints.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <sstream>
#include <tuple>
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

// A device-set member's object.path, "<set path>/AA:BB:CC:DD:EE:FF-<node id>" (bluez5-device.c),
// → its address; "" for anything else.
std::string set_member_address(const std::string& object_path, const std::string& set) {
  if (set.empty() || object_path.compare(0, set.size() + 1, set + "/") != 0) return {};
  std::string a = object_path.substr(set.size() + 1);
  const size_t dash = a.rfind('-');
  if (dash != std::string::npos) a.resize(dash);
  return a.size() == 17 ? bt_address(a) : std::string{};
}

// A property PipeWire may hold as a boolean or as its text ("true"): WirePlumber rewrites the
// device-set leader's api.bluez5.set.leader as a boolean, the SPA plugin emits it as a string.
bool prop_true(const json& p, const char* k) {
  if (!p.contains(k)) return false;
  const json& v = p[k];
  return v.is_boolean() ? v.get<bool>() : v.is_string() && v.get<std::string>() == "true";
}

// Each PipeWire device's api.bluez5.path, by its id ("" for a device that is not Bluetooth).
std::map<long long, std::string> device_paths(const json& dump) {
  std::map<long long, std::string> out;
  for (const json& o : dump)
    if (o.value("type", "") == "PipeWire:Interface:Device" && o.contains("info") && o["info"].contains("props"))
      out[o.value("id", -1LL)] = prop(o["info"]["props"], "api.bluez5.path");
  return out;
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
  const std::map<long long, std::string> path_of = device_paths(dump);
  // WirePlumber records a call's audio (HFP) and an LE Audio device's microphone from an internal
  // node (Audio/Source/Internal) through a loopback of its own, "bluez_input.AA:BB:…", the source
  // offered: that one carries no Bluetooth properties. Its profile and codec are its device's
  // internal node's.
  std::map<std::string, std::pair<std::string, std::string>> internal_of;
  for (const json& o : dump)
    if (o.value("type", "") == "PipeWire:Interface:Node" && o.contains("info") && o["info"].contains("props")) {
      const json& p = o["info"]["props"];
      if (prop(p, "media.class") == "Audio/Source/Internal" && !prop(p, "device.id").empty())
        internal_of[prop(p, "device.id")] = {prop(p, "api.bluez5.profile"), prop(p, "api.bluez5.codec")};
    }
  int broadcasts = 0;
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Node") continue;
    if (!o.contains("info") || !o["info"].contains("props")) continue;
    const json& p = o["info"]["props"];
    const std::string cls = prop(p, "media.class");
    const std::string name = prop(p, "node.name");
    if (name.empty()) continue;
    // A coordinated set (CSIP: a pair of LE Audio earbuds) is one combined node, the leader, which
    // plays to its members' own nodes; those are Audio/Sink/Internal. They are listed too, marked,
    // so that a client can hide them or still play to one earbud alone.
    const std::string set = prop(p, "api.bluez5.set");
    const bool set_leader = !set.empty() && prop_true(p, "api.bluez5.set.leader");
    // The other way round: what the board sends a device in a role where PipeWire records it from
    // the board's microphone (the board as HFP hands-free unit, as an LE Audio earbud) is a stream
    // too (Stream/Input/Audio, bluez_output.<addr>.<n>), and it is played into as a sink is.
    const bool bt_record = cls == "Stream/Input/Audio" &&
                           (prop(p, "device.api") == "bluez5" || starts_with(name, "bluez_output."));
    const bool sink = cls == "Audio/Sink" || bt_record || (!set.empty() && cls == "Audio/Sink/Internal");
    // What a Bluetooth device sends over A2DP is not an Audio/Source in PipeWire but a stream
    // (Stream/Output/Audio, bluez_input.<addr>.<n>) that plays into the default sink, the way a
    // phone's music plays on a desktop's speakers. It is still where that audio is captured from.
    const bool bt_stream = cls == "Stream/Output/Audio" &&
                           (prop(p, "device.api") == "bluez5" || starts_with(name, "bluez_input."));
    const bool source = cls == "Audio/Source" || cls == "Audio/Source/Virtual" || bt_stream ||
                        (!set.empty() && cls == "Audio/Source/Internal");
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
    std::string profile = prop(p, "api.bluez5.profile"), codec = prop(p, "api.bluez5.codec");
    if (profile.empty() && kind == "bluetooth") {
      const auto in = internal_of.find(prop(p, "device.id"));
      if (in != internal_of.end()) std::tie(profile, codec) = in->second;
    }
    json e{{"id", name},
           {"backend", "pipewire"},
           {"direction", sink ? "sink" : "source"},
           {"label", label},
           {"kind", kind},
           {"address", address},
           {"adapter", adapter},
           {"transport", transport},
           // PipeWire's Bluetooth node is a stream, not a device (see bt_stream, bt_record): what a
           // device sends the board in a role where the board is not the one that set it up — a
           // broadcast received, an A2DP source — and the hands-free or earbud side's sending.
           {"stream", bt_stream || bt_record},
           {"profile", profile},
           {"codec", codec},
           {"serial", prop(p, "object.serial")},
           {"state", o["info"].value("state", "")},
           {"default", name == (sink ? def_sink : def_source)},
           {"rate", p.contains("audio.rate") && p["audio.rate"].is_number() ? p["audio.rate"] : json(nullptr)},
           {"channels", p.contains("audio.channels") && p["audio.channels"].is_number() ? p["audio.channels"] : json(nullptr)},
           {"set", set.empty() ? json(nullptr) : json(set)},
           {"set_leader", set_leader}};
    if (set_leader) {
      // The leader is no one device: its address is its first member's, which says nothing.
      // api.bluez5.set.members ([{"object.path", "channels"}], as the SPA plugin emits it) names
      // the members; WirePlumber drops it from the combined node it makes, so then they are the
      // set's member nodes, filled in below.
      e["address"] = nullptr;
      json members = json::array();
      json m = p.contains("api.bluez5.set.members") ? p["api.bluez5.set.members"] : json(nullptr);
      if (m.is_string()) m = json::parse(m.get<std::string>(), nullptr, false);
      if (m.is_array())
        for (const json& x : m) {
          const std::string a = x.is_object() ? set_member_address(prop(x, "object.path"), set) : std::string{};
          if (!a.empty()) members.push_back(a);
        }
      e["members"] = members;
      e["label_set"] = prop(p, "node.description").empty();
    }
    (sink ? sinks : sources).push_back(std::move(e));
  }
  for (json* list : {&sinks, &sources})
    for (json& e : *list) {
      if (!e["set_leader"].get<bool>()) continue;
      const json* first = nullptr;
      for (const json& x : *list) {
        if (x["set"] != e["set"] || x["set_leader"].get<bool>()) continue;
        if (!first) first = &x;
        if (x["address"].is_string() &&
            std::find(e["members"].begin(), e["members"].end(), x["address"]) == e["members"].end())
          e["members"].push_back(x["address"]);
      }
      // Without a description of its own the leader is named after its node (bluez_output.<set>),
      // which is no name: "<first member> · set" says what it plays to.
      if (e["label_set"].get<bool>() && first) e["label"] = (*first)["label"].get<std::string>() + " · set";
      e.erase("label_set");
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
                  {"channels", nullptr}, {"set", nullptr}, {"set_leader", false}};
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

// ---- the PipeWire view --------------------------------------------------------------------------

namespace {

const json* props_of(const json& o) {
  return o.contains("info") && o["info"].is_object() && o["info"].contains("props") && o["info"]["props"].is_object()
             ? &o["info"]["props"] : nullptr;
}

// A metadata object's entries for the global subject (0), by key; empty without that metadata.
std::map<std::string, json> metadata(const json& dump, const char* name) {
  std::map<std::string, json> out;
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Metadata") continue;
    if (!o.contains("props") || o["props"].value("metadata.name", "") != name) continue;
    if (!o.contains("metadata") || !o["metadata"].is_array()) continue;
    for (const json& m : o["metadata"])
      if (m.is_object() && m.value("subject", -1) == 0 && m.contains("key") && m["key"].is_string())
        out[m["key"].get<std::string>()] = m.contains("value") ? m["value"] : json(nullptr);
  }
  return out;
}

// A metadata value PipeWire holds as a number or as its text ("1024"): null for anything else.
json int_value(const json& v) {
  if (v.is_number_integer()) return v;
  if (v.is_number()) return static_cast<long long>(v.get<double>());
  if (!v.is_string()) return nullptr;
  const std::string s = trim(v.get<std::string>());
  char* end = nullptr;
  const long long n = std::strtoll(s.c_str(), &end, 10);
  return !s.empty() && end && *end == '\0' ? json(n) : json(nullptr);
}

// "[ 44100, 48000 ]" (how PipeWire writes clock.allowed-rates), or a JSON array → the rates.
json rates_value(const json& v) {
  json out = json::array();
  if (v.is_array()) {
    for (const json& x : v)
      if (!int_value(x).is_null()) out.push_back(int_value(x));
    return out;
  }
  if (!v.is_string()) return nullptr;
  std::string s = v.get<std::string>();
  for (char& c : s)
    if (c == '[' || c == ']' || c == ',') c = ' ';
  std::istringstream in(s);
  for (std::string w; in >> w;)
    if (!int_value(json(w)).is_null()) out.push_back(int_value(json(w)));
  return out;
}

json opt_bool(const json& p, const char* k) {
  if (!p.contains(k)) return nullptr;
  return prop_true(p, k);
}

json opt_int(const json& p, const char* k) {
  return p.contains(k) ? int_value(p[k]) : json(nullptr);
}

json opt_str(const json& p, const char* k) {
  const std::string v = prop(p, k);
  return v.empty() ? json(nullptr) : json(v);
}

// What a node is, by its media class: the order nodes are listed in, too.
const char* const kKinds[] = {"sink", "source", "duplex", "stream_out", "stream_in", "driver", "other"};

std::string node_kind(const std::string& cls, bool driver) {
  if (starts_with(cls, "Audio/Sink")) return "sink";
  if (starts_with(cls, "Audio/Source")) return "source";
  if (starts_with(cls, "Audio/Duplex")) return "duplex";
  if (cls == "Stream/Output/Audio") return "stream_out";
  if (cls == "Stream/Input/Audio") return "stream_in";
  if (cls.empty() && driver) return "driver";
  return "other";
}

int kind_rank(const std::string& k) {
  for (int i = 0; i < static_cast<int>(std::size(kKinds)); ++i)
    if (k == kKinds[i]) return i;
  return static_cast<int>(std::size(kKinds));
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

// The node's volume as wpctl shows and sets it: the cube root of the channel volumes PipeWire
// keeps (wpctl set-volume 0.5 stores 0.125), so a slider set to a value reads back the same.
json node_volume(const json& info) {
  if (!info.contains("params") || !info["params"].contains("Props") || !info["params"]["Props"].is_array()) return nullptr;
  for (const json& pr : info["params"]["Props"]) {
    if (!pr.is_object()) continue;
    json channels = json::array();
    if (pr.contains("channelVolumes") && pr["channelVolumes"].is_array())
      for (const json& v : pr["channelVolumes"])
        if (v.is_number()) channels.push_back(round3(std::cbrt(v.get<double>())));
    if (channels.empty() && !(pr.contains("volume") && pr["volume"].is_number())) continue;
    const double vol = !channels.empty() ? channels[0].get<double>() : round3(std::cbrt(pr["volume"].get<double>()));
    return json{{"volume", vol},
                {"channels", channels},
                {"channel_map", pr.contains("channelMap") ? pr["channelMap"] : json::array()},
                {"mute", pr.value("mute", false)}};
  }
  return nullptr;
}

// The format the node runs at (its negotiated Format), else what its props say it would.
json node_format(const json& info, const json& p) {
  if (info.contains("params") && info["params"].contains("Format") && info["params"]["Format"].is_array() &&
      !info["params"]["Format"].empty() && info["params"]["Format"][0].is_object()) {
    const json& f = info["params"]["Format"][0];
    return json{{"type", f.value("mediaSubtype", "")},
                {"format", f.value("format", "")},
                {"rate", f.contains("rate") ? f["rate"] : json(nullptr)},
                {"channels", f.contains("channels") ? f["channels"] : json(nullptr)},
                {"position", f.contains("position") ? f["position"] : json::array()}};
  }
  if (!p.contains("audio.format") && !p.contains("audio.rate") && !p.contains("audio.channels")) return nullptr;
  return json{{"type", "raw"}, {"format", prop(p, "audio.format")}, {"rate", opt_int(p, "audio.rate")},
              {"channels", opt_int(p, "audio.channels")}, {"position", json::array()}};
}

}  // namespace

json clock_from_pw_dump(const json& dump) {
  if (!dump.is_array()) return nullptr;
  const std::map<std::string, json> s = metadata(dump, "settings");
  if (s.empty()) return nullptr;
  auto num = [&s](const char* k) { const auto it = s.find(k); return it == s.end() ? json(nullptr) : int_value(it->second); };
  const auto ar = s.find("clock.allowed-rates");
  return json{{"rate", num("clock.rate")},
              {"allowed_rates", ar == s.end() ? json(nullptr) : rates_value(ar->second)},
              {"quantum", num("clock.quantum")},
              {"min_quantum", num("clock.min-quantum")},
              {"max_quantum", num("clock.max-quantum")},
              {"force_rate", num("clock.force-rate")},
              {"force_quantum", num("clock.force-quantum")}};
}

json graph_from_pw_dump(const json& dump) {
  json nodes = json::array(), links = json::array();
  json defaults{{"sink", nullptr}, {"source", nullptr}, {"configured_sink", nullptr}, {"configured_source", nullptr}};
  if (!dump.is_array()) return json{{"clock", nullptr}, {"defaults", defaults}, {"nodes", nodes}, {"links", links}};
  auto def = [&dump](const char* k) { const std::string n = default_name(dump, k); return n.empty() ? json(nullptr) : json(n); };
  defaults = json{{"sink", def("default.audio.sink")},
                  {"source", def("default.audio.source")},
                  {"configured_sink", def("default.configured.audio.sink")},
                  {"configured_source", def("default.configured.audio.source")}};
  const std::map<long long, std::string> path_of = device_paths(dump);
  std::map<long long, const json*> devices, clients, ports;
  std::map<long long, bool> have_node;
  for (const json& o : dump) {
    const std::string t = o.value("type", "");
    const long long id = o.value("id", -1LL);
    if (!props_of(o)) continue;
    if (t == "PipeWire:Interface:Device") devices[id] = props_of(o);
    else if (t == "PipeWire:Interface:Client") clients[id] = props_of(o);
    else if (t == "PipeWire:Interface:Port") ports[id] = props_of(o);
    else if (t == "PipeWire:Interface:Node") have_node[id] = true;
  }
  auto is_default = [&defaults](const std::string& cls, const std::string& name, const char* sink, const char* source) {
    const json& d = defaults[starts_with(cls, "Audio/Sink") ? sink : source];
    return (starts_with(cls, "Audio/Sink") || starts_with(cls, "Audio/Source")) && d.is_string() && d == name;
  };
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Node" || !props_of(o)) continue;
    const json& info = o["info"];
    const json& p = *props_of(o);
    const long long id = o.value("id", -1LL);
    const std::string cls = prop(p, "media.class"), name = prop(p, "node.name");
    std::string desc = prop(p, "node.description");
    if (desc.empty()) desc = prop(p, "node.nick");
    if (desc.empty()) desc = name;
    json latency_ms = nullptr;
    const std::string lat = prop(p, "node.latency");
    if (const size_t sl = lat.find('/'); sl != std::string::npos) {
      const double n = std::atof(lat.substr(0, sl).c_str()), d = std::atof(lat.substr(sl + 1).c_str());
      if (d > 0) latency_ms = round3(n / d * 1000.0);
    }
    const json driver_id = opt_int(p, "node.driver-id");
    json application = nullptr;
    if (!prop(p, "application.name").empty()) {
      application = json{{"name", prop(p, "application.name")}, {"binary", opt_str(p, "application.process.binary")},
                         {"pid", opt_int(p, "application.process.id")}};
    } else if (const json c = opt_int(p, "client.id"); !c.is_null() && clients.count(c.get<long long>())) {
      const json& cp = *clients[c.get<long long>()];
      application = json{{"name", prop(cp, "application.name")}, {"binary", opt_str(cp, "application.process.binary")},
                         {"pid", opt_int(cp, "application.process.id")}};
    }
    json device = nullptr;
    const json dev_id = opt_int(p, "device.id");
    if (!dev_id.is_null() && devices.count(dev_id.get<long long>())) {
      const json& dp = *devices[dev_id.get<long long>()];
      device = json{{"id", dev_id}, {"name", prop(dp, "device.name")}, {"api", prop(dp, "device.api")},
                    {"description", prop(dp, "device.description")}};
    }
    json bluetooth = nullptr;
    if (prop(p, "device.api") == "bluez5" || starts_with(name, "bluez_")) {
      const auto pi = dev_id.is_null() ? path_of.end() : path_of.find(dev_id.get<long long>());
      const std::string path = pi != path_of.end() ? pi->second : std::string{};
      std::string a = prop(p, "api.bluez5.address");
      if (a.empty() && split(name, '.').size() >= 2) a = split(name, '.')[1];
      bluetooth = json{{"address", a.empty() ? json(nullptr) : json(bt_address(a))},
                       {"profile", prop(p, "api.bluez5.profile")},
                       {"codec", prop(p, "api.bluez5.codec")},
                       {"adapter", bt_adapter(path).empty() ? json(nullptr) : json(bt_adapter(path))},
                       {"transport", bt_broadcast_path(path) ? json(path) : json(nullptr)}};
    }
    const std::string kind = node_kind(cls, prop_true(p, "node.driver"));
    nodes.push_back(json{
        {"id", id},
        {"serial", opt_int(p, "object.serial")},
        {"name", name},
        {"nick", prop(p, "node.nick")},
        {"description", desc},
        {"media_class", cls},
        {"kind", kind},
        {"internal", ends_with(cls, "/Internal")},
        {"state", info.value("state", "")},
        {"error", info.contains("error") ? info["error"] : json(nullptr)},
        {"format", node_format(info, p)},
        {"latency", lat.empty() ? json(nullptr) : json(lat)},
        {"latency_ms", latency_ms},
        {"driver", prop_true(p, "node.driver")},
        {"driver_id", driver_id},
        {"driving", driver_id.is_number_integer() && driver_id.get<long long>() == id},
        {"priority", json{{"driver", opt_int(p, "priority.driver")}, {"session", opt_int(p, "priority.session")}}},
        {"group", opt_str(p, "node.group")},
        {"link_group", opt_str(p, "node.link-group")},
        {"media_role", opt_str(p, "media.role")},
        {"autoconnect", opt_bool(p, "node.autoconnect")},
        {"always_process", opt_bool(p, "node.always-process")},
        {"application", application},
        {"client_id", opt_int(p, "client.id")},
        {"device", device},
        {"bluetooth", bluetooth},
        {"ports", json{{"in", info.value("n-input-ports", 0)}, {"out", info.value("n-output-ports", 0)}}},
        {"volume", node_volume(info)},
        {"default", is_default(cls, name, "sink", "source")},
        {"configured", is_default(cls, name, "configured_sink", "configured_source")},
        {"can_default", (starts_with(cls, "Audio/Sink") || starts_with(cls, "Audio/Source")) && !ends_with(cls, "/Internal")},
        {"props", p}});
  }
  std::stable_sort(nodes.begin(), nodes.end(), [](const json& a, const json& b) {
    const int ka = kind_rank(a["kind"].get<std::string>()), kb = kind_rank(b["kind"].get<std::string>());
    return ka != kb ? ka < kb : a["id"].get<long long>() < b["id"].get<long long>();
  });
  // Port links, one entry a pair of nodes: a stereo stream into a sink is two links.
  std::map<std::pair<long long, long long>, json> pairs;
  std::vector<std::pair<long long, long long>> order;
  auto port_node = [&ports](const json& v) -> long long {
    const json id = int_value(v);
    if (id.is_null() || !ports.count(id.get<long long>())) return -1;
    const json n = opt_int(*ports[id.get<long long>()], "node.id");
    return n.is_null() ? -1 : n.get<long long>();
  };
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Link" || !o.contains("info") || !o["info"].is_object()) continue;
    const json& info = o["info"];
    auto node_id = [&info](const char* k) {
      const json v = info.contains(k) ? int_value(info[k]) : json(nullptr);
      return v.is_null() ? -1LL : v.get<long long>();
    };
    long long out_n = node_id("output-node-id"), in_n = node_id("input-node-id");
    const json op = info.contains("output-port-id") ? info["output-port-id"] : json(nullptr);
    const json ip = info.contains("input-port-id") ? info["input-port-id"] : json(nullptr);
    if (out_n < 0) out_n = port_node(op);
    if (in_n < 0) in_n = port_node(ip);
    if (!have_node.count(out_n) || !have_node.count(in_n)) continue;
    const auto key = std::make_pair(out_n, in_n);
    if (!pairs.count(key)) {
      order.push_back(key);
      pairs[key] = json{{"output_node", out_n}, {"input_node", in_n}, {"ports", json::array()}, {"passive", true}};
    }
    const json* opp = !int_value(op).is_null() && ports.count(int_value(op).get<long long>()) ? ports[int_value(op).get<long long>()] : nullptr;
    const json* ipp = !int_value(ip).is_null() && ports.count(int_value(ip).get<long long>()) ? ports[int_value(ip).get<long long>()] : nullptr;
    const json* lp = props_of(o);
    std::string channel = opp ? prop(*opp, "audio.channel") : std::string{};
    if (channel.empty() && ipp) channel = prop(*ipp, "audio.channel");
    json& g = pairs[key];
    g["ports"].push_back(json{{"id", o.value("id", -1LL)},
                              {"output", opp ? prop(*opp, "port.name") : std::string{}},
                              {"input", ipp ? prop(*ipp, "port.name") : std::string{}},
                              {"channel", channel},
                              {"state", info.value("state", "")},
                              {"error", info.contains("error") ? info["error"] : json(nullptr)}});
    if (!(lp && prop_true(*lp, "link.passive"))) g["passive"] = false;
  }
  for (const auto& key : order) {
    json& g = pairs[key];
    std::string state = "active";
    for (const json& l : g["ports"]) {
      const std::string s = l.value("state", "");
      if (s == "error") { state = "error"; break; }
      if (s != "active" && state == "active") state = s;
    }
    g["state"] = state;
    links.push_back(std::move(g));
  }
  return json{{"clock", clock_from_pw_dump(dump)}, {"defaults", defaults}, {"nodes", nodes}, {"links", links}};
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
