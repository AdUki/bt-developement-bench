#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "adv.h"
#include "audio/engine.h"
#include "audio/pwtop.h"
#include "bench_player.h"
#include "obex/obex.h"
#include "bluetooth.h"
#include "dbus_log.h"
#include "gatt_server.h"
#include "hid_device.h"
#include "hid_host.h"
#include "jobs.h"
#include "journal.h"
#include "scripts.h"
#include "setup.h"
#include "system.h"
#include "ws_hub.h"

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace btb {

struct WebOptions {
  std::string www_dir = "/usr/share/btbenchd/www";
  std::string host = "0.0.0.0";
  int port = 80;
  // Reboot and the bluetooth/service restarts act on the machine; off on a PC.
  bool allow_power = true;
  // btbench-logd's files (GET /api/system/logs).
  std::string logs_dir = "/tmp/logs";
};

// What the routes work on. Owned by main(), alive for as long as the server is.
struct Deps {
  BtManager& bt;
  AdvManager& adv;
  GattServer& gatt_server;
  Jobs& jobs;
  JournalFollower& journal;
  TargetScripts& scripts;
  SystemInfo& sys;
  WsHub& hub;
  audio::AudioEngine& audio;
  audio::PwTop& pwtop;
  obex::ObexManager& obex;
  HidRemote& hid_remote;
  HidHost& hid_host;
  Setup& setup;
  BenchPlayer& player;
  DbusLog& dbus_log;
};

// Route installers, one per file (api_*.cpp).
void install_bluetooth_routes(httplib::Server& svr, Deps& d);
void install_le_routes(httplib::Server& svr, Deps& d);
void install_le_audio_routes(httplib::Server& svr, Deps& d);
void install_system_routes(httplib::Server& svr, Deps& d, const WebOptions& opt);
void install_audio_routes(httplib::Server& svr, Deps& d);
void install_obex_routes(httplib::Server& svr, Deps& d);
void install_hid_routes(httplib::Server& svr, Deps& d);
void install_setup_routes(httplib::Server& svr, Deps& d);

class WebServer {
 public:
  WebServer(Deps deps, WebOptions opt);
  ~WebServer();

  // For modules that add their own routes (the HCI monitor) before start().
  httplib::Server& server() { return *svr_; }

  bool start();  // binds and serves on the calling thread until stop()
  void stop();

  // The "bt" and "media" topics, from BtManager's change hook (on the bus thread).
  void publish_bt(bool request_changed);

 private:
  void install_routes();
  void run_sampler();
  bool captive_redirect(const httplib::Request& req, httplib::Response& res);

  Deps d_;
  WebOptions opt_;
  std::unique_ptr<httplib::Server> svr_;
  std::thread sampler_;
  std::atomic<bool> running_{false};

  std::mutex pub_m_;
  std::string last_bt_;
  std::string last_media_;
};

}  // namespace btb
