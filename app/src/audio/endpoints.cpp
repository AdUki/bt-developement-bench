#include "audio/endpoints.h"

#include <algorithm>
#include <cctype>

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

std::string bt_address(std::string a) {
  // PipeWire names carry it as AA_BB_..., the property as AA:BB:...
  for (char& c : a)
    if (c == '_') c = ':';
  return upper(a);
}

}  // namespace

json endpoints_from_pw_dump(const json& dump) {
  json sinks = json::array(), sources = json::array();
  if (!dump.is_array()) return json{{"sinks", sinks}, {"sources", sources}};
  // "default.audio.sink" is what is in use now; "default.configured.*" what was chosen, which may
  // name a device that is not there. Only the former marks an endpoint.
  const std::string def_sink = default_name(dump, "default.audio.sink");
  const std::string def_source = default_name(dump, "default.audio.source");
  for (const json& o : dump) {
    if (o.value("type", "") != "PipeWire:Interface:Node") continue;
    if (!o.contains("info") || !o["info"].contains("props")) continue;
    const json& p = o["info"]["props"];
    const std::string cls = prop(p, "media.class");
    const bool sink = cls == "Audio/Sink";
    const bool source = cls == "Audio/Source" || cls == "Audio/Source/Virtual";
    if (!sink && !source) continue;
    const std::string name = prop(p, "node.name");
    if (name.empty()) continue;
    std::string label = prop(p, "node.description");
    if (label.empty()) label = prop(p, "node.nick");
    if (label.empty()) label = name;
    const std::string api = prop(p, "device.api");
    std::string kind = "hardware";
    json address = nullptr;
    if (api == "bluez5" || starts_with(name, "bluez_")) {
      kind = "bluetooth";
      std::string a = prop(p, "api.bluez5.address");
      if (a.empty()) {
        // bluez_output.AA_BB_CC_DD_EE_FF.1
        const std::vector<std::string> parts = split(name, '.');
        if (parts.size() >= 2) a = parts[1];
      }
      if (!a.empty()) address = bt_address(a);
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
           {"profile", prop(p, "api.bluez5.profile")},
           {"codec", prop(p, "api.bluez5.codec")},
           {"serial", prop(p, "object.serial")},
           {"state", o["info"].value("state", "")},
           {"default", name == (sink ? def_sink : def_source)},
           {"rate", p.contains("audio.rate") && p["audio.rate"].is_number() ? p["audio.rate"] : json(nullptr)},
           {"channels", p.contains("audio.channels") && p["audio.channels"].is_number() ? p["audio.channels"] : json(nullptr)}};
    (sink ? sinks : sources).push_back(std::move(e));
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

json endpoints_from_alsa(const json& media, bool loopback) {
  json sinks = json::array(), sources = json::array();
  if (media.contains("transports") && media["transports"].is_array()) {
    for (const json& t : media["transports"]) {
      const std::string uuid = t.value("uuid", "");
      const std::string addr = t.value("address", "");
      if (addr.empty()) continue;
      const bool a2dp_src = uuid == "110a", a2dp_snk = uuid == "110b";
      const bool sco = uuid == "111e" || uuid == "111f" || uuid == "1108" || uuid == "1112";
      if (!a2dp_src && !a2dp_snk && !sco) continue;
      const std::string profile = sco ? "sco" : "a2dp";
      const json codec = t.contains("codec") ? t["codec"] : json::object();
      json e{{"id", "bluealsa:DEV=" + addr + ",PROFILE=" + profile},
             {"backend", "alsa"},
             {"label", addr + " (" + (sco ? std::string("HFP/HSP") : std::string("A2DP")) + ", " +
                           codec.value("name", std::string("?")) + ")"},
             {"kind", "bluetooth"},
             {"address", addr},
             {"profile", profile},
             {"codec", codec.value("name", std::string{})},
             {"serial", ""},
             {"state", t.value("state", "")},
             {"default", false},
             {"rate", codec.contains("rate") && codec["rate"].is_number() ? codec["rate"] : json(nullptr)},
             {"channels", nullptr}};
      if (a2dp_src || sco) {
        json s = e;
        s["direction"] = "sink";
        sinks.push_back(std::move(s));
      }
      if (a2dp_snk || sco) {
        e["direction"] = "source";
        sources.push_back(std::move(e));
      }
    }
  }
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
