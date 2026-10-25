// /api/le-audio: LE Audio broadcasts (Auracast), the board's own and the ones in range
// (le_audio.h). Unicast LE Audio needs nothing of its own here: a connected earbud is a PipeWire
// sink like an A2DP speaker (GET /api/media, /api/audio).
#include <systemd/sd-bus.h>

#include <algorithm>
#include <set>

#include "http_util.h"
#include "le_audio.h"
#include "mgmt.h"
#include "util/log.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

namespace {

// SetConfiguration on a broadcast PAC answers once PipeWire has taken the transport (BlueZ calls
// its endpoint in between); Select answers at once, the BIG sync comes with PipeWire's Acquire.
constexpr int kCallTimeoutMs = 10000;

json le_audio_state(BtManager& bt, audio::AudioEngine& engine) {
  json controllers = json::object();
  std::string err;
  mgmt_read_controllers(&controllers, &err);
  return model_le_audio(*bt.objects(), controllers, engine.endpoints(false));
}
json le_audio_state(Deps& d) { return le_audio_state(d.bt, d.audio); }

// Every BIS of broadcast `b`'s BIG on `apath` goes (ClearConfiguration), and PipeWire's node with it.
bool clear_big(BtManager& bt, const json& broadcasts, int big, const std::string& apath, int* n, BtCallResult* r) {
  for (const json& b : broadcasts) {
    if (b["big"] != big || "/org/bluez/" + b["adapter"].get<std::string>() != apath) continue;
    const std::string path = b["path"].get<std::string>();
    const std::string ep = path.substr(0, path.rfind('/'));
    *r = bt.call(ep, "org.bluez.MediaEndpoint1", "ClearConfiguration",
                 [path](sd_bus_message* m) { return sd_bus_message_append(m, "o", path.c_str()); }, kCallTimeoutMs);
    if (!r->ok) return false;
    ++*n;
  }
  return true;
}

void call_error(httplib::Response& res, const BtCallResult& r) {
  const int status = r.timed_out ? 504 : r.error_name == "org.bluez.Error.InvalidArguments" ? 400 : 502;
  send_json(res, json{{"error", r.error}, {"dbus_error", r.error_name}}, status);
}

// MediaEndpoint1.SetConfiguration(o, a{sv}) for one BIS. The object path is the local endpoint in
// BlueZ's API, but for a broadcast PAC BlueZ takes the one the PAC belongs to (PipeWire's) and
// ignores it; PipeWire's own path is passed all the same.
int append_bis(sd_bus_message* m, const LeBcastConfig& c) {
  int r = sd_bus_message_append(m, "o", "/MediaEndpointLE/BAPBroadcastSource/lc3");
  if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{sv}");
  auto bytes = [&](const char* k, const std::vector<uint8_t>& v) {
    if (r >= 0) r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", k);
    if (r >= 0) r = append_variant(m, DVar::bytes(v));
    if (r >= 0) r = sd_bus_message_close_container(m);
  };
  bytes("Capabilities", c.capabilities);
  bytes("Metadata", c.metadata);
  const DDict qos{{"BIG", DVar::u8(c.big)},
                  {"BIS", DVar::u8(0xff)},  // the kernel picks
                  {"SyncFactor", DVar::u8(2)},
                  {"Options", DVar::u8(0)},
                  {"Skip", DVar::u16(0)},
                  {"SyncTimeout", DVar::u16(2000)},
                  {"MSE", DVar::u8(0)},
                  {"Timeout", DVar::u16(2000)},
                  {"BCode", DVar::bytes(c.code)},
                  {"Encryption", DVar::u8(c.encrypted ? 1 : 0)},
                  {"Interval", DVar::u32(c.interval_us)},
                  {"Framing", DVar::u8(0)},
                  {"PHY", DVar::u8(0x02)},  // LE 2M
                  {"SDU", DVar::u16(c.sdu)},
                  {"Retransmissions", DVar::u8(c.preset.rtn)},
                  {"Latency", DVar::u16(c.preset.latency_ms)},
                  {"PresentationDelay", DVar::u32(c.presentation_delay_us)}};
  if (r >= 0) r = sd_bus_message_open_container(m, 'e', "sv");
  if (r >= 0) r = sd_bus_message_append(m, "s", "QoS");
  if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a{sv}");
  if (r >= 0) r = append_dict(m, qos);
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_message_close_container(m);
  return r;
}

}  // namespace

void le_audio_end_unused(BtManager& bt, audio::AudioEngine& engine, const json& outputs, std::set<std::string>* seen) {
  // Nothing to look at on a board that broadcasts nothing (the Pi Zero): no transport under a
  // broadcast PAC.
  const auto objs = bt.objects();
  bool any = false;
  for (auto it = objs->begin(); it != objs->end() && !any; ++it)
    any = it.key().find("/pac_bcast") != std::string::npos && it.key().find("/fd") != std::string::npos;
  if (!any) {
    seen->clear();
    return;
  }
  std::set<std::string> used, now;
  for (const json& k : outputs) used.insert(k.value("target", std::string{}));
  const json st = model_le_audio(*objs, json::object(), engine.endpoints(false));
  for (const json& b : st["broadcasts"]) {
    // Off air with no node yet is one being started (its node comes before SetConfiguration
    // answers, and POST then adds it): left alone.
    if (!b["node"].is_string() || b["state"] != "idle" || used.count(b["node"].get<std::string>())) continue;
    const std::string node = b["node"].get<std::string>();
    now.insert(node);
    // Unused at two sweeps running: not one that POST is adding to the outputs this moment.
    if (!seen->count(node)) continue;
    int n = 0;
    BtCallResult r;
    const int big = b["big"].get<int>();
    if (clear_big(bt, st["broadcasts"], big, "/org/bluez/" + b["adapter"].get<std::string>(), &n, &r))
      LOG_INFO("le-audio: BIG {} ended: not one of the outputs", big);
    else
      LOG_WARN("le-audio: ending BIG {}, not one of the outputs: {}", big, r.error);
    bt.touch();
  }
  *seen = now;
}

void install_le_audio_routes(httplib::Server& svr, Deps& d) {
  svr.Get("/api/le-audio", [&d](const httplib::Request&, httplib::Response& res) { send_json(res, le_audio_state(d)); });

  // Body: {adapter ("" the primary), preset, channels, code, presentation_delay_us} (le_audio.h).
  // A new BIG of one BIS; it goes on air once something plays into its node.
  svr.Post("/api/le-audio/broadcasts", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string apath = d.bt.find_adapter(j.value("adapter", std::string{}));
    if (apath.empty()) return send_error(res, 404, "no such adapter");
    const json st = le_audio_state(d);
    std::string pac;
    for (const json& a : st["adapters"])
      if ("/org/bluez/" + a["adapter"].get<std::string>() == apath) {
        if (!a["broadcaster"].get<bool>())
          return send_error(res, 409, "this adapter cannot broadcast (no ISO broadcaster in its controller)");
        if (a["pac"].is_string()) pac = a["pac"].get<std::string>();
      }
    if (pac.empty())
      return send_error(res, 409, "no broadcast endpoint on this adapter: PipeWire registers it with the "
                                  "LE Audio broadcast source role (System → Profiles)");
    const uint8_t big = le_free_big(st["broadcasts"]);
    LeBcastConfig c;
    std::string err;
    if (!le_bcast_config_from_json(j, big, &c, &err)) return send_error(res, 400, err);
    const BtCallResult r = d.bt.call(pac, "org.bluez.MediaEndpoint1", "SetConfiguration",
                                     [c](sd_bus_message* m) { return append_bis(m, c); }, kCallTimeoutMs);
    if (!r.ok) return call_error(res, r);
    d.bt.touch();
    // A broadcast is one of the route's outputs from the start: what Listen plays goes on air, and
    // taking it off the outputs ends it (le_audio_end_output).
    json out{{"big", big}, {"pac", pac}, {"node", nullptr}};
    for (const json& b : le_audio_state(d)["broadcasts"])
      if (b["big"] == big && "/org/bluez/" + b["adapter"].get<std::string>() == apath && b["node"].is_string()) out["node"] = b["node"];
    if (out["node"].is_string()) {
      json outputs = d.route.get()["outputs"];
      outputs.push_back(json{{"type", "pipewire"}, {"target", out["node"]}});
      json routed;
      std::string rerr;
      int rst = 0;
      if (!d.route.put(json{{"outputs", outputs}}, &routed, &rerr, &rst)) out["output_error"] = rerr;
    }
    send_json(res, out, 201);
  }));

  // Every BIS of the BIG goes, and PipeWire's node with it. Only off air: BlueZ clearing a stream
  // PipeWire still holds leaves its transport half gone, and the next Acquire on it crashed
  // bluetoothd (5.87). One of the route's outputs is taken off them instead (202), and the route
  // ends it once PipeWire let it go off air, a few seconds later.
  svr.Delete("/api/le-audio/broadcasts/:big", [&d](const httplib::Request& req, httplib::Response& res) {
    const std::string apath = d.bt.find_adapter(query(req, "adapter", ""));
    const int big = std::atoi(path_param(req, "big").c_str());
    const json st = le_audio_state(d);
    std::vector<std::string> nodes;
    for (const json& b : st["broadcasts"])
      if (b["big"] == big && "/org/bluez/" + b["adapter"].get<std::string>() == apath && b["node"].is_string())
        nodes.push_back(b["node"].get<std::string>());
    json outputs = json::array();
    bool routed = false;
    for (const json& k : d.route.get()["outputs"]) {
      const bool mine = std::find(nodes.begin(), nodes.end(), k.value("target", std::string{})) != nodes.end();
      routed = routed || mine;
      if (!mine) outputs.push_back(k);
    }
    if (routed) {
      json out;
      std::string err;
      int status = 0;
      if (!d.route.put(json{{"outputs", outputs}}, &out, &err, &status)) return send_error(res, status, err);
      // Ended by the route's sweep once off air (le_audio_end_unused).
      return send_json(res, json{{"ok", true}, {"ending", true}}, 202);
    }
    for (const json& b : st["broadcasts"])
      if (b["big"] == big && "/org/bluez/" + b["adapter"].get<std::string>() == apath && b["state"] != "idle")
        return send_error(res, 409, "on air: stop what plays into it first; PipeWire lets the broadcast go a few "
                                    "seconds later");
    int n = 0;
    BtCallResult r;
    if (!clear_big(d.bt, st["broadcasts"], big, apath, &n, &r)) return call_error(res, r);
    if (!n) return send_error(res, 404, "no such broadcast");
    d.bt.touch();
    send_ok(res);
  });

  // Body: {path: a broadcaster's stream (sources[].streams[].path), receive: true|false, code,
  // links}. true selects it (BlueZ syncs to the BIG when PipeWire takes it; an encrypted one needs
  // `code` first), false lets it go. `links`: the broadcaster's other streams to receive with it —
  // a BIG sync names its BISes once, when PipeWire takes the first one selected, so a BIS selected
  // after that is never synced; linked first (MediaTransport1.Links, as bluetoothctl's
  // transport.select does with several), they are synced together, and each is selected here.
  svr.Put("/api/le-audio/streams", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string path = j.value("path", std::string{});
    const auto objs = d.bt.objects();
    if (!objs->contains(path) || !(*objs)[path].contains("org.bluez.MediaTransport1") ||
        path.find("/sid") == std::string::npos)
      return send_error(res, 404, "no such broadcast stream");
    if (!j.contains("receive") || !j["receive"].is_boolean()) return send_error(res, 400, "receive is true or false");
    const bool on = j["receive"].get<bool>();
    if (on && j.contains("code") && j["code"].is_string() && !j["code"].get<std::string>().empty()) {
      std::vector<uint8_t> code;
      std::string err;
      if (!le_bcast_code(j["code"].get<std::string>(), &code, &err)) return send_error(res, 400, err);
      // The code goes in with the transport's QoS (BlueZ takes BCode alone).
      const BtCallResult r = d.bt.call(path, "org.freedesktop.DBus.Properties", "Set", [path, code](sd_bus_message* m) {
        int r = sd_bus_message_append(m, "ss", "org.bluez.MediaTransport1", "QoS");
        if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a{sv}");
        if (r >= 0) r = append_dict(m, DDict{{"BCode", DVar::bytes(code)}});
        if (r >= 0) r = sd_bus_message_close_container(m);
        return r;
      }, kCallTimeoutMs);
      if (!r.ok) return call_error(res, r);
    }
    std::vector<std::string> links;
    if (j.contains("links")) {
      if (!j["links"].is_array()) return send_error(res, 400, "links is a list of the broadcaster's other streams");
      for (const json& l : j["links"]) {
        const std::string lp = l.is_string() ? l.get<std::string>() : std::string{};
        if (lp == path) continue;
        if (!objs->contains(lp) || !(*objs)[lp].contains("org.bluez.MediaTransport1") || lp.find("/sid") == std::string::npos)
          return send_error(res, 404, "no such broadcast stream: " + lp);
        links.push_back(lp);
      }
    }
    if (on && !links.empty()) {
      const BtCallResult r = d.bt.call(path, "org.freedesktop.DBus.Properties", "Set", [&links](sd_bus_message* m) {
        int r = sd_bus_message_append(m, "ss", "org.bluez.MediaTransport1", "Links");
        if (r >= 0) r = sd_bus_message_open_container(m, 'v', "ao");
        if (r >= 0) r = sd_bus_message_open_container(m, 'a', "o");
        for (const std::string& l : links)
          if (r >= 0) r = sd_bus_message_append(m, "o", l.c_str());
        if (r >= 0) r = sd_bus_message_close_container(m);
        if (r >= 0) r = sd_bus_message_close_container(m);
        return r;
      }, kCallTimeoutMs);
      if (!r.ok) return call_error(res, r);
    }
    // The linked ones first: the first path's Select is what PipeWire acquires the BIG on.
    std::vector<std::string> all = links;
    all.push_back(path);
    for (const std::string& t : all) {
      const BtCallResult r = d.bt.call(t, "org.bluez.MediaTransport1", on ? "Select" : "Unselect",
                                       [](sd_bus_message*) { return 0; }, kCallTimeoutMs);
      if (!r.ok) return call_error(res, r);
    }
    d.bt.touch();
    send_ok(res);
  }));
}

}  // namespace btb
