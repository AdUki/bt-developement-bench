#include "webserver.h"

#include <httplib.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include "bluez_model.h"
#include "http_util.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/sysinfo.h"

using json = nlohmann::json;

namespace btb {

namespace {

// The pool's floor. The Zero W has one core and 512 MB: three idle workers serve the console's
// polling. Long-lived connections (a WebSocket, a live pcap, a GATT read waiting on the air) each
// hold a worker for as long as they last, so the pool grows on demand to the ceiling and shrinks
// back when they end, rather than letting a second browser tab starve the first.
constexpr size_t kThreadsMin = 3;
constexpr size_t kThreadsMax = 24;

// The push-only telemetry handler only ever *sends*. A send-only httplib WebSocket loop never sees
// the browser's Close frame: read() is what processes it (flips is_open(), echoes it so the browser
// can tear the TCP down) and what answers pings. So this pump gives the connection its reader: one
// side thread that drains inbound frames — here also the client's subscription changes,
// {"topics":[...]} — so a closed tab is noticed within a frame. It is the only reader and never
// races the sender. At shutdown its blocking recv only unwinds on the socket's timeout, so main()
// hard-exits a moment after a stop is requested rather than stalling a reboot on this join.
class WsReadPump {
 public:
  WsReadPump(httplib::ws::WebSocket& ws, WsHub& hub, WsHub::ClientPtr c) {
    thread_ = std::thread([&ws, &hub, c] {
      std::string msg;
      while (ws.read(msg) != httplib::ws::Fail) {
        try {
          const json j = json::parse(msg);
          if (j.contains("topics") && j["topics"].is_array()) {
            std::vector<std::string> t;
            for (const json& x : j["topics"])
              if (x.is_string()) t.push_back(x.get<std::string>());
            hub.set_topics(c, std::move(t));
          }
        } catch (const std::exception&) {
          // Anything else a client sends is ignored: the socket is for pushing.
        }
      }
    });
  }
  ~WsReadPump() {
    if (thread_.joinable()) thread_.join();
  }
  WsReadPump(const WsReadPump&) = delete;
  WsReadPump& operator=(const WsReadPump&) = delete;

 private:
  std::thread thread_;
};

std::string read_file(const std::string& path, bool* ok) {
  std::ifstream f(path, std::ios::binary);
  *ok = static_cast<bool>(f);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Whether the Wi-Fi is in its setup-AP mode right now, from the state file btbench-wifi keeps
// (docs/contracts.md: /run/btbench/wifi.json). Read at most every two seconds: it is asked on
// every request while there is no answer cached.
bool wifi_is_ap() {
  static std::mutex m;
  static auto at = std::chrono::steady_clock::time_point{};
  static bool ap = false;
  std::lock_guard<std::mutex> lk(m);
  const auto now = std::chrono::steady_clock::now();
  if (now - at < std::chrono::seconds(2)) return ap;
  at = now;
  bool ok = false;
  const std::string s = read_file("/run/btbench/wifi.json", &ok);
  ap = false;
  if (ok) {
    try {
      ap = json::parse(s).value("state", std::string{}) == "ap";
    } catch (const std::exception&) {
    }
  }
  return ap;
}

}  // namespace

WebServer::WebServer(Deps deps, WebOptions opt) : d_(deps), opt_(std::move(opt)) {
  svr_ = std::make_unique<httplib::Server>();
  install_routes();
}

WebServer::~WebServer() { stop(); }

// In AP mode the board is a captive portal: dnsmasq answers every name with the board, so a phone
// checking for internet (connectivitycheck.gstatic.com, captive.apple.com) lands here. Any page
// asked for under a host name that is not the board's own goes to /wifi, which is what makes the
// phone pop the page up. Outside AP mode nothing is redirected: someone reaching the board under a
// lab DNS name must get the console.
bool WebServer::captive_redirect(const httplib::Request& req, httplib::Response& res) {
  // /wifi and what it loads are served whatever the Host: the captive sheet must get its page.
  if (req.method != "GET" || starts_with(req.path, "/api") || req.path == "/wifi" ||
      ends_with(req.path, ".js") || ends_with(req.path, ".css") || ends_with(req.path, ".json"))
    return false;
  if (!wifi_is_ap()) return false;
  std::string host = req.get_header_value("Host");
  if (host.empty()) return false;
  if (host[0] == '[') return false;  // an IPv6 literal: someone typed it, so it is the board
  const size_t colon = host.find(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  host = lower(host);
  char hn[256] = {0};
  gethostname(hn, sizeof(hn) - 1);
  const std::string me = lower(hn);
  if (host == "localhost" || host == me || host == me + ".local") return false;
  for (const std::string& ip : local_ipv4())
    if (host == ip) return false;
  const std::string port = opt_.port == 80 ? "" : ":" + std::to_string(opt_.port);
  res.set_redirect("http://" + req.local_addr + port + "/wifi", 302);
  return true;
}

void WebServer::install_routes() {
  httplib::Server& svr = *svr_;

  svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    return captive_redirect(req, res) ? httplib::Server::HandlerResponse::Handled
                                      : httplib::Server::HandlerResponse::Unhandled;
  });

  svr.set_mount_point("/", opt_.www_dir);
  // The console is replaced on disk (deploy-www) while browsers still hold the last copy, and a
  // cached app.js against a newer index.html, or a newer daemon, breaks the page. With no-cache
  // every load revalidates against the ETag httplib sends: a 304 when nothing changed.
  svr.set_file_request_handler([](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache");
  });

  // GET /api: docs/api.md rendered at build time (www/api.html). Read per request, like the rest
  // of www, so a deploy-www takes effect without a restart.
  auto page = [this](const char* file) {
    return [this, file](const httplib::Request&, httplib::Response& res) {
      bool ok = false;
      const std::string body = read_file(opt_.www_dir + "/" + file, &ok);
      if (!ok) return send_error(res, 404, std::string(file) + " not found");
      res.set_header("Cache-Control", "no-cache");
      res.set_content(body, "text/html; charset=utf-8");
    };
  };
  svr.Get("/api", page("api.html"));
  svr.Get("/api/", page("api.html"));
  // The Wi-Fi page on its own: what a phone's captive-portal sheet shows in AP mode.
  svr.Get("/wifi", page("wifi.html"));

  install_bluetooth_routes(svr, d_);
  install_le_routes(svr, d_);
  install_system_routes(svr, d_, opt_);
  install_audio_routes(svr, d_);

  // /api/ws?topics=bt,hci.stats — no topics parameter subscribes to everything.
  svr.WebSocket("/api/ws", [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
    std::vector<std::string> topics =
        req.has_param("topics") ? WsHub::parse_topics(req.get_param_value("topics")) : std::vector<std::string>{"*"};
    auto client = d_.hub.add(topics);
    WsReadPump pump(ws, d_.hub, client);
    LOG_DEBUG("ws client connected ({} total)", d_.hub.clients());
    // A newcomer gets the current state of what it asked for at once, not at the next change
    // (which, for a board sitting idle, may be a long time coming).
    auto wants = [&topics](const char* t) {
      for (const std::string& p : topics)
        if (WsHub::topic_matches(p, t)) return true;
      return false;
    };
    auto greet = [&ws](const char* topic, const json& data) {
      ws.send(json{{"topic", topic}, {"data", data}}.dump(-1, ' ', false, json::error_handler_t::replace));
    };
    if (wants("bt")) greet("bt", d_.bt.state());
    if (wants("media")) greet("media", model_media(*d_.bt.objects()));
    if (wants("bt.request")) {
      BtRequest r;
      greet("bt.request", d_.bt.has_request(&r) ? bt_request_json(r) : json(nullptr));
    }
    while (running_.load() && ws.is_open()) {
      WsMessagePtr m = d_.hub.wait(client, 250);
      if (!m) continue;
      if (!ws.send(*m)) break;
    }
    d_.hub.remove(client);
    LOG_DEBUG("ws client gone ({} left)", d_.hub.clients());
  });
}

void WebServer::publish_bt(bool request_changed) {
  std::lock_guard<std::mutex> lk(pub_m_);
  if (d_.hub.has_subscribers("bt")) {
    const json s = d_.bt.state();
    const std::string dumped = s.dump(-1, ' ', false, json::error_handler_t::replace);
    // Every re-read of BlueZ's tree calls this; only a change goes out.
    if (dumped != last_bt_ || request_changed) {
      last_bt_ = dumped;
      d_.hub.publish("bt", s);
    }
  } else {
    last_bt_.clear();
  }
  if (request_changed && d_.hub.has_subscribers("bt.request")) {
    BtRequest r;
    d_.hub.publish("bt.request", d_.bt.has_request(&r) ? bt_request_json(r) : json(nullptr));
  }
  if (d_.hub.has_subscribers("media")) {
    const json m = model_media(*d_.bt.objects());
    const std::string dumped = m.dump();
    if (dumped != last_media_) {
      last_media_ = dumped;
      d_.hub.publish("media", m);
    }
  } else {
    last_media_.clear();
  }
}

// Host health at 1 Hz. The CPU figure is a delta and has exactly one owner, this thread; the
// System page and GET /api/system read what it last stored.
void WebServer::run_sampler() {
  CpuSampler cpu;
  auto next = std::chrono::steady_clock::now();
  while (running_.load()) {
    next += std::chrono::seconds(1);
    SysInfo s = sample_sysinfo();
    s.cpu_pct = cpu.sample();
    d_.sys.store_sample(s);
    if (d_.hub.has_subscribers("system")) d_.hub.publish("system", sysinfo_json(s));
    // Slept in short steps so a stop is quick.
    while (running_.load() && std::chrono::steady_clock::now() < next)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

bool WebServer::start() {
  running_.store(true);
  svr_->new_task_queue = [] { return new httplib::ThreadPool(kThreadsMin, kThreadsMax); };
  // Long-lived streams (a WebSocket, a live pcap) must not be reaped by an idle timeout.
  svr_->set_read_timeout(3600, 0);
  svr_->set_write_timeout(3600, 0);
  sampler_ = std::thread([this] { run_sampler(); });

  LOG_INFO("serving http://{}:{} from {}", opt_.host, opt_.port, opt_.www_dir);
  const bool ok = svr_->listen(opt_.host.c_str(), opt_.port);
  if (!ok) LOG_ERROR("cannot bind {}:{}", opt_.host, opt_.port);

  running_.store(false);
  d_.hub.shutdown();
  if (sampler_.joinable()) sampler_.join();
  return ok;
}

void WebServer::stop() {
  running_.store(false);
  d_.hub.shutdown();
  if (svr_) svr_->stop();
  if (sampler_.joinable()) sampler_.join();
}

}  // namespace btb
