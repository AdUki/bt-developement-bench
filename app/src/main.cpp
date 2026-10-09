#include <CLI11.hpp>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "adv.h"
#include "audio/engine.h"
#include "bluetooth.h"
#include "bluez_model.h"
#include "gatt_server.h"
#include "jobs.h"
#include "journal.h"
#include "scripts.h"
#include "system.h"
#include "util/log.h"
#include "util/strings.h"
#include "uuids.h"
#include "webserver.h"
#include "ws_hub.h"

#ifdef BTB_HAVE_HCI
#include "hci/hci_monitor.h"
#endif

namespace {

btb::WebServer* g_server = nullptr;
std::atomic<bool> g_stopping{false};

void on_signal(int) {
  g_stopping.store(true);
  if (g_server) g_server->stop();
}

// Bound how long a stop can take. Each WebSocket connection has a reader thread parked in a
// blocking recv (WsReadPump in webserver.cpp), and a live pcap stream a writer; a client still
// attached when we are asked to stop leaves those blocked until the socket's timeout. The daemon
// holds nothing a teardown must flush (bluetoothd.env is renamed into place, BlueZ drops our
// registrations with our bus name), so once a stop is requested give the clean path a moment and
// then hard-exit. Without this, a reboot from the console would wait for systemd's SIGKILL.
void start_shutdown_watchdog() {
  std::thread([] {
    using namespace std::chrono_literals;
    while (!g_stopping.load()) std::this_thread::sleep_for(100ms);
    std::this_thread::sleep_for(3s);
    std::_Exit(0);
  }).detach();
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"btbenchd — the Bluetooth development bench's daemon and web console"};

  int port = 80;
  std::string host = "0.0.0.0";
  std::string www = "/usr/share/btbenchd/www";
  std::string data_dir = "/data/btbench";
  std::string capture_dir = "/data/btsnoop";
  std::string scripts_dir;
  std::string bt_adapter;
  std::string hci_replay;
  bool no_bluetooth = false;
  bool pc = false;
  bool verbose = false;

  app.add_option("--port", port, "HTTP port (default 80)");
  app.add_option("--host", host, "Address to listen on (default all)");
  app.add_option("--www", www, "Directory of the web console's files");
  app.add_option("--data-dir", data_dir, "Where the bench keeps its state: bluetoothd.env (default /data/btbench)");
  app.add_option("--capture-dir", capture_dir, "The btsnoop ring the capture routes manage (default /data/btsnoop)");
  app.add_option("--scripts-dir", scripts_dir,
                 "Also look here for btbench-audio/-wifi/-kernel (default: PATH only)");
  app.add_option("--bt-adapter", bt_adapter,
                 "The adapter to run, as hciN or its address, on a machine whose other adapters are "
                 "not the bench's (a PC). Only its devices are listed and the agent answers only for it");
  app.add_flag("--no-bluetooth", no_bluetooth, "Leave BlueZ alone: no D-Bus, no agent");
  app.add_option("--hci-replay", hci_replay,
                 "Feed the HCI monitor from a btsnoop capture, paced in real time, instead of the "
                 "kernel's monitor socket");
  app.add_flag("--pc", pc, "Running on a workstation: no reboot or service restarts from the API");
  app.add_flag("-v,--verbose", verbose, "Debug logging");
  CLI11_PARSE(app, argc, argv);

  btb::init_logging(verbose);

  // The UUID names the console shows come from the same file the console reads.
  std::string err;
  if (!btb::uuid_names().load(www + "/uuids.json", &err))
    LOG_WARN("no UUID names: {} (the API shows bare UUIDs)", err);

  btb::WsHub hub;
  // Every module publishes through this: serialized only when someone is subscribed.
  auto publish = [&hub](const std::string& topic, const nlohmann::json& data) {
    if (hub.has_subscribers(topic)) hub.publish(topic, data);
  };

  btb::BtManager bt;
  bt.set_adapter(bt_adapter);
  btb::AdvManager adv(bt);
  btb::GattServer gatt_server(bt, [&publish](const nlohmann::json& ev) { publish("gatt.server", ev); });
  bt.add_user(&adv);
  bt.add_user(&gatt_server);
  // A characteristic's value changes (notifications, indications, reads) go out on gatt.notify,
  // with what a console needs to place them: the device, the handle and the UUID.
  bt.on_notify([&bt, &hub](const std::string& path, const std::vector<uint8_t>& value) {
    if (!hub.has_subscribers("gatt.notify")) return;
    const auto objs = bt.objects();
    std::string uuid;
    uint16_t handle = 0;
    if (objs->contains(path) && (*objs)[path].contains("org.bluez.GattCharacteristic1")) {
      const auto& c = (*objs)[path]["org.bluez.GattCharacteristic1"];
      uuid = btb::uuid_short(btb::str_of(c, "UUID"));
      handle = btb::gatt_handle(path, c);
    } else {
      handle = btb::gatt_handle(path, nlohmann::json::object());
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    hub.publish("gatt.notify", nlohmann::json{{"address", btb::address_from_path(path)},
                                              {"path", path},
                                              {"handle", handle},
                                              {"uuid", uuid},
                                              {"value", btb::to_hex(value)},
                                              {"text", btb::printable(value)},
                                              {"ts", now}});
  });

  btb::Jobs jobs(publish);
  btb::JournalFollower journal([&hub] { return hub.has_subscribers("journal"); },
                               [&hub](const nlohmann::json& e) { hub.publish("journal", e); });
  btb::TargetScripts scripts(scripts_dir);
  btb::SystemInfo sys(data_dir);

  btb::WebOptions wopt;
  wopt.www_dir = www;
  wopt.host = host;
  wopt.port = port;
  wopt.allow_power = !pc;

  // The audio streams. Without PipeWire, the BlueZ transports say which BlueALSA PCMs exist.
  btb::audio::AudioEngine audio(publish, [&hub](const std::string& t) { return hub.has_subscribers(t); },
                                data_dir, [&bt] { return btb::model_media(*bt.objects()); });

  btb::Deps deps{bt, adv, gatt_server, jobs, journal, scripts, sys, hub, audio};
  btb::WebServer server(deps, wopt);
  bt.on_change([&server](bool request) { server.publish_bt(request); });

#ifdef BTB_HAVE_HCI
  btb::hci::Options hopt;
  hopt.replay_file = hci_replay;
  hopt.capture_dir = capture_dir;
  // It says itself, once, when its source cannot be opened (no CAP_NET_RAW on a PC).
  std::unique_ptr<btb::hci::Monitor> mon = btb::hci::start(hopt, publish);
  btb::hci::register_routes(server.server(), *mon);
#else
  if (!hci_replay.empty()) LOG_WARN("--hci-replay: this build has no HCI monitor (app/src/hci)");
  (void)capture_dir;
#endif

  if (no_bluetooth) {
    bt.set_not_running_reason("started with --no-bluetooth");
  } else {
    bt.start();
  }
  journal.start();

  g_server = &server;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
  start_shutdown_watchdog();

  const bool ok = server.start();

  LOG_INFO("shutting down");
  g_stopping.store(true);
  g_server = nullptr;
  journal.stop();
  jobs.stop_all();
  audio.stop_all();
#ifdef BTB_HAVE_HCI
  mon->stop();
#endif
  bt.stop();
  return ok ? 0 : 1;
}
