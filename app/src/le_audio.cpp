#include "le_audio.h"

#include <algorithm>
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
constexpr const char* kEndpoint = "org.bluez.MediaEndpoint1";
constexpr const char* kTransport = "org.bluez.MediaTransport1";
const std::string kBcaa = "00001852-0000-1000-8000-00805f9b34fb";  // Broadcast Audio Announcement
const std::string kPba = "00001856-0000-1000-8000-00805f9b34fb";   // Public Broadcast Announcement

void put_ltv(std::vector<uint8_t>* v, uint8_t type, uint32_t value, int bytes) {
  v->push_back(static_cast<uint8_t>(bytes + 1));
  v->push_back(type);
  for (int i = 0; i < bytes; ++i) v->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

// LC3's Sampling_Frequency codes (Assigned Numbers 6.12.5.1).
uint8_t lc3_rate_code(unsigned rate) {
  switch (rate) {
    case 8000: return 0x01;
    case 16000: return 0x03;
    case 24000: return 0x05;
    case 32000: return 0x06;
    case 48000: return 0x08;
    default: return 0;
  }
}

std::string hciN(const std::string& path) { return adapter_from_path(path); }

json program_info_of(const std::vector<uint8_t>& m) {
  const json j = le_metadata_json(m);
  return j.contains("program_info") ? j["program_info"] : json(nullptr);
}

// The node a transport plays through: GET /api/audio/endpoints' entry whose transport it is.
json node_of(const json& endpoints, const std::string& transport) {
  for (const char* list : {"sinks", "sources"})
    if (endpoints.contains(list))
      for (const json& e : endpoints[list])
        if (e.contains("transport") && e["transport"] == transport) return e["id"];
  return nullptr;
}

// A transport's QoS (an a{sv}) in the API's words; only the keys BlueZ gave.
json qos_json(const json& q) {
  json o = json::object();
  if (!q.is_object()) return o;
  auto num = [&](const char* k, const char* as) {
    if (q.contains(k) && q[k].is_number()) o[as] = q[k];
  };
  num("BIG", "big");
  num("BIS", "bis");
  num("Interval", "interval_us");
  num("SDU", "sdu");
  num("Retransmissions", "rtn");
  num("Latency", "latency_ms");
  num("PresentationDelay", "presentation_delay_us");
  num("SyncFactor", "sync_factor");
  if (q.contains("PHY") && q["PHY"].is_number()) {
    const int p = q["PHY"].get<int>();
    o["phy"] = p == 1 ? "1M" : p == 2 ? "2M" : p == 4 ? "Coded" : std::to_string(p);
  }
  if (q.contains("Framing") && q["Framing"].is_number()) o["framed"] = q["Framing"].get<int>() != 0;
  return o;
}

// A received stream's node: by its transport, or — PipeWire puts a broadcaster's first BIS (fd0) on
// the device's own card, whose path is the device's — the device's capture node, while the stream
// is being received.
json source_node_of(const json& endpoints, const std::string& transport, const std::string& address, bool active) {
  json n = node_of(endpoints, transport);
  if (!n.is_null() || !active || !endpoints.contains("sources")) return n;
  for (const json& e : endpoints["sources"])
    if (e.value("kind", std::string{}) == "bluetooth" && e.contains("address") && e["address"] == address &&
        (!e.contains("transport") || e["transport"].is_null()))
      return e["id"];
  return nullptr;
}

bool encrypted_qos(const json& q) { return q.is_object() && q.contains("Encryption") && q["Encryption"].is_number() && q["Encryption"].get<int>() != 0; }

}  // namespace

const std::vector<LeBcastPreset>& le_bcast_presets() {
  static const std::vector<LeBcastPreset> t = [] {
    // rate, frame, octets, then low latency (rtn, ms) and high reliability (rtn, ms).
    struct Row { const char* id; unsigned rate, frame_us; uint16_t octets; uint8_t ll_rtn; uint16_t ll_ms, hr_ms; };
    static const Row rows[] = {
        {"8_1", 8000, 7500, 26, 2, 8, 45},     {"8_2", 8000, 10000, 30, 2, 10, 60},
        {"16_1", 16000, 7500, 30, 2, 8, 45},   {"16_2", 16000, 10000, 40, 2, 10, 60},
        {"24_1", 24000, 7500, 45, 2, 8, 45},   {"24_2", 24000, 10000, 60, 2, 10, 60},
        {"32_1", 32000, 7500, 60, 2, 8, 45},   {"32_2", 32000, 10000, 80, 2, 10, 60},
        {"48_1", 48000, 7500, 75, 4, 15, 50},  {"48_2", 48000, 10000, 100, 4, 20, 65},
        {"48_3", 48000, 7500, 90, 4, 15, 50},  {"48_4", 48000, 10000, 120, 4, 20, 65},
        {"48_5", 48000, 7500, 117, 4, 15, 50}, {"48_6", 48000, 10000, 155, 4, 20, 65}};
    std::vector<LeBcastPreset> v;
    for (const Row& r : rows) {
      v.push_back({std::string(r.id) + "_1", r.rate, r.frame_us, r.octets, r.ll_rtn, r.ll_ms});
      v.push_back({std::string(r.id) + "_2", r.rate, r.frame_us, r.octets, 4, r.hr_ms});
    }
    return v;
  }();
  return t;
}

bool le_bcast_code(const std::string& text, std::vector<uint8_t>* out, std::string* err) {
  out->assign(16, 0);
  if (text.empty()) return true;
  if (starts_with(text, "0x") || starts_with(text, "0X")) {
    std::vector<uint8_t> b;
    if (!from_hex(text.substr(2), &b) || b.size() != 16) {
      *err = "a hex Broadcast Code is 0x and 32 hex digits";
      return false;
    }
    *out = b;
    return true;
  }
  if (text.size() > 16) {
    *err = "a Broadcast Code is at most 16 bytes of text";
    return false;
  }
  std::copy(text.begin(), text.end(), out->begin());
  return true;
}

bool le_bcast_config_from_json(const json& j, uint8_t big, LeBcastConfig* out, std::string* err) {
  if (!j.is_object()) {
    *err = "expected an object";
    return false;
  }
  LeBcastConfig c;
  const std::string preset = j.value("preset", std::string("48_2_1"));
  const auto& ps = le_bcast_presets();
  const auto p = std::find_if(ps.begin(), ps.end(), [&](const LeBcastPreset& x) { return x.name == preset; });
  if (p == ps.end()) {
    *err = "preset is one of 8_1_1 … 48_6_2 (BAP's broadcast configurations)";
    return false;
  }
  c.preset = *p;
  c.channels = j.value("channels", 2u);
  if (c.channels != 1 && c.channels != 2) {
    *err = "channels is 1 or 2";
    return false;
  }
  // One channel is front left unless it is said to be the right one: two such broadcasts, a left
  // and a right, are one stereo programme for a receiver that takes both (the route's L/R input).
  const std::string location = j.value("location", std::string(c.channels == 1 ? "left" : ""));
  if (c.channels == 1 ? location != "left" && location != "right" : !location.empty()) {
    *err = "location is left or right, for one channel";
    return false;
  }
  c.right = location == "right";
  const std::string code = j.value("code", std::string{});
  if (!le_bcast_code(code, &c.code, err)) return false;
  c.encrypted = !code.empty();
  c.presentation_delay_us = j.value("presentation_delay_us", 40000u);
  if (c.presentation_delay_us > 0xffffff) {
    *err = "presentation_delay_us is at most 16777215";
    return false;
  }
  c.big = big;
  put_ltv(&c.capabilities, 0x01, lc3_rate_code(c.preset.rate), 1);
  put_ltv(&c.capabilities, 0x02, c.preset.frame_us == 7500 ? 0 : 1, 1);
  put_ltv(&c.capabilities, 0x03, c.channels == 1 ? (c.right ? 0x2 : 0x1) : 0x3, 4);
  put_ltv(&c.capabilities, 0x04, c.preset.octets, 2);
  put_ltv(&c.metadata, 0x02, 0x0004, 2);  // Streaming_Audio_Contexts: media
  c.sdu = static_cast<uint16_t>(c.preset.octets * c.channels);
  c.interval_us = c.preset.frame_us;
  *out = c;
  return true;
}

json le_metadata_json(const std::vector<uint8_t>& m) {
  json o = json::object();
  for (size_t i = 0; i + 1 < m.size() && m[i];) {
    const size_t len = m[i];
    if (i + 1 + len > m.size()) break;
    const uint8_t type = m[i + 1];
    const std::string v(m.begin() + static_cast<long>(i) + 2, m.begin() + static_cast<long>(i + 1 + len));
    if (type == 0x02 && v.size() == 2) o["contexts"] = static_cast<uint8_t>(v[0]) | static_cast<uint8_t>(v[1]) << 8;
    if (type == 0x03) o["program_info"] = v;  // invalid UTF-8 is replaced when sent
    if (type == 0x04 && v.size() == 3) o["language"] = v;
    i += 1 + len;
  }
  return o;
}

json model_le_audio(const json& objs, const json& controllers, const json& endpoints) {
  json adapters = json::array(), broadcasts = json::array(), sources = json::array();
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!it->contains(kAdapter)) continue;
    const json& a = (*it)[kAdapter];
    const std::string addr = str_of(a, "Address");
    bool bcaster = false, receiver = false;
    if (controllers.contains("controllers"))
      for (const json& c : controllers["controllers"])
        if (lower(c.value("address", std::string{})) == lower(addr) && c.contains("supported"))
          for (const json& s : c["supported"]) {
            if (s == "iso-broadcaster") bcaster = true;
            if (s == "sync-receiver") receiver = true;
          }
    json pac = nullptr;
    for (auto e = objs.begin(); e != objs.end(); ++e)
      if (e->contains(kEndpoint) && starts_with(e.key(), it.key() + "/pac_bcast") &&
          str_of((*e)[kEndpoint], "Device") == it.key()) {
        pac = e.key();
        break;
      }
    adapters.push_back(json{{"adapter", hciN(it.key())}, {"address", addr}, {"name", str_of(a, "Alias")},
                            {"broadcaster", bcaster}, {"receiver", receiver}, {"pac", pac},
                            {"discovering", bool_of(a, "Discovering")}});
  }

  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!it->contains(kTransport)) continue;
    const std::string& path = it.key();
    const json& t = (*it)[kTransport];
    const std::string uuid = uuid_full(str_of(t, "UUID"));
    // The board's own: under an adapter's broadcast PAC, not under a device.
    if (path.find("/dev_") != std::string::npos || path.find("/pac_bcast") == std::string::npos) continue;
    const json q = t.contains("QoS") ? t["QoS"] : json::object();
    json qos = qos_json(q);
    broadcasts.push_back(json{{"path", path},
                              {"adapter", hciN(path)},
                              {"uuid", uuid_short(uuid)},
                              {"big", qos.value("big", 0)},
                              {"bis", qos.contains("bis") ? qos["bis"] : json(nullptr)},
                              {"state", str_of(t, "State")},
                              {"codec", decode_codec(static_cast<uint8_t>(num_of(t, "Codec")), bytes_of(t, "Configuration"), false)},
                              {"qos", qos},
                              {"encrypted", encrypted_qos(q)},
                              {"node", node_of(endpoints, path)}});
  }

  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (!it->contains(kDevice)) continue;
    const std::string& dev = it.key();
    const json& d = (*it)[kDevice];
    json streams = json::array();
    for (auto t = objs.begin(); t != objs.end(); ++t) {
      if (!t->contains(kTransport) || !starts_with(t.key(), dev + "/")) continue;
      // A Broadcast Assistant's source (BASS: src_<addr>/sid0/bis1) is the remote assistant's to
      // drive, not the console's.
      if (t.key().find("/src_") != std::string::npos || t.key().find("/sid") == std::string::npos) continue;
      const json& tr = (*t)[kTransport];
      const json q = tr.contains("QoS") ? tr["QoS"] : json::object();
      const json qos = qos_json(q);
      streams.push_back(json{{"path", t.key()},
                             {"endpoint", str_of(tr, "Endpoint")},
                             {"big", qos.contains("big") ? qos["big"] : json(nullptr)},
                             {"bis", qos.contains("bis") ? qos["bis"] : json(nullptr)},
                             {"state", str_of(tr, "State")},
                             {"codec", decode_codec(static_cast<uint8_t>(num_of(tr, "Codec")), bytes_of(tr, "Configuration"), false)},
                             {"location", num_of(tr, "Location")},
                             {"metadata", to_hex(bytes_of(tr, "Metadata"))},
                             {"program_info", program_info_of(bytes_of(tr, "Metadata"))},
                             {"encrypted", encrypted_qos(q)},
                             {"node", source_node_of(endpoints, t.key(), str_of(d, "Address"),
                                                     str_of(tr, "State") == "active")}});
    }
    const json sd = d.contains("ServiceData") && d["ServiceData"].is_object() ? d["ServiceData"] : json::object();
    const std::vector<uint8_t> id = sd.contains(kBcaa) ? bytes_of(sd, kBcaa.c_str()) : std::vector<uint8_t>{};
    if (streams.empty() && id.size() < 3) continue;
    char idbuf[8] = "";
    if (id.size() >= 3) std::snprintf(idbuf, sizeof(idbuf), "%02X%02X%02X", id[2], id[1], id[0]);
    // Public Broadcast Announcement features, bit 0: the broadcast is encrypted.
    const std::vector<uint8_t> pba = sd.contains(kPba) ? bytes_of(sd, kPba.c_str()) : std::vector<uint8_t>{};
    bool enc = !pba.empty() && (pba[0] & 0x01);
    for (const json& s : streams) enc = enc || s["encrypted"].get<bool>();
    sources.push_back(json{{"address", str_of(d, "Address")},
                           {"device", dev},
                           {"adapter", hciN(dev)},
                           {"name", str_of(d, "Alias")},
                           {"rssi", d.contains("RSSI") ? json(num_of(d, "RSSI")) : json(nullptr)},
                           {"broadcast_id", idbuf[0] ? json(idbuf) : json(nullptr)},
                           {"encrypted", enc},
                           {"paired", bool_of(d, "Paired")},
                           {"connected", bool_of(d, "Connected")},
                           {"streams", streams}});
  }
  // Strongest first, the ones without a signal (no longer heard) last.
  std::stable_sort(sources.begin(), sources.end(), [](const json& a, const json& b) {
    const int ra = a["rssi"].is_number() ? a["rssi"].get<int>() : -1000;
    const int rb = b["rssi"].is_number() ? b["rssi"].get<int>() : -1000;
    return ra > rb;
  });
  return json{{"adapters", adapters}, {"broadcasts", broadcasts}, {"sources", sources}};
}

uint8_t le_free_big(const json& broadcasts) {
  for (int b = 0; b < 0xef; ++b) {
    bool used = false;
    for (const json& x : broadcasts)
      if (x.value("big", -1) == b) used = true;
    if (!used) return static_cast<uint8_t>(b);
  }
  return 0xff;
}

}  // namespace btb
