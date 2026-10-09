// HTTP routes of the HCI monitor: /api/hci/* and /api/capture*.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <httplib.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

#include "capture.h"
#include "hci_monitor.h"
#include "monitor_impl.h"
#include "pcap.h"

namespace btb::hci {

using nlohmann::json;

namespace {

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& msg) {
  send_json(res, json{{"error", msg}}, status);
}

// The monitor's routes all answer 503 with the reason when the source never opened.
bool require_source(const Monitor& mon, httplib::Response& res) {
  if (mon.available()) return true;
  send_error(res, 503, "HCI monitor unavailable: " + mon.error());
  return false;
}

bool int_param(const httplib::Request& req, const char* name, int* out) {
  if (!req.has_param(name)) return false;
  const std::string v = req.get_param_value(name);
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 0);
  if (v.empty() || *end) return false;
  *out = static_cast<int>(n);
  return true;
}

bool conn_params(const httplib::Request& req, httplib::Response& res, int* index, int* handle) {
  if (!int_param(req, "handle", handle)) {
    send_error(res, 400, "handle (and index, default 0) required");
    return false;
  }
  if (!int_param(req, "index", index)) *index = 0;
  return true;
}

}  // namespace

void register_routes(httplib::Server& svr, Monitor& mon) {
  Monitor* m = &mon;
  const std::shared_ptr<PcapFeed> feed = mon.impl().feed;
  const std::shared_ptr<Capture> cap = mon.impl().capture;

  svr.Get("/api/hci/stats", [m](const httplib::Request&, httplib::Response& res) {
    if (!require_source(*m, res)) return;
    send_json(res, m->stats());
  });

  svr.Get("/api/hci/history", [m](const httplib::Request& req, httplib::Response& res) {
    if (!require_source(*m, res)) return;
    int index = 0, handle = 0, seconds = 300;
    if (!conn_params(req, res, &index, &handle)) return;
    int_param(req, "seconds", &seconds);
    json out;
    if (!m->history(index, handle, seconds, &out)) return send_error(res, 404, "no such connection");
    send_json(res, out);
  });

  svr.Get("/api/hci/latency", [m](const httplib::Request& req, httplib::Response& res) {
    if (!require_source(*m, res)) return;
    int index = 0, handle = 0;
    if (!conn_params(req, res, &index, &handle)) return;
    json out;
    if (!m->latency(index, handle, &out)) return send_error(res, 404, "no such connection");
    send_json(res, out);
  });

  svr.Get("/api/hci/events", [m](const httplib::Request& req, httplib::Response& res) {
    if (!require_source(*m, res)) return;
    uint64_t since = 0;
    if (req.has_param("since")) since = std::strtoull(req.get_param_value("since").c_str(), nullptr, 10);
    send_json(res, m->events(since));
  });

  svr.Post("/api/hci/mark", [m](const httplib::Request& req, httplib::Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("text") ||
        !body["text"].is_string()) {
      return send_error(res, 400, "body must be {\"text\":\"...\"}");
    }
    send_json(res, m->mark(body["text"].get<std::string>()));
  });

  // Live capture: `curl -sN http://<board>/api/hci/live.pcap | wireshark -k -i -`. Holds one HTTP
  // worker for as long as the client stays connected.
  svr.Get("/api/hci/live.pcap", [m, feed](const httplib::Request&, httplib::Response& res) {
    if (!require_source(*m, res)) return;
    PcapFeed::SubPtr sub = feed->subscribe();
    if (!sub) {
      return send_error(res, 503,
                        "too many live captures (" + std::to_string(PcapFeed::kMaxSubscribers) +
                            " max): each one holds an HTTP worker");
    }
    spdlog::info("HCI monitor: live pcap stream opened");
    res.set_header("Cache-Control", "no-store");
    res.set_chunked_content_provider(
        "application/vnd.tcpdump.pcap",
        [sub](size_t, httplib::DataSink& sink) {
          std::string chunk;
          if (!PcapFeed::take(*sub, &chunk, 1000)) {
            sink.done();  // the monitor is shutting down
            return true;
          }
          // Nothing for a second: probe the socket so a vanished client frees the worker.
          if (chunk.empty()) return !sink.is_writable || sink.is_writable();
          return sink.write(chunk.data(), chunk.size());
        },
        [feed, sub](bool) {
          feed->unsubscribe(sub);
          std::lock_guard<std::mutex> lk(sub->m);
          spdlog::info("HCI monitor: live pcap stream closed ({} packets sent, {} dropped)",
                       sub->sent_records, sub->dropped_records);
        });
  });

  // ---- capture ring ----

  svr.Get("/api/capture", [cap](const httplib::Request&, httplib::Response& res) {
    send_json(res, cap->status());
  });

  svr.Put("/api/capture", [cap](const httplib::Request& req, httplib::Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("running") ||
        !body["running"].is_boolean()) {
      return send_error(res, 400, "body must be {\"running\":true|false}");
    }
    std::string err;
    if (!cap->set_running(body["running"].get<bool>(), &err)) return send_error(res, 500, err);
    send_json(res, cap->status());
  });

  svr.Get(R"(/api/capture/files/([^/]+)/analyze)",
          [cap](const httplib::Request& req, httplib::Response& res) {
            const std::string name = req.matches[1];
            if (!Capture::valid_name(name)) return send_error(res, 400, "invalid capture name");
            if (!cap->exists(name)) return send_error(res, 404, "no such capture");
            const CommandResult r = cap->analyze(name);
            if (!r.started || (r.exit_code == 127 && r.output.empty())) {
              return send_error(res, 500, r.error.empty() ? "btmon not available" : r.error);
            }
            std::string text = r.output;
            if (r.truncated) text += "\n[output truncated]\n";
            if (r.timed_out) {
              text += "\n[btmon -a timed out]\n";
              res.status = 504;
            }
            res.set_content(text, "text/plain; charset=utf-8");
          });

  svr.Get(R"(/api/capture/files/([^/]+))",
          [cap](const httplib::Request& req, httplib::Response& res) {
            const std::string name = req.matches[1];
            if (!Capture::valid_name(name)) return send_error(res, 400, "invalid capture name");
            const std::string path = cap->path(name);
            struct stat st {};
            if (!cap->exists(name) || stat(path.c_str(), &st) != 0) {
              return send_error(res, 404, "no such capture");
            }
            std::shared_ptr<FILE> f(std::fopen(path.c_str(), "rbe"), [](FILE* p) {
              if (p) std::fclose(p);
            });
            if (!f) return send_error(res, 500, "cannot open " + name);
            // The size is fixed when the download starts: the active file keeps growing, and its
            // first N bytes are a valid capture (a cut last record reads as end of file).
            res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
            res.set_content_provider(
                static_cast<size_t>(st.st_size), "application/octet-stream",
                [f](size_t offset, size_t length, httplib::DataSink& sink) {
                  char buf[64 * 1024];
                  if (fseeko(f.get(), static_cast<off_t>(offset), SEEK_SET) != 0) return false;
                  const size_t n = std::fread(buf, 1, std::min(length, sizeof(buf)), f.get());
                  if (n == 0) return false;
                  return sink.write(buf, n);
                });
          });

  svr.Delete(R"(/api/capture/files/([^/]+))",
             [cap](const httplib::Request& req, httplib::Response& res) {
               const std::string name = req.matches[1];
               if (!Capture::valid_name(name)) return send_error(res, 400, "invalid capture name");
               if (!cap->exists(name)) return send_error(res, 404, "no such capture");
               if (cap->is_active(name)) {
                 return send_error(res, 409, "btmon is writing this file; stop the capture first");
               }
               if (unlink(cap->path(name).c_str()) != 0) {
                 return send_error(res, 500, std::string("unlink: ") + std::strerror(errno));
               }
               send_json(res, json{{"ok", true}});
             });
}

}  // namespace btb::hci
