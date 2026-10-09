// /api/audio/*: the audio streams (play a test signal, a radio or UPnP URL, or a device's audio
// into a speaker; listen to any of them in the browser), the endpoints they play to and capture
// from, UPnP media servers, and the radio station list. The audio mode itself (/api/audio) is a
// target script and lives with the others in api_system.cpp.
#include "audio/engine.h"
#include "http_util.h"
#include "util/log.h"
#include "webserver.h"

using json = nlohmann::json;

namespace btb {

namespace {

bool stream_id(const httplib::Request& req, int* id) {
  const std::string s = req.matches.size() > 1 ? std::string(req.matches[1]) : std::string{};
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (s.empty() || *end || v <= 0 || v > 1000000) return false;
  *id = static_cast<int>(v);
  return true;
}

}  // namespace

void install_audio_routes(httplib::Server& svr, Deps& d) {
  audio::AudioEngine& a = d.audio;

  svr.Get("/api/audio/endpoints", [&a](const httplib::Request& req, httplib::Response& res) {
    send_json(res, a.endpoints(query(req, "fresh") == "1"));
  });

  svr.Get("/api/audio/streams", [&a](const httplib::Request&, httplib::Response& res) {
    send_json(res, a.list());
  });

  svr.Post("/api/audio/streams", json_route([&a](const json& j, const httplib::Request&, httplib::Response& res) {
    json out;
    std::string err;
    int status = 400;
    if (!a.create(j, &out, &err, &status)) return send_error(res, status, err);
    send_json(res, out, 201);
  }));

  svr.Get(R"(/api/audio/streams/(\d+))", [&a](const httplib::Request& req, httplib::Response& res) {
    int id = 0;
    if (!stream_id(req, &id)) return send_error(res, 404, "no such stream");
    const json s = a.get(id);
    if (s.is_null()) return send_error(res, 404, "no such stream");
    send_json(res, s);
  });

  svr.Put(R"(/api/audio/streams/(\d+))", json_route([&a](const json& j, const httplib::Request& req, httplib::Response& res) {
    int id = 0;
    if (!stream_id(req, &id)) return send_error(res, 404, "no such stream");
    json out;
    std::string err;
    int status = 400;
    if (!a.update(id, j, &out, &err, &status)) return send_error(res, status, err);
    send_json(res, out);
  }));

  svr.Delete(R"(/api/audio/streams/(\d+))", [&a](const httplib::Request& req, httplib::Response& res) {
    int id = 0;
    if (!stream_id(req, &id) || !a.remove(id)) return send_error(res, 404, "no such stream");
    send_ok(res);
  });

  // The stream's samples as an endless WAV: `curl -sN http://<board>/api/audio/streams/3/listen |
  // aplay`, or the console's player. ?mono=1 halves the bandwidth of a stereo stream (Wi-Fi to a
  // phone). Holds one HTTP worker while the client stays.
  svr.Get(R"(/api/audio/streams/(\d+)/listen)", [&a](const httplib::Request& req, httplib::Response& res) {
    int id = 0;
    if (!stream_id(req, &id)) return send_error(res, 404, "no such stream");
    std::string err;
    int status = 404;
    audio::AudioEngine::ListenerPtr l = a.listen(id, query(req, "mono") == "1", &err, &status);
    if (!l) return send_error(res, status, err);
    LOG_INFO("audio: stream {}: a listener joined", id);
    res.set_header("Cache-Control", "no-store");
    res.set_chunked_content_provider(
        "audio/wav",
        [l](size_t, httplib::DataSink& sink) {
          std::string chunk;
          if (!audio::AudioEngine::take(*l, &chunk, 1000)) {
            sink.done();  // the stream ended
            return true;
          }
          // Nothing for a second (a capture with nothing arriving): probe the socket, so a
          // vanished client frees its worker.
          if (chunk.empty()) return !sink.is_writable || sink.is_writable();
          return sink.write(chunk.data(), chunk.size());
        },
        [&a, id, l](bool) {
          a.unlisten(id, l);
          LOG_INFO("audio: stream {}: a listener left ({} chunks dropped)", id, l->dropped);
        });
  });

  // ---- UPnP media servers -----------------------------------------------------------------------

  // ?search=1 searches the LAN (about 2 s); without it, the last search's servers.
  svr.Get("/api/audio/upnp/servers", [&a](const httplib::Request& req, httplib::Response& res) {
    if (query(req, "search") == "1") {
      const long ms = std::clamp(query_long(req, "timeout_ms", 2500), 500L, 8000L);
      return send_json(res, a.upnp().search(static_cast<int>(ms)));
    }
    send_json(res, a.upnp().servers());
  });

  // ?server=<id>&object=<container id, "0" the root>&start=0&count=100
  svr.Get("/api/audio/upnp/browse", [&a](const httplib::Request& req, httplib::Response& res) {
    const std::string server = query(req, "server");
    if (server.empty()) return send_error(res, 400, "server is required");
    const unsigned start = static_cast<unsigned>(std::clamp(query_long(req, "start", 0), 0L, 1000000L));
    const unsigned count = static_cast<unsigned>(std::clamp(query_long(req, "count", 100), 1L, 500L));
    json out;
    std::string err;
    int status = 502;
    if (!a.upnp().browse(server, query(req, "object", "0"), start, count, &out, &err, &status))
      return send_error(res, status, err);
    send_json(res, out);
  });

  // ---- radio stations ---------------------------------------------------------------------------

  svr.Get("/api/audio/radio", [&a](const httplib::Request&, httplib::Response& res) {
    send_json(res, a.radio());
  });

  svr.Put("/api/audio/radio", json_route([&a](const json& j, const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (!a.set_radio(j, &err)) return send_error(res, 400, err);
    send_json(res, a.radio());
  }));
}

}  // namespace btb
