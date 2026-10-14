// /api/obex: the OBEX tester (obex/obex.h) — obexd's state, client sessions and their
// operations, transfers, the server's agent and the files it works with — and the AVRCP cover
// art that rides on it (/api/media/players/cover).
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "bluez_model.h"
#include "dbus_util.h"
#include "http_util.h"
#include "obex/obex.h"
#include "util/log.h"
#include "util/strings.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

namespace {

constexpr size_t kMaxUpload = 32 * 1024 * 1024;  // /data is a card; OBEX tests do not need more

std::string content_type(const std::string& name) {
  const std::string l = lower(name);
  if (ends_with(l, ".jpg") || ends_with(l, ".jpeg")) return "image/jpeg";
  if (ends_with(l, ".png")) return "image/png";
  if (ends_with(l, ".vcf")) return "text/vcard; charset=utf-8";
  if (ends_with(l, ".txt") || ends_with(l, ".bmsg") || ends_with(l, ".xml")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

void send_file(httplib::Response& res, const std::string& path, const std::string& name) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return send_error(res, 404, "no such file");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string body = ss.str();
  // Images are labelled by what they are: a BIP responder's "JPEG" thumbnail is not always one.
  std::string type = content_type(name);
  if (body.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) type = "image/png";
  else if (body.compare(0, 3, "\xff\xd8\xff") == 0) type = "image/jpeg";
  res.set_header("Cache-Control", "no-store");
  res.set_content(body, type);
}

}  // namespace

void install_obex_routes(httplib::Server& svr, Deps& d) {
  obex::ObexManager& o = d.obex;

  svr.Get("/api/obex", [&o](const httplib::Request&, httplib::Response& res) { send_json(res, o.status()); });

  // Body: any of running (start or stop obexd), policy (auto | ask | reject: what the server's
  // agent does with an incoming push).
  svr.Put("/api/obex", json_route([&o](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (j.contains("policy") && !o.set_policy(j["policy"].get<std::string>(), &err)) return send_error(res, 400, err);
    if (j.contains("running")) {
      if (j["running"].get<bool>()) {
        if (!o.start(&err)) return send_error(res, 503, err);
      } else {
        o.stop();
      }
    }
    send_json(res, o.status());
  }));

  // An incoming push held by policy "ask": {"id", "accept", "name" (optional: save it as)}.
  svr.Post("/api/obex/request", json_route([&o](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (!o.answer(j.at("id").get<uint64_t>(), j.value("accept", false), j.value("name", std::string{}), &err))
      return send_error(res, 409, err);
  }));

  // ---- sessions ----

  // Body: {"address", "target": opp|ftp|pbap|map|bip-avrcp, "channel" / "psm" (optional)}.
  svr.Post("/api/obex/sessions", json_route([&o](const json& j, const httplib::Request&, httplib::Response& res) {
    const std::string addr = upper(j.at("address").get<std::string>());
    if (!bt_address_ok(addr)) return send_error(res, 400, "address is AA:BB:CC:DD:EE:FF");
    json out;
    std::string err;
    int status = 400;
    if (!o.create_session(addr, j.at("target").get<std::string>(), j, &out, &err, &status))
      return send_error(res, status, err);
    send_json(res, out, 201);
  }));

  svr.Delete(R"(/api/obex/sessions/([A-Za-z0-9_]+))", [&o](const httplib::Request& req, httplib::Response& res) {
    std::string err;
    int status = 404;
    if (!o.remove_session(req.matches[1], &err, &status)) return send_error(res, status, err);
    send_ok(res);
  });

  // Body: {"op": "...", ...arguments} — see docs/api.md for each profile's operations.
  svr.Post(R"(/api/obex/sessions/([A-Za-z0-9_]+))", json_route([&o](const json& j, const httplib::Request& req, httplib::Response& res) {
    json out;
    std::string err;
    int status = 400;
    if (!o.op(req.matches[1], j, &out, &err, &status)) return send_error(res, status, err);
    send_json(res, out);
  }));

  // {"action": "cancel" | "suspend" | "resume"}
  svr.Post(R"(/api/obex/transfers/([A-Za-z0-9_/]+))", json_route([&o](const json& j, const httplib::Request& req, httplib::Response& res) {
    std::string err;
    int status = 400;
    if (!o.transfer_action(req.matches[1], j.at("action").get<std::string>(), &err, &status))
      return send_error(res, status, err);
  }));

  // ---- files: in/ (received and pulled), out/ (to send), shared/ (the FTP server's folder) ----

  svr.Get(R"(/api/obex/files/(in|out|shared))", [&o](const httplib::Request& req, httplib::Response& res) {
    send_json(res, o.status()["files"][std::string(req.matches[1])]);
  });

  // The body is the file itself (Content-Type anything); ?name= says what to call it.
  svr.Post(R"(/api/obex/files/(out|shared))", [&o](const httplib::Request& req, httplib::Response& res) {
    const std::string path = o.path_of(req.matches[1], query(req, "name"));
    if (path.empty()) return send_error(res, 400, "?name= is a plain file name");
    if (req.body.size() > kMaxUpload) return send_error(res, 413, "at most 32 MiB");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return send_error(res, 500, "cannot write " + path);
    f << req.body;
    LOG_INFO("obex: stored {} ({} bytes)", path, req.body.size());
    send_json(res, json{{"name", query(req, "name")}, {"size", req.body.size()}}, 201);
  });

  svr.Get(R"(/api/obex/files/(in|out|shared)/([^/]+))", [&o](const httplib::Request& req, httplib::Response& res) {
    const std::string path = o.path_of(req.matches[1], req.matches[2]);
    if (path.empty()) return send_error(res, 400, "bad file name");
    send_file(res, path, req.matches[2]);
  });

  svr.Delete(R"(/api/obex/files/(in|out|shared)/([^/]+))", [&o](const httplib::Request& req, httplib::Response& res) {
    const std::string path = o.path_of(req.matches[1], req.matches[2]);
    if (path.empty()) return send_error(res, 400, "bad file name");
    if (unlink(path.c_str()) != 0) return send_error(res, 404, "no such file");
    send_ok(res);
  });

  // ---- AVRCP cover art ----

  // ?path=<a MediaPlayer1>[&full=1]: the current track's image, pulled over BIP from the player's
  // ObexPort (its thumbnail unless full=1), cached per image handle.
  svr.Get("/api/media/players/cover", [&o, &d](const httplib::Request& req, httplib::Response& res) {
    const std::string path = query(req, "path");
    const auto objs = d.bt.objects();
    const json m = model_media(*objs);
    // The adapter the player's device is connected on, for the BIP channel to leave from.
    const auto ad = objs->find("/org/bluez/" + adapter_from_path(path));
    const std::string source = ad != objs->end() && ad->contains("org.bluez.Adapter1")
                                   ? str_of((*ad)["org.bluez.Adapter1"], "Address")
                                   : "";
    for (const json& p : m["players"]) {
      if (p["path"] != path) continue;
      const std::string handle = p["track"].value("img_handle", std::string{});
      if (p["obex_port"].is_null()) return send_error(res, 404, "the player offers no cover art (no ObexPort)");
      if (handle.empty()) {
        // No handle: a phone sends them only once a BIP channel is open. Open it, so the next
        // track info carries one. Open already, this track came without an image.
        std::string err;
        int status = 502;
        bool opened = false;
        if (!o.cover_connect(p["address"].get<std::string>(), source, p["obex_port"].get<uint16_t>(), &opened, &err,
                             &status))
          return send_error(res, status, err);
        return send_json(res, json{{"image", false}, {"channel", opened ? "opened" : "open"}});
      }
      std::string file, err;
      int status = 502;
      if (!o.cover_art(p["address"].get<std::string>(), source, p["obex_port"].get<uint16_t>(), handle,
                       query(req, "full") != "1", &file, &err, &status))
        return send_error(res, status, err);
      return send_file(res, file, file);
    }
    send_error(res, 404, "no such player");
  });
}

}  // namespace btb
