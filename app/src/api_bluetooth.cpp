// /api/bluetooth (adapters, discovery, devices, the agent), /api/gatt (the GATT client) and
// /api/media. Everything here returns quickly: the bus thread does the D-Bus work, and anything
// slow (a pairing can wait a minute for someone to tap their phone) reports its outcome on the
// device in the next GET. GATT reads and writes are the exception: they wait for the air, because
// the answer is the point.
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

#include "avdtp/probe.h"
#include "bluez_model.h"
#include "http_util.h"
#include "util/exec.h"
#include "util/strings.h"
#include "uuids.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

namespace {

constexpr int kGattTimeoutMs = 20000;  // BlueZ's own ATT timeout is 30 s; a dead link fails sooner

bool parse_handle(const std::string& s, uint16_t* out) {
  uint64_t v = 0;
  if (!parse_uint(s, 0xffff, &v) || v == 0) return false;
  *out = static_cast<uint16_t>(v);
  return true;
}

// The device and the GATT object for /api/gatt/:addr/:handle, or the reply that says why not.
bool gatt_target(Deps& d, const httplib::Request& req, httplib::Response& res, std::string* path,
                 std::string* iface, uint16_t* handle) {
  const std::string addr = path_param(req, "addr");
  const std::string dev = d.bt.device_path(addr, query(req, "adapter", ""));
  if (dev.empty()) {
    send_error(res, 404, "no such device");
    return false;
  }
  if (!parse_handle(path_param(req, "handle"), handle)) {
    send_error(res, 404, "a handle is 1..0xffff, decimal or 0x-hex");
    return false;
  }
  if (!gatt_find(*d.bt.objects(), dev, *handle, path, iface)) {
    send_error(res, 404, "no characteristic or descriptor with that handle (is the device connected "
                         "and its services resolved?)");
    return false;
  }
  return true;
}

void reply_error(httplib::Response& res, const BtCallResult& r) {
  int status = 502;  // the device or BlueZ said no
  if (r.timed_out) status = 504;
  else if (r.error_name == "org.bluez.Error.NotConnected" || r.error_name == "org.bluez.Error.InProgress")
    status = 409;
  else if (r.error_name == "org.bluez.Error.NotPermitted" || r.error_name == "org.bluez.Error.NotAuthorized" ||
           r.error_name == "org.bluez.Error.NotSupported")
    status = 403;
  else if (r.error_name == "org.bluez.Error.InvalidValueLength" || r.error_name == "org.bluez.Error.InvalidOffset" ||
           r.error_name == "org.bluez.Error.InvalidArguments")
    status = 400;
  send_json(res, json{{"error", r.error}, {"dbus_error", r.error_name}}, status);
}

// An AVRCP command goes to the phone and back; a phone with no app in front answers in well under
// a second, or not at all.
constexpr int kPlayerTimeoutMs = 5000;
// Browsing waits longer than BlueZ's own 10 s browsing timeout. BlueZ sends one browsing PDU at a
// time, and on a change to Now Playing it first asks for the folder's item count, which some phones
// (an S25, seen) never answer, so the listing after it waits the full 10 s. Giving up sooner
// leaves BlueZ still holding that listing, and every later request gets "busy".
constexpr int kBrowseTimeoutMs = 15000;

// A listing asks for the first hundred items. Without a range BlueZ asks for everything, and a
// phone (a Samsung, seen) answers with its now-playing list over and over until the count is met.
const DDict kListRange{{"Start", DVar::u32(0)}, {"End", DVar::u32(99)}};

bool media_object(const json& objs, const std::string& path, const char* iface) {
  return objs.contains(path) && objs[path].contains(iface);
}

}  // namespace

namespace {

// AVRCP browsing is one request at a time per player, through one "current folder": two overlapping
// requests (a page's queue and its library, a second tab) either get "busy" back or interleave —
// one moves the folder, the other then lists it — and items appear in the wrong list or vanish.
// Every browsing sequence (change folder, list, read the name) runs under this lock, and a busy
// answer is tried again a little later.
std::mutex g_browse_m;

bool busy(const BtCallResult& r) {
  // BlueZ answers InProgress, or passes the kernel's EBUSY on as Failed "Device or resource busy".
  return !r.ok && !r.timed_out &&
         (r.error_name == "org.bluez.Error.InProgress" || r.error.find("busy") != std::string::npos);
}

BtCallResult browse_call(BtManager& bt, const std::string& path, const std::string& iface, const std::string& method,
                         std::function<int(sd_bus_message*)> append, int timeout_ms) {
  BtCallResult r;
  for (int attempt = 0; attempt < 4; ++attempt) {
    r = bt.call(path, iface, method, append, timeout_ms);
    if (!busy(r)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(300 * (attempt + 1)));
  }
  return r;
}

}  // namespace

namespace {

// btmgmt colours its errors whether or not it writes to a terminal.
std::string strip_ansi(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      i += 2;
      while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
      continue;
    }
    out += s[i];
  }
  return out;
}

}  // namespace

// A device is named by its address, and on a board with more than one adapter optionally by which
// adapter's object of it (each adapter that has seen a device has one): ?adapter=hciN or the
// adapter's address. Without it the primary adapter's, else the first adapter's that has it.
void install_bluetooth_routes(httplib::Server& svr, Deps& d) {
  // ---- The adapter and the agent ----------------------------------------------------------------

  // BlueZ's D-Bus traffic (dbus_log.h): ?since=SEQ for what came after it, ?limit (default 500).
  svr.Get("/api/bluetooth/dbus", [&d](const httplib::Request& req, httplib::Response& res) {
    const long long since = std::max(0LL, std::atoll(query(req, "since", "0").c_str()));
    const long long limit = std::clamp(std::atoll(query(req, "limit", "500").c_str()), 1LL, 2000LL);
    send_json(res, d.dbus_log.entries(static_cast<uint64_t>(since), static_cast<size_t>(limit)));
  });

  svr.Get("/api/bluetooth", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.bt.state());
  });

  // Body: any of powered, alias, discoverable, discoverable_timeout_s, pairable (on `adapter`, hciN
  // or an address, the primary by default), primary (hciN or an address: the adapter the
  // advertisements, the GATT server and requests that name no adapter go to; "" the board's own),
  // agent ("auto"|"ask"), agent_capability, reconnect (bool).
  svr.Put("/api/bluetooth", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (j.contains("primary")) {
      if (!j["primary"].is_string()) return send_error(res, 400, "primary is an adapter: hciN or its address, or \"\"");
      if (!d.bt.set_primary(j["primary"].get<std::string>(), &err))
        return send_error(res, err == "no such adapter" ? 404 : 409, err);
    }
    if (j.contains("agent") || j.contains("agent_capability")) {
      if (!d.bt.set_agent(j.value("agent", std::string{}), j.value("agent_capability", std::string{}), &err))
        return send_error(res, 400, err);
    }
    if (j.contains("reconnect")) {
      if (!j["reconnect"].is_boolean()) return send_error(res, 400, "reconnect is true or false");
      d.bt.set_reconnect(j["reconnect"].get<bool>());
    }
    json props = json::object();
    for (const char* k : {"powered", "alias", "discoverable", "discoverable_timeout_s", "pairable"})
      if (j.contains(k)) props[k] = j[k];
    if (!props.empty() && !d.bt.set_adapter_props(j.value("adapter", std::string{}), props, &err))
      return send_error(res, err.find("no ") == 0 ? 404 : 409, err);
    send_json(res, d.bt.state());
  }));

  // Body: {"on": bool, "adapter": "hci1" (default the primary; with "on": false and none, every
  // scan started here), "transport": "auto|le|bredr", "rssi": -70, "duplicate_data": bool,
  // "uuids": [...], "pattern": "...", "seconds": N (0 = until stopped; default 30)}.
  svr.Post("/api/bluetooth/scan", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    BtScanFilter f;
    std::string err;
    if (!bt_scan_filter_from_json(j, &f, &err)) return send_error(res, 400, err);
    const bool on = j.value("on", true);
    const unsigned seconds = j.value("seconds", 30u);
    if (!d.bt.scan(on, j.value("adapter", std::string{}), f, seconds, &err))
      return send_error(res, err == "no such adapter" ? 404 : 503, err);
  }));

  // ---- Devices ----------------------------------------------------------------------------------

  svr.Get("/api/bluetooth/devices", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.bt.state()["devices"]);
  });

  svr.Get("/api/bluetooth/devices/:addr", [&d](const httplib::Request& req, httplib::Response& res) {
    const json dev = d.bt.device(path_param(req, "addr"), query(req, "adapter", ""));
    if (dev.is_null()) return send_error(res, 404, "no such device");
    send_json(res, dev);
  });

  // pair (body {"pin": "1234"} for a legacy device), cancel-pairing, connect / disconnect (body
  // {"uuid": "110b"} for one profile), trust, untrust, block, unblock.
  svr.Post("/api/bluetooth/devices/:addr/:action", [&d](const httplib::Request& req, httplib::Response& res) {
    const std::string addr = path_param(req, "addr");
    const std::string adapter = query(req, "adapter", "");
    const std::string act = path_param(req, "action");
    json body = json::object();
    if (!req.body.empty()) {
      try {
        body = json::parse(req.body);
      } catch (const std::exception& e) {
        return send_error(res, 400, e.what());
      }
    }
    std::string err;
    bool ok;
    if (act == "trust" || act == "untrust") {
      ok = d.bt.set_device_props(addr, json{{"trusted", act == "trust"}}, &err, adapter);
    } else if (act == "block" || act == "unblock") {
      ok = d.bt.set_device_props(addr, json{{"blocked", act == "block"}}, &err, adapter);
    } else {
      BtManager::Action a;
      std::string arg;
      if (act == "pair") {
        a = BtManager::Action::Pair;
        arg = body.value("pin", std::string{});
        if (arg.size() > 16) return send_error(res, 400, "a PIN is at most 16 characters");
      } else if (act == "cancel-pairing") {
        a = BtManager::Action::CancelPairing;
      } else if (act == "connect" || act == "disconnect") {
        // {"bearer": "le"|"bredr"}: that transport only (org.bluez.Bearer.*1), else the device,
        // or {"uuid"} one profile.
        if (body.contains("bearer")) {
          if (body.contains("uuid")) return send_error(res, 400, "either a bearer or a profile uuid, not both");
          a = act == "connect" ? BtManager::Action::ConnectBearer : BtManager::Action::DisconnectBearer;
          arg = body.value("bearer", std::string{});
          if (arg != "le" && arg != "bredr") return send_error(res, 400, "bearer is le or bredr");
        } else {
          a = act == "connect" ? BtManager::Action::Connect : BtManager::Action::Disconnect;
          arg = body.value("uuid", std::string{});
        }
      } else {
        return send_error(res, 404, "no such action: " + act);
      }
      ok = d.bt.device_action(addr, a, arg, &err, adapter);
    }
    if (!ok) return send_error(res, err == "no such device" ? 404 : err.find("UUID") != std::string::npos ? 400 : 503, err);
    send_ok(res);
  });

  // A connected device's link as the controller measures it, per bearer that is connected:
  // {bredr: {rssi, tx_power, max_tx_power}|null, le: ...|null}. Through btmgmt conn-info (mgmt Get
  // Connection Information), the one place that has a live link's RSSI; asked when called (the
  // console asks every two seconds while a device is open), never in the background.
  svr.Get("/api/bluetooth/devices/:addr/link", [&d](const httplib::Request& req, httplib::Response& res) {
    const json dev = d.bt.device(path_param(req, "addr"), query(req, "adapter", ""));
    if (dev.is_null()) return send_error(res, 404, "no such device");
    static const std::string btmgmt = [] {
      for (const char* p : {"/usr/bin/btmgmt", "/usr/local/bin/btmgmt"})
        if (access(p, X_OK) == 0) return std::string(p);
      return std::string{};
    }();
    if (btmgmt.empty()) return send_error(res, 503, "btmgmt is not installed");
    const std::string hci = dev.value("adapter", std::string{});
    if (hci.compare(0, 3, "hci") != 0) return send_error(res, 500, "the device's adapter is not an hciN");
    const std::string index = hci.substr(3);
    const std::string addr = dev.value("address", std::string{});
    auto ask = [&](int type) -> json {
      ExecResult r;
      json j;
      // The controller refuses one link's query right after another's now and then ("Invalid
      // Parameters"): once more, a moment later.
      for (int attempt = 0; attempt < 2 && j.is_null(); ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(150));
        r = run_cmd({btmgmt, "--index", index, "conn-info", "-t", std::to_string(type), addr}, 3000);
        j = parse_conn_info(r.out);
      }
      if (j.is_null()) return json{{"error", trim(strip_ansi(r.out.empty() ? r.reason() : r.out))}};
      // BR/EDR's Read RSSI is not a level but the distance from the controller's golden receive
      // power range: 0 is inside it, +/-n dB above or below. LE's is the level in dBm.
      j["rssi_relative"] = type == 0;
      return j;
    };
    const json& b = dev["bearers"];
    // Without bearer objects (an older BlueZ): the device's one connection, by its address type.
    const bool bredr = b["bredr"].is_object() ? b["bredr"].value("connected", false)
                                               : dev.value("connected", false) && dev.value("address_type", "") == "public" &&
                                                     !b["le"].is_object();
    const bool le = b["le"].is_object() && b["le"].value("connected", false);
    send_json(res, json{{"bredr", bredr ? ask(0) : json(nullptr)},
                        {"le", le ? ask(dev.value("address_type", "") == "random" ? 2 : 1) : json(nullptr)}});
  });

  // Body: any of alias, trusted, blocked, wake_allowed, preferred_bearer (last-used | bredr | le |
  // last-seen: which transport a dual-mode device connects on first).
  svr.Put("/api/bluetooth/devices/:addr", json_route([&d](const json& j, const httplib::Request& req,
                                                          httplib::Response& res) {
    std::string err;
    const std::string adapter = query(req, "adapter", "");
    if (!d.bt.set_device_props(path_param(req, "addr"), j, &err, adapter))
      return send_error(res, err == "no such device" ? 404 : 409, err);
    const json dev = d.bt.device(path_param(req, "addr"), adapter);
    send_json(res, dev);
  }));

  svr.Delete("/api/bluetooth/devices/:addr", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string err;
    if (!d.bt.device_action(path_param(req, "addr"), BtManager::Action::Remove, "", &err, query(req, "adapter", "")))
      return send_error(res, err == "no such device" ? 404 : 503, err);
    send_ok(res);
  });

  // ---- The agent's question ------------------------------------------------------------------

  svr.Get("/api/bluetooth/request", [&d](const httplib::Request&, httplib::Response& res) {
    BtRequest r;
    send_json(res, d.bt.has_request(&r) ? bt_request_json(r) : json(nullptr));
  });

  // Body: {"id": N, "accept": bool} plus "pin" or "passkey" for those kinds of request.
  svr.Post("/api/bluetooth/request", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string value;
    if (j.contains("pin")) value = j["pin"].get<std::string>();
    if (j.contains("passkey")) {
      const json& p = j["passkey"];
      value = p.is_string() ? p.get<std::string>() : std::to_string(p.get<long long>());
    }
    std::string err;
    if (!d.bt.answer(j.at("id").get<uint64_t>(), j.value("accept", false), value, &err))
      send_error(res, 409, err);
  }));

  // ---- GATT client ------------------------------------------------------------------------------

  svr.Get("/api/gatt/:addr", [&d](const httplib::Request& req, httplib::Response& res) {
    const std::string dev = d.bt.device_path(path_param(req, "addr"), query(req, "adapter", ""));
    if (dev.empty()) return send_error(res, 404, "no such device");
    const auto objs = d.bt.objects();
    json j = model_gatt(*objs, dev);
    const json& dp = (*objs)[dev]["org.bluez.Device1"];
    j["address"] = upper(path_param(req, "addr"));
    j["connected"] = bool_of(dp, "Connected");
    j["services_resolved"] = bool_of(dp, "ServicesResolved");
    send_json(res, j);
  });

  // A read over the air (ReadValue); ?offset=N.
  svr.Get("/api/gatt/:addr/:handle", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string path, iface;
    uint16_t handle = 0;
    if (!gatt_target(d, req, res, &path, &iface, &handle)) return;
    const uint16_t offset = static_cast<uint16_t>(query_long(req, "offset", 0));
    const BtCallResult r = d.bt.call(path, iface, "ReadValue",
                                     [offset](sd_bus_message* m) {
                                       DDict o;
                                       if (offset) o.push_back({"offset", DVar::u16(offset)});
                                       return append_dict(m, o);
                                     },
                                     kGattTimeoutMs);
    if (!r.ok) return reply_error(res, r);
    const std::vector<uint8_t> v = bytes_of(json{{"v", r.reply.empty() ? json::array() : r.reply[0]}}, "v");
    const auto objs = d.bt.objects();
    const std::string uuid = objs->contains(path) && (*objs)[path].contains(iface) ? str_of((*objs)[path][iface], "UUID") : "";
    send_json(res, json{{"handle", handle}, {"path", path}, {"value", to_hex(v)}, {"text", gatt_text(uuid, v)}});
  });

  // Body: {"value": "hex", "type": "request|command|reliable" (default request), "offset": N}.
  svr.Put("/api/gatt/:addr/:handle", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string path, iface;
    uint16_t handle = 0;
    if (!gatt_target(d, req, res, &path, &iface, &handle)) return;
    std::vector<uint8_t> value;
    std::string type = "request";
    uint16_t offset = 0;
    try {
      const json j = json::parse(req.body.empty() ? "{}" : req.body);
      if (j.contains("text")) {
        const std::string t = j["text"].get<std::string>();
        value.assign(t.begin(), t.end());
      } else if (!from_hex(j.at("value").get<std::string>(), &value)) {
        return send_error(res, 400, "value is not hex");
      }
      type = j.value("type", type);
      offset = j.value("offset", static_cast<uint16_t>(0));
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    if (type != "request" && type != "command" && type != "reliable")
      return send_error(res, 400, "type must be request, command or reliable");
    if (value.size() > 512) return send_error(res, 400, "an attribute value is at most 512 bytes");
    const BtCallResult r = d.bt.call(path, iface, "WriteValue",
                                     [value, type, offset](sd_bus_message* m) {
                                       int rc = append_bytes(m, value);
                                       DDict o{{"type", DVar::str(type)}};
                                       if (offset) o.push_back({"offset", DVar::u16(offset)});
                                       if (rc >= 0) rc = append_dict(m, o);
                                       return rc;
                                     },
                                     kGattTimeoutMs);
    if (!r.ok) return reply_error(res, r);
    send_json(res, json{{"ok", true}, {"handle", handle}, {"value", to_hex(value)}});
  });

  // Body: {"on": bool}. The values then arrive on the "gatt.notify" topic.
  svr.Post("/api/gatt/:addr/:handle/notify", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string path, iface;
    uint16_t handle = 0;
    if (!gatt_target(d, req, res, &path, &iface, &handle)) return;
    if (iface != "org.bluez.GattCharacteristic1") return send_error(res, 400, "a descriptor does not notify");
    bool on = true;
    try {
      if (!req.body.empty()) on = json::parse(req.body).value("on", true);
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    const BtCallResult r = d.bt.call(path, iface, on ? "StartNotify" : "StopNotify", nullptr, kGattTimeoutMs);
    // Already in the state asked for is success: the console's toggle may be a step behind.
    if (!r.ok && r.error_name != "org.bluez.Error.InProgress" &&
        !(r.error_name == "org.bluez.Error.Failed" && r.error.find("No notify session") != std::string::npos))
      return reply_error(res, r);
    send_json(res, json{{"ok", true}, {"handle", handle}, {"notifying", on}, {"topic", "gatt.notify"}});
  });

  // ---- Media ------------------------------------------------------------------------------------

  svr.Get("/api/media", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, model_media(*d.bt.objects()));
  });

  // Body: {"path": "<a MediaTransport1>", "volume": 0..127}: AVRCP absolute volume.
  svr.Put("/api/media/transport", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string path = j.at("path").get<std::string>();
    const auto objs = d.bt.objects();
    if (!objs->contains(path) || !(*objs)[path].contains("org.bluez.MediaTransport1"))
      return send_error(res, 404, "no such transport");
    const int v = j.at("volume").get<int>();
    if (v < 0 || v > 127) return send_error(res, 400, "volume is 0..127");
    const BtCallResult r = d.bt.set_property(path, "org.bluez.MediaTransport1", "Volume",
                                             DVar::u16(static_cast<uint16_t>(v)), 5000);
    if (!r.ok) return reply_error(res, r);
  }));

  // ---- AVDTP browsing: a device's stream endpoints, asked directly ------------------------------

  // POST: probe over an AVDTP channel of our own (pages the device; up to ?timeout_s, default 15).
  // Answers {address, seps:[...], took_ms}, or 409 when the device refuses a second channel —
  // GET /api/media's endpoints are then BlueZ's view of the same SEPs.
  svr.Post("/api/media/avdtp/:addr", [](const httplib::Request& req, httplib::Response& res) {
    const std::string addr = upper(path_param(req, "addr"));
    if (!bt_address_ok(addr)) return send_error(res, 400, "address is AA:BB:CC:DD:EE:FF");
    const int timeout_ms = static_cast<int>(std::clamp(query_long(req, "timeout_s", 15), 2L, 60L) * 1000);
    json out;
    std::string err;
    if (!avdtp::probe(addr, timeout_ms, &out, &err)) return send_error(res, err.find("refused") != std::string::npos ? 409 : 502, err);
    send_json(res, out);
  });

  // ---- AVRCP: the remote players (a phone's music app, as BlueZ's MediaPlayer1) -----------------

  // Body: {"path": "<a MediaPlayer1>", "action": "play|pause|stop|next|previous|fast-forward|
  // rewind|release|press:0x44|hold:0x4b"}. Waited for, so a player that refuses (NotSupported, a
  // phone with no app playing) says so to the button that asked.
  svr.Post("/api/media/players/control", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string path = j.at("path").get<std::string>();
    std::string action = lower(j.at("action").get<std::string>());
    if (!media_object(*d.bt.objects(), path, "org.bluez.MediaPlayer1"))
      return send_error(res, 404, "no such player");
    std::string method;
    int key = -1;
    if (!player_method(action, &method, &key))
      return send_error(res, 400, "action is play, pause, stop, next, previous, fast-forward, rewind, "
                                  "release, press:<key> or hold:<key> (an AV/C operation id, 0..0x7f)");
    const BtCallResult r = d.bt.call(path, "org.bluez.MediaPlayer1", method, [key](sd_bus_message* m) {
      return key < 0 ? 0 : sd_bus_message_append(m, "y", static_cast<uint8_t>(key));
    }, kPlayerTimeoutMs);
    if (!r.ok) return reply_error(res, r);
    d.bt.touch();
  }));

  // ---- AVRCP: the board's own player (bench_player.h), what a car or speaker browses ----------

  svr.Get("/api/media/local-player", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.player.state());
  });

  // Body: any of {"enabled": bool} (whether the player is registered with BlueZ at all),
  // {"repeat": "off|track|all"}, {"shuffle": bool} (of the stream it tells about, else its own).
  svr.Put("/api/media/local-player", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const bool en = j.contains("enabled"), rep = j.contains("repeat"), shf = j.contains("shuffle");
    if ((!en && !rep && !shf) || (en && !j["enabled"].is_boolean()) || (rep && !j["repeat"].is_string()) ||
        (shf && !j["shuffle"].is_boolean()))
      return send_error(res, 400, "body has any of {\"enabled\": bool, \"repeat\": \"off|track|all\", \"shuffle\": bool}");
    std::string err;
    if (en && !d.player.set_enabled(j["enabled"].get<bool>(), &err)) return send_error(res, 500, err);
    if (rep || shf) {
      const bool on = shf && j["shuffle"].get<bool>();
      if (!d.player.set_settings(rep ? j["repeat"].get<std::string>() : "", shf ? &on : nullptr, &err))
        return send_error(res, 400, err);
    }
    send_json(res, d.player.state());
  }));

  // Body: {"action": "play|pause|stop|next|previous"}, or {"play": <library id>} (a folder plays its
  // tracks), or {"enqueue": <library id>} (added to the now playing list).
  svr.Post("/api/media/local-player", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    bool ok;
    if (j.contains("action")) ok = d.player.action(lower(j["action"].get<std::string>()), &err);
    else if (j.contains("play")) ok = d.player.play_item(j["play"].get<int>(), false, &err);
    else if (j.contains("enqueue")) ok = d.player.play_item(j["enqueue"].get<int>(), true, &err);
    else return send_error(res, 400, "body is {\"action\"}, {\"play\": id} or {\"enqueue\": id}");
    if (!ok) return send_error(res, 400, err);
    send_json(res, d.player.state());
  }));

  // Body: {"path": "<a MediaPlayer1>"} and any of repeat (off|singletrack|alltracks|group),
  // shuffle (off|alltracks|group), equalizer (off|on), scan (off|alltracks|group).
  svr.Put("/api/media/players", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string path = j.at("path").get<std::string>();
    if (!media_object(*d.bt.objects(), path, "org.bluez.MediaPlayer1"))
      return send_error(res, 404, "no such player");
    static const std::pair<const char*, const char*> kSettings[] = {
        {"repeat", "Repeat"}, {"shuffle", "Shuffle"}, {"equalizer", "Equalizer"}, {"scan", "Scan"}};
    bool any = false;
    for (const auto& [key, prop] : kSettings) {
      if (!j.contains(key)) continue;
      any = true;
      const BtCallResult r = d.bt.set_property(path, "org.bluez.MediaPlayer1", prop,
                                               DVar::str(j[key].get<std::string>()), kPlayerTimeoutMs);
      if (!r.ok) return reply_error(res, r);
    }
    if (!any) return send_error(res, 400, "nothing to set: repeat, shuffle, equalizer or scan");
    d.bt.touch();
  }));

  // Body: {"path": "<a MediaPlayer1>", "folder": "<a folder item's path>" (optional)}: ChangeFolder
  // when asked, then ListItems. BlueZ creates the MediaItem1 objects it lists, so they also show
  // under the player in GET /api/media from then on; the answer is the listing itself.
  svr.Post("/api/media/players/browse", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string path = j.at("path").get<std::string>();
    if (!media_object(*d.bt.objects(), path, "org.bluez.MediaFolder1"))
      return send_error(res, 404, "no such player, or it is not browsable");
    const std::string folder = j.value("folder", std::string{});
    std::lock_guard<std::mutex> lk(g_browse_m);
    if (!folder.empty()) {
      const BtCallResult r = browse_call(d.bt, path, "org.bluez.MediaFolder1", "ChangeFolder", [folder](sd_bus_message* m) {
        return sd_bus_message_append(m, "o", folder.c_str());
      }, kBrowseTimeoutMs);
      if (!r.ok) return reply_error(res, r);
    }
    const BtCallResult r = browse_call(d.bt, path, "org.bluez.MediaFolder1", "ListItems", [](sd_bus_message* m) {
      return append_dict(m, kListRange);
    }, kBrowseTimeoutMs);
    if (!r.ok) return reply_error(res, r);
    d.bt.touch();
    // The folder now listed: the player's MediaFolder1 Name (re-read: ChangeFolder just moved it).
    std::string name;
    const BtCallResult n = d.bt.call(path, "org.freedesktop.DBus.Properties", "Get", [](sd_bus_message* m) {
      return sd_bus_message_append(m, "ss", "org.bluez.MediaFolder1", "Name");
    }, kBrowseTimeoutMs);
    if (n.ok && !n.reply.empty() && n.reply[0].is_string()) name = n.reply[0].get<std::string>();
    send_json(res, json{{"items", browse_items(r.reply)}, {"folder", name}});
  }));

  // Body: {"path": "<a MediaPlayer1>", "value": "text"}: MediaFolder1.Search, then the result
  // folder's listing. A player that is not Searchable answers NotSupported (403).
  svr.Post("/api/media/players/search", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string path = j.at("path").get<std::string>();
    const std::string value = j.at("value").get<std::string>();
    if (!media_object(*d.bt.objects(), path, "org.bluez.MediaFolder1"))
      return send_error(res, 404, "no such player, or it is not browsable");
    std::lock_guard<std::mutex> lk(g_browse_m);
    const BtCallResult r = browse_call(d.bt, path, "org.bluez.MediaFolder1", "Search", [value](sd_bus_message* m) {
      int rc = sd_bus_message_append(m, "s", value.c_str());
      return rc >= 0 ? append_dict(m, DDict{}) : rc;
    }, kBrowseTimeoutMs);
    if (!r.ok) return reply_error(res, r);
    const std::string folder = !r.reply.empty() && r.reply[0].is_string() ? r.reply[0].get<std::string>() : path;
    // The results are a folder of their own; some stacks list them through the player instead.
    BtCallResult l = d.bt.call(folder, "org.bluez.MediaFolder1", "ListItems", [](sd_bus_message* m) {
      return append_dict(m, kListRange);
    }, kBrowseTimeoutMs);
    if (!l.ok && folder != path)
      l = d.bt.call(path, "org.bluez.MediaFolder1", "ListItems", [](sd_bus_message* m) { return append_dict(m, kListRange); },
                    kBrowseTimeoutMs);
    if (!l.ok) return reply_error(res, l);
    d.bt.touch();
    send_json(res, json{{"items", browse_items(l.reply)}, {"folder", "search: " + value}, {"result", folder}});
  }));

  // Body: {"path": "<a MediaItem1>", "action": "play|add"}: play it now, or AddtoNowPlaying.
  svr.Post("/api/media/items", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string path = j.at("path").get<std::string>();
    if (!media_object(*d.bt.objects(), path, "org.bluez.MediaItem1"))
      return send_error(res, 404, "no such item (browse the player first)");
    const std::string action = j.value("action", std::string("play"));
    if (action != "play" && action != "add") return send_error(res, 400, "action is play or add");
    const BtCallResult r = d.bt.call(path, "org.bluez.MediaItem1", action == "play" ? "Play" : "AddtoNowPlaying",
                                     nullptr, kPlayerTimeoutMs);
    if (!r.ok) return reply_error(res, r);
    d.bt.touch();
  }));
}

}  // namespace btb
