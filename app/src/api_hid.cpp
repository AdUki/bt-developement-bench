// /api/le/hid: the board as a HID over GATT remote (hid_device.h). /api/hid: the board as a HID
// host — the Bluetooth input devices connected to it and what they send (hid_host.h).
#include "http_util.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

void install_hid_routes(httplib::Server& svr, Deps& d) {
  svr.Get("/api/le/hid", [&d](const httplib::Request&, httplib::Response& res) { send_json(res, d.hid_remote.state()); });

  // Body: {"enabled": true, "name": "btbench remote"} — registers the HID service and advertises;
  // {"enabled": false} removes both. {"battery": 0..100} sets the battery level hosts show.
  svr.Put("/api/le/hid", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (j.contains("enabled")) {
      if (j["enabled"].get<bool>()) {
        if (!d.bt.running()) return send_error(res, 503, "Bluetooth is not running");
        if (!d.hid_remote.enable(j.value("name", std::string{}), &err)) return send_error(res, 409, err);
      } else {
        d.hid_remote.disable();
      }
    }
    if (j.contains("battery") && !d.hid_remote.set_battery(j["battery"].get<int>(), &err))
      return send_error(res, 400, err);
    send_json(res, d.hid_remote.state());
  }));

  // Body: {"key": "play_pause", "action": "tap|down|up", "hold_ms": 80}.
  svr.Post("/api/le/hid/key", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (!d.hid_remote.key(j.at("key").get<std::string>(), j.value("action", std::string("tap")),
                          j.value("hold_ms", 0u), &err))
      return send_error(res, err == "the remote is off" ? 409 : 400, err);
    send_ok(res);
  }));

  // Body: {"text": "hello"}: typed on the keyboard report.
  svr.Post("/api/le/hid/type", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (!d.hid_remote.type(j.at("text").get<std::string>(), &err))
      return send_error(res, err == "the remote is off" ? 409 : 400, err);
    send_ok(res);
  }));

  svr.Get("/api/hid", [&d](const httplib::Request&, httplib::Response& res) { send_json(res, d.hid_host.state()); });

  // Body: {"media_keys": true}: a connected remote's media keys drive the board's player.
  svr.Put("/api/hid", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    if (j.contains("media_keys")) {
      d.hid_host.set_media_keys(j["media_keys"].get<bool>());
    }
    send_json(res, d.hid_host.state());
  }));
}

}  // namespace btb
