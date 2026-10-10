// /api/audio, /api/wifi, /api/kernel (the target scripts), /api/system and /api/jobs.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

  // PipeWire's timing (audio/pwtop.h): the last pw-top table, while someone follows the
  // "pipewire" topic; {"running": false} otherwise. The topic is how it is meant to be read.
  svr.Get("/api/audio/pipewire", [&d](const httplib::Request&, httplib::Response& res) {
    send_json(res, d.pwtop.last());
  });

  svr.Get("/api/audio", [&d](const httplib::Request&, httplib::Response& res) {
    reply(res, d.scripts.json_cmd("btbench-audio", {"status"}, 10000));
  });

  // Body: {"mode": "pipewire|none", "restart_bt": bool}. Switching starts or stops PipeWire and
  // WirePlumber, a few seconds on a Zero W; answered with the new status.
  svr.Put("/api/audio", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string mode = j.at("mode").get<std::string>();
    if (!audio_mode_ok(mode)) return send_error(res, 400, "mode must be pipewire or none");
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

  // ---- Log files (btbench-logd, /tmp/logs) --------------------------------------------------------

  // {dir, files: [{name, size, mtime_ms, link}]}: what is in the directory, links included (btsnoop).
  svr.Get("/api/system/logs", [opt](const httplib::Request&, httplib::Response& res) {
    json files = json::array();
    if (DIR* dir = opendir(opt.logs_dir.c_str())) {
      while (dirent* e = readdir(dir)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string p = opt.logs_dir + "/" + n;
        struct stat st {}, lst {};
        if (lstat(p.c_str(), &lst) != 0) continue;
        const bool link = S_ISLNK(lst.st_mode);
        char target[512] = {0};
        if (link && readlink(p.c_str(), target, sizeof target - 1) < 0) target[0] = 0;
        stat(p.c_str(), &st);
        files.push_back(json{{"name", n}, {"size", S_ISREG(st.st_mode) ? json(st.st_size) : json(nullptr)},
                             {"mtime_ms", static_cast<int64_t>(st.st_mtime) * 1000},
                             {"link", link ? json(std::string(target)) : json(nullptr)}});
      }
      closedir(dir);
    }
    std::sort(files.begin(), files.end(), [](const json& a, const json& b) { return a["name"] < b["name"]; });
    send_json(res, json{{"dir", opt.logs_dir}, {"files", files}});
  });

  // A file as text; ?tail=N for its last N lines, ?from=OFFSET for what came after. Only a plain
  // file directly in the directory.
  svr.Get(R"(/api/system/logs/([A-Za-z0-9_.-]+))", [opt](const httplib::Request& req, httplib::Response& res) {
    const std::string name = req.matches[1];
    const std::string p = opt.logs_dir + "/" + name;
    struct stat st {};
    if (name.empty() || name[0] == '.' || lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
      return send_error(res, 404, "no such log file");
    std::ifstream f(p, std::ios::binary);
    res.set_header("Cache-Control", "no-store");
    // ?from=OFFSET: what was appended since, in whole lines, for a viewer following the file.
    // X-Size is where to ask from next; X-Reset says the file was rotated (or the gap was over
    // 1 MiB) and the text starts afresh rather than where the viewer stopped.
    if (req.has_param("from")) {
      const long long size = static_cast<long long>(st.st_size);
      long long from = std::atoll(query(req, "from", "0").c_str());
      bool reset = false;
      if (from < 0 || from > size) from = 0, reset = true;
      constexpr long long kMax = 1 << 20;
      if (size - from > kMax) from = size - kMax, reset = true;
      std::string text(static_cast<size_t>(size - from), '\0');
      f.seekg(from);
      f.read(text.data(), static_cast<std::streamsize>(text.size()));
      text.resize(static_cast<size_t>(f.gcount()));
      size_t cut = text.rfind('\n');
      cut = cut == std::string::npos ? 0 : cut + 1;
      // Joining mid-file after a reset: start at a line boundary.
      size_t start = 0;
      if (reset && from > 0) {
        const size_t nl = text.find('\n');
        start = nl == std::string::npos || nl >= cut ? cut : nl + 1;
      }
      res.set_header("X-Size", std::to_string(from + static_cast<long long>(cut)));
      if (reset) res.set_header("X-Reset", "1");
      res.set_content(text.substr(start, cut - start), "text/plain; charset=utf-8");
      return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    res.set_header("X-Size", std::to_string(text.size()));
    const long tail = std::atol(query(req, "tail", "0").c_str());
    if (tail > 0) {
      size_t at = text.size();
      for (long n = 0; n <= tail && at > 0; ++n) {
        at = text.rfind('\n', at - 1);
        if (at == std::string::npos) { at = 0; break; }
      }
      if (at > 0) text = text.substr(at + 1);
    }
    if (query(req, "download", "") == "1") res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
    res.set_content(text, "text/plain; charset=utf-8");
  });

  // ---- Kernel Bluetooth logs (btbench-btdebug) ----------------------------------------------------

  svr.Get("/api/system/btdebug", [&d](const httplib::Request&, httplib::Response& res) {
    reply(res, d.scripts.json_cmd("btbench-btdebug", {"status"}, 15000));
  });

  // Body: {"on": ["l2cap"], "off": ["all"], "keep": ["l2cap"]} — any of them; "keep" (the areas on
  // from boot) replaces what was kept, [] keeps nothing. Answers with the status after.
  svr.Put("/api/system/btdebug", json_route([&d](const json& j, const httplib::Request&, httplib::Response& res) {
    std::vector<std::pair<std::string, std::vector<std::string>>> ops;
    for (const char* op : {"off", "on", "keep"}) {
      if (!j.contains(op)) continue;
      if (!j[op].is_array()) return send_error(res, 400, std::string(op) + " is a list of areas");
      std::vector<std::string> args{op};
      for (const json& a : j[op]) {
        if (!a.is_string() || !bt_debug_area_ok(a.get<std::string>()))
          return send_error(res, 400, "areas are hci, mgmt, l2cap, smp, iso, sco, rfcomm, bnep, hidp, driver or all");
        args.push_back(a.get<std::string>());
      }
      // on/off without an area would only print the usage.
      if (args.size() == 1 && std::string(op) != "keep") continue;
      ops.emplace_back(op, std::move(args));
    }
    for (const auto& [op, args] : ops) {
      const TargetScripts::Result r = d.scripts.action("btbench-btdebug", args, 15000);
      if (r.http != 200) return reply(res, r);
    }
    reply(res, d.scripts.json_cmd("btbench-btdebug", {"status"}, 15000));
  }));

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
