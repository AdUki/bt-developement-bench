// /api/audio, /api/wifi, /api/kernel (the target scripts), /api/system and /api/jobs.
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "http_util.h"
#include "util/exec.h"
#include "util/log.h"
#include "util/strings.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

namespace {

void reply(httplib::Response& res, const TargetScripts::Result& r) { send_json(res, r.body, r.http); }

// Runs `cmd` after the reply has left: a reboot or a kernel trial takes the connection down, and
// the browser needs its answer before that.
void after_reply(std::function<void()> fn) {
  std::thread([fn] {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    fn();
  }).detach();
}

}  // namespace

void install_system_routes(httplib::Server& svr, Deps& d, const WebOptions& opt) {
  // ---- Audio mode (btbench-audio) ---------------------------------------------------------------

  svr.Get("/api/audio", [&d](const httplib::Request&, httplib::Response& res) {
    reply(res, d.scripts.json_cmd("btbench-audio", {"status"}, 10000));
  });

  // Body: {"mode": "pipewire|bluealsa|none", "restart_bt": bool}. Switching stops one stack and
  // starts the other, a few seconds on a Zero W; answered with the new status.
  svr.Put("/api/audio", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string mode = j.at("mode").get<std::string>();
    if (!audio_mode_ok(mode)) return send_error(res, 400, "mode must be pipewire, bluealsa or none");
    std::vector<std::string> args{"set", mode};
    if (j.value("restart_bt", false)) args.push_back("--restart-bt");
    const TargetScripts::Result r = d.scripts.action("btbench-audio", args, 60000);
    if (r.http != 200) return reply(res, r);
    reply(res, d.scripts.json_cmd("btbench-audio", {"status"}, 10000));
  }));

  // ---- Wi-Fi (btbench-wifi) -----------------------------------------------------------------------
  //
  // Adding a network or changing the mode can take down the very link the request came over (the
  // setup AP), so those are answered at once and run in the background; GET /api/wifi carries how
  // the last one went ("op").

  svr.Get("/api/wifi", [&d](const httplib::Request&, httplib::Response& res) {
    TargetScripts::Result r = d.scripts.json_cmd("btbench-wifi", {"status"}, 10000);
    if (r.http == 200 && r.body.is_object()) r.body["op"] = d.scripts.background_json("btbench-wifi");
    reply(res, r);
  });

  svr.Get("/api/wifi/scan", [&d](const httplib::Request&, httplib::Response& res) {
    reply(res, d.scripts.json_cmd("btbench-wifi", {"scan"}, 20000));
  });

  // Body: {"ssid": "...", "psk": "..."} (no psk: an open network).
  svr.Post("/api/wifi/networks", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string ssid = j.at("ssid").get<std::string>();
    const std::string psk = j.value("psk", std::string{});
    std::string err;
    if (!wifi_ssid_ok(ssid, &err) || !wifi_psk_ok(psk, &err)) return send_error(res, 400, err);
    std::vector<std::string> args{"add", ssid};
    if (!psk.empty()) args.push_back(psk);
    reply(res, d.scripts.background("btbench-wifi", args, 120000, 300));
  }));

  // ?ssid=... (an SSID may hold a slash, so it is not a path segment).
  svr.Delete("/api/wifi/networks", [&d](const httplib::Request& req, httplib::Response& res) {
    const std::string ssid = query(req, "ssid");
    std::string err;
    if (!wifi_ssid_ok(ssid, &err)) return send_error(res, 400, err);
    reply(res, d.scripts.action("btbench-wifi", {"remove", ssid}, 30000));
  });

  // Body: {"mode": "auto|sta|ap|off"}.
  svr.Put("/api/wifi/mode", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string mode = j.at("mode").get<std::string>();
    if (!wifi_mode_ok(mode)) return send_error(res, 400, "mode must be auto, sta, ap or off");
    reply(res, d.scripts.background("btbench-wifi", {"mode", mode}, 120000, 300));
  }));

  // ---- Kernel (btbench-kernel) --------------------------------------------------------------------

  svr.Get("/api/kernel", [&d](const httplib::Request&, httplib::Response& res) {
    TargetScripts::Result r = d.scripts.json_cmd("btbench-kernel", {"status"}, 10000);
    if (r.http == 200 && r.body.is_object()) r.body["op"] = d.scripts.background_json("btbench-kernel");
    reply(res, r);
  });

  // A trial boot reboots the board: answered first, then run.
  svr.Post("/api/kernel/try", [&d](const httplib::Request&, httplib::Response& res) {
    reply(res, d.scripts.background("btbench-kernel", {"try"}, 60000, 500));
  });
  for (const char* op : {"commit", "rollback"}) {
    svr.Post(std::string("/api/kernel/") + op, [&d, op](const httplib::Request&, httplib::Response& res) {
      const TargetScripts::Result r = d.scripts.action("btbench-kernel", {op}, 60000);
      if (r.http != 200) return reply(res, r);
      reply(res, d.scripts.json_cmd("btbench-kernel", {"status"}, 10000));
    });
  }

  // ---- System -------------------------------------------------------------------------------------

  svr.Get("/api/system", [&d](const httplib::Request&, httplib::Response& res) { send_json(res, d.sys.full()); });

  svr.Get("/api/system/health", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.sys.health());
  });

  svr.Get("/api/system/services", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.sys.services());
  });

  svr.Post("/api/system/services/:unit/restart", [&d, opt](const httplib::Request& req, httplib::Response& res) {
    std::string unit;
    if (!journal_unit_name(path_param(req, "unit"), &unit) || !restartable_service(unit))
      return send_error(res, 404, "not one of the services the bench restarts");
    if (!opt.allow_power) return send_error(res, 403, "service restarts are off on a PC");
    const ExecResult r = run_cmd({"systemctl", "restart", unit}, 30000);
    if (!r.ok()) return send_error(res, 500, "systemctl restart " + unit + ": " + r.reason());
    LOG_INFO("system: restarted {}", unit);
    send_json(res, d.sys.services());
  });

  svr.Get("/api/system/bluetoothd-args", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, json{{"args", read_bluetoothd_args(d.sys.data_dir())},
                        {"noplugin", read_bluetoothd_noplugin(d.sys.data_dir())},
                        {"file", d.sys.data_dir() + "/bluetoothd.env"}});
  });

  // Body: {"args": "-d -E", "restart": true}. Written to bluetoothd.env, then (by default)
  // bluetooth.service restarted so they take effect.
  svr.Put("/api/system/bluetoothd-args", json_route([&d, opt](const json& j, const httplib::Request&,
                                                              httplib::Response& res) {
    const std::string args = trim(j.at("args").get<std::string>());
    std::string err;
    if (!bluetoothd_args_ok(args, &err)) return send_error(res, 400, err);
    if (!write_bluetoothd_args(d.sys.data_dir(), args, &err)) return send_error(res, 500, err);
    LOG_INFO("system: bluetoothd arguments now \"{}\"", args);
    bool restarted = false;
    if (j.value("restart", true) && opt.allow_power) {
      const ExecResult r = run_cmd({"systemctl", "restart", "bluetooth.service"}, 30000);
      if (!r.ok()) return send_error(res, 500, "saved, but restarting bluetooth failed: " + r.reason());
      restarted = true;
    }
    send_json(res, json{{"ok", true}, {"args", args}, {"restarted", restarted}});
  }));

  // ?unit=bluetooth&lines=200: the last lines of a unit's journal.
  svr.Get("/api/system/journal", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string unit;
    if (!journal_unit_name(query(req, "unit", d.journal.unit()), &unit)) return send_error(res, 400, "bad unit name");
    const long lines = std::clamp(query_long(req, "lines", 200), 1L, 2000L);
    std::string err;
    const json j = journal_tail(unit, static_cast<int>(lines), &err);
    if (!err.empty()) return send_error(res, 503, err);
    send_json(res, json{{"unit", unit}, {"entries", j}});
  });

  // Body: {"unit": "bluetooth"}: which unit the "journal" topic follows.
  svr.Put("/api/system/journal", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string unit;
    if (!journal_unit_name(j.at("unit").get<std::string>(), &unit)) return send_error(res, 400, "bad unit name");
    d.journal.set_unit(unit);
    send_json(res, json{{"ok", true}, {"unit", unit}, {"topic", "journal"}});
  }));

  svr.Post("/api/system/reboot", [opt](const httplib::Request&, httplib::Response& res) {
    if (!opt.allow_power) return send_error(res, 403, "reboot is off on a PC");
    send_ok(res);
    after_reply([] {
      sync();
      if (std::system("systemctl reboot") != 0) LOG_ERROR("reboot failed");
    });
  });

  // ---- Jobs -------------------------------------------------------------------------------------

  svr.Get("/api/jobs", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, json{{"jobs", d.jobs.list()}, {"allowed", job_allowlist()}});
  });

  // Body: {"cmd": "l2ping", "args": ["-c", "3", "AA:BB:..."] or "-c 3 AA:BB:...", "timeout_s": N}.
  svr.Post("/api/jobs", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string cmd = j.at("cmd").get<std::string>();
    std::vector<std::string> args;
    if (j.contains("args")) {
      if (j["args"].is_string()) args = split_args(j["args"].get<std::string>());
      else args = j["args"].get<std::vector<std::string>>();
    }
    json out;
    std::string err;
    if (!d.jobs.start(cmd, args, j.value("timeout_s", 0u), &out, &err))
      return send_error(res, job_allowed(cmd) ? 409 : 403, err);
    send_json(res, out, 201);
  }));

  svr.Get("/api/jobs/:id", [&d](const httplib::Request& req, httplib::Response& res) {
    const json j = d.jobs.get(std::atoi(path_param(req, "id").c_str()));
    if (j.is_null()) return send_error(res, 404, "no such job");
    send_json(res, j);
  });

  svr.Delete("/api/jobs/:id", [&d](const httplib::Request& req, httplib::Response& res) {
    std::string err;
    if (!d.jobs.kill(std::atoi(path_param(req, "id").c_str()), &err)) return send_error(res, 404, err);
    send_ok(res);
  });
}

}  // namespace btb
