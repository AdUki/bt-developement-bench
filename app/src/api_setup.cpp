// /api/setup: profiles, BlueZ plugins and the main.conf / WirePlumber settings (setup.h).
#include "http_util.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

void install_setup_routes(httplib::Server& svr, Deps& d) {
  svr.Get("/api/setup", [&d](const httplib::Request&, httplib::Response& res) { send_json(res, d.setup.state()); });

  // Body: {"profiles": {"a2dp_source": false}, "plugins": {"policy": false},
  //        "settings": {"General.ControllerMode": "le", "LE.MinConnectionInterval": 7.5, "GATT.Cache": null}}.
  // Answers with the new state and what was restarted (bluetooth.service, wireplumber.service, obexd).
  svr.Put("/api/setup", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    json out;
    std::string err;
    int status = 400;
    if (!d.setup.change(j, &out, &err, &status)) return send_error(res, status, err);
    send_json(res, out);
  }));
}

}  // namespace btb
