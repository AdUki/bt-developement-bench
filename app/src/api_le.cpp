// /api/le: advertisements (LEAdvertisingManager1) and the local GATT server (GattManager1).
#include <cstdlib>

#include "http_util.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

void install_le_routes(httplib::Server& svr, Deps& d) {
  svr.Get("/api/le/adv", [&d](const httplib::Request&, httplib::Response& res) { send_json(res, d.adv.list()); });

  // Body: an advertisement (docs/api.md). Waits for BlueZ: 200 with the instance when it is
  // advertising (or pending, with no BlueZ yet), 409 with the instance and BlueZ's reason when it
  // was refused (the controller's instances are all taken, say).
  svr.Post("/api/le/adv", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    AdvSpec spec;
    std::string err;
    if (!adv_spec_from_json(j, &spec, &err)) return send_error(res, 400, err);
    json out;
    if (!d.adv.add(spec, &out, &err)) return send_error(res, 503, err);
    if (out.value("state", std::string{}) == "failed") {
      json e = out;
      e["error"] = out.value("error", std::string("refused"));
      return send_json(res, e, 409);
    }
    send_json(res, out);
  }));

  svr.Delete("/api/le/adv/:id", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string err;
    if (!d.adv.remove(std::atoi(path_param(req, "id").c_str()), &err)) return send_error(res, 404, err);
    send_ok(res);
  });

  svr.Delete("/api/le/adv", [&d](const httplib::Request&, httplib::Response& res) {
    d.adv.remove_all();
    send_ok(res);
  });

  svr.Get("/api/le/gatt-server", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.gatt_server.state());
  });

  svr.Get("/api/le/gatt-server/example", [](const httplib::Request&, httplib::Response& res) {
    send_json(res, gatt_app_example());
  });

  // Body: an application definition (docs/api.md); replaces the current one.
  svr.Put("/api/le/gatt-server", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    GattAppDef def;
    std::string err;
    if (!gatt_app_from_json(j, &def, &err)) return send_error(res, 400, err);
    if (!d.gatt_server.set(def, &err)) {
      json s = d.gatt_server.state();
      s["error"] = err;
      return send_json(res, s, 409);
    }
    send_json(res, d.gatt_server.state());
  }));

  svr.Delete("/api/le/gatt-server", [&d](const httplib::Request&, httplib::Response& res) {
    d.gatt_server.clear();
    send_ok(res);
  });
}

}  // namespace btb
