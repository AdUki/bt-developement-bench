// /api/bluetooth (adapters, discovery, devices, the agent), /api/gatt (the GATT client) and
// /api/media. Everything here returns quickly: the bus thread does the D-Bus work, and anything
// slow (a pairing can wait a minute for someone to tap their phone) reports its outcome on the
// device in the next GET. GATT reads and writes are the exception: they wait for the air, because
// the answer is the point.
#include <systemd/sd-bus.h>

#include "bluez_model.h"
#include "http_util.h"
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
  const std::string dev = d.bt.device_path(addr);
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

}  // namespace

void install_bluetooth_routes(httplib::Server& svr, Deps& d) {
  // ---- The adapter and the agent ----------------------------------------------------------------

  svr.Get("/api/bluetooth", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.bt.state());
  });

  // Body: any of powered, alias, discoverable, discoverable_timeout_s, pairable (on `adapter`, the
  // primary by default), agent ("auto"|"ask"), agent_capability.
  svr.Put("/api/bluetooth", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (j.contains("agent") || j.contains("agent_capability")) {
      if (!d.bt.set_agent(j.value("agent", std::string{}), j.value("agent_capability", std::string{}), &err))
        return send_error(res, 400, err);
    }
    json props = json::object();
    for (const char* k : {"powered", "alias", "discoverable", "discoverable_timeout_s", "pairable"})
      if (j.contains(k)) props[k] = j[k];
    if (!props.empty() && !d.bt.set_adapter_props(j.value("adapter", std::string{}), props, &err))
      return send_error(res, err.find("no ") == 0 ? 404 : 409, err);
    send_json(res, d.bt.state());
  }));

  // Body: {"on": bool, "transport": "auto|le|bredr", "rssi": -70, "duplicate_data": bool,
  // "uuids": [...], "pattern": "...", "seconds": N (0 = until stopped; default 30)}.
  svr.Post("/api/bluetooth/scan", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    BtScanFilter f;
    std::string err;
    if (!bt_scan_filter_from_json(j, &f, &err)) return send_error(res, 400, err);
    const bool on = j.value("on", true);
    const unsigned seconds = j.value("seconds", 30u);
    if (!d.bt.scan(on, f, seconds, &err)) return send_error(res, 503, err);
  }));

  // ---- Devices ----------------------------------------------------------------------------------

  svr.Get("/api/bluetooth/devices", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.bt.state()["devices"]);
  });

  svr.Get("/api/bluetooth/devices/:addr", [&d](const httplib::Request& req, httplib::Response& res) {
    const json dev = d.bt.device(path_param(req, "addr"));
    if (dev.is_null()) return send_error(res, 404, "no such device");
    send_json(res, dev);
  });

  // pair (body {"pin": "1234"} for a legacy device), cancel-pairing, connect / disconnect (body
  // {"uuid": "110b"} for one profile), trust, untrust, block, unblock.
  svr.Post("/api/bluetooth/devices/:addr/:action", [&d](const httplib::Request& req, httplib::Response& res) {
    const std::string addr = path_param(req, "addr");
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
      ok = d.bt.set_device_props(addr, json{{"trusted", act == "trust"}}, &err);
    } else if (act == "block" || act == "unblock") {
      ok = d.bt.set_device_props(addr, json{{"blocked", act == "block"}}, &err);
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
        a = act == "connect" ? BtManager::Action::Connect : BtManager::Action::Disconnect;
        arg = body.value("uuid", std::string{});
      } else {
        return send_error(res, 404, "no such action: " + act);
      }
      ok = d.bt.device_action(addr, a, arg, &err);
    }
    if (!ok) return send_error(res, err == "no such device" ? 404 : err.find("UUID") != std::string::npos ? 400 : 503, err);
    send_ok(res);
  });

  // Body: any of alias, trusted, blocked, wake_allowed.
  svr.Put("/api/bluetooth/devices/:addr", json_route([&d](const json& j, const httplib::Request& req,
                                                          httplib::Response& res) {
    std::string err;
    if (!d.bt.set_device_props(path_param(req, "addr"), j, &err))
      return send_error(res, err == "no such device" ? 404 : 409, err);
    const json dev = d.bt.device(path_param(req, "addr"));
    send_json(res, dev);
  }));

  svr.Delete("/api/bluetooth/devices/:addr", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string err;
    if (!d.bt.device_action(path_param(req, "addr"), BtManager::Action::Remove, "", &err))
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
    const std::string dev = d.bt.device_path(path_param(req, "addr"));
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
    send_json(res, json{{"handle", handle}, {"path", path}, {"value", to_hex(v)}, {"text", printable(v)}});
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
}

}  // namespace btb
