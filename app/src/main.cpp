#include <CLI11.hpp>
#include <linux/input.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "adv.h"
#include "audio/engine.h"
#include "obex/obex.h"
#include "bluetooth.h"
#include "bluez_model.h"
#include "gatt_server.h"
#include "hid_device.h"
#include "hid_host.h"
#include "jobs.h"
#include "journal.h"
#include "scripts.h"
#include "setup.h"
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
  // Settings on the SD card (/data), what changes often in RAM: the card is otherwise read-only
  // (docs/system.md "The SD card").
  std::string capture_dir = "/tmp/btsnoop";
  std::string volatile_dir = "/tmp/btbench";
  std::string scripts_dir;
  std::string bt_adapter;
  std::string hci_replay;
  bool no_bluetooth = false;
  bool pc = false;
  std::string logs_dir = "/tmp/logs";
  bool verbose = false;

  app.add_option("--port", port, "HTTP port (default 80)");
  app.add_option("--host", host, "Address to listen on (default all)");
  app.add_option("--www", www, "Directory of the web console's files");
  app.add_option("--data-dir", data_dir, "Where the bench keeps its state: bluetoothd.env (default /data/btbench)");
  app.add_option("--capture-dir", capture_dir, "The btsnoop ring the capture routes manage (default /tmp/btsnoop)");
  app.add_option("--volatile-dir", volatile_dir,
                 "Where what changes often and need not survive a reboot goes: OBEX transfers (default /tmp/btbench)");
  app.add_option("--logs-dir", logs_dir, "btbench-logd's per-program log files, served by the API (default /tmp/logs)");
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
  bt.set_default_alias(!pc);
  bt.set_state_file(data_dir + "/bluetooth.json");
  btb::AdvManager adv(bt);
  btb::GattServer gatt_server(bt, [&publish](const nlohmann::json& ev) { publish("gatt.server", ev); });
  // The HID remote is a second GATT application with its own root, so the console's GATT server
  // and the remote can be up at the same time.
  btb::GattServer hid_gatt(bt, [&publish](const nlohmann::json& ev) { publish("gatt.server", ev); }, "/org/btbench/hid");
  btb::HidRemote hid_remote(bt, adv, hid_gatt, data_dir + "/hid-remote.json");
  bt.add_user(&adv);
  bt.add_user(&gatt_server);
  bt.add_user(&hid_gatt);
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
                                              {"text", btb::gatt_text(uuid, value)},
                                              {"ts", now}});
  });

  btb::Jobs jobs(publish);
  btb::JournalFollower journal([&hub] { return hub.has_subscribers("journal"); },
                               [&hub](const nlohmann::json& e) { hub.publish("journal", e); });
  btb::audio::PwTop pwtop([&hub] { return hub.has_subscribers("pipewire"); },
                          [&hub](const nlohmann::json& t) { hub.publish("pipewire", t); });
  btb::TargetScripts scripts(scripts_dir);
  btb::SystemInfo sys(data_dir);
  // BlueZ's D-Bus traffic, watched from a monitor connection and written beside btbench-logd's
  // files; not on a PC without BT=fake, whose system bus is the desktop's.
  btb::DbusLog::Options dlopt;
  dlopt.file = logs_dir + "/bluez-dbus.log";
  btb::DbusLog dbus_log(dlopt, [&hub] { return hub.has_subscribers("bluez.dbus"); },
                        [&hub](const nlohmann::json& b) { hub.publish("bluez.dbus", b); });

  btb::WebOptions wopt;
  wopt.www_dir = www;
  wopt.host = host;
  wopt.port = port;
  wopt.allow_power = !pc;
  wopt.logs_dir = logs_dir;

  // The audio streams: PipeWire's graph when it runs, the loopback card when it does not.
  btb::audio::AudioEngine audio(publish, [&hub](const std::string& t) { return hub.has_subscribers(t); },
                                data_dir);

  // OBEX (obexd on the system bus), started on first use; its files live under the data dir.
  btb::obex::Options oopt;
  oopt.root = volatile_dir + "/obex";
  oopt.state_file = data_dir + "/obex.json";
  btb::obex::ObexManager obex(oopt, publish, [&hub](const std::string& t) { return hub.has_subscribers(t); });

  // The HID host side: Bluetooth input devices, their events, and their media keys driving the
  // board's media — the connected phone's AVRCP player (the playing one first) and the volume of
  // its transport, as a remote paired with a speaker would.
  btb::HidHostOptions hhopt;
  hhopt.state_file = data_dir + "/hid.json";
  btb::HidHost hid_host(hhopt, publish, [&hub](const std::string& t) { return hub.has_subscribers(t); },
                        [&bt](const std::string& address, int code, int value) {
    if (value == 0) return;  // presses and auto-repeat; releases do nothing
    const nlohmann::json m = btb::model_media(*bt.objects());
    const nlohmann::json* player = nullptr;
    for (const auto& p : m["players"])
      if (!player || p.value("status", std::string{}) == "playing") player = &p;
    if (!player) return;
    const std::string path = player->value("path", std::string{});
    std::string method;
    switch (code) {
      case KEY_PLAYPAUSE: method = player->value("status", std::string{}) == "playing" ? "Pause" : "Play"; break;
      case KEY_PLAYCD: case KEY_PLAY: method = "Play"; break;
      case KEY_PAUSECD: method = "Pause"; break;
      case KEY_STOPCD: method = "Stop"; break;
      case KEY_NEXTSONG: method = "Next"; break;
      case KEY_PREVIOUSSONG: method = "Previous"; break;
      case KEY_VOLUMEUP: case KEY_VOLUMEDOWN: {
        const std::string tp = player->value("transport", std::string{});
        for (const auto& t : m["transports"]) {
          if (t.value("path", std::string{}) != tp || !t.contains("volume") || t["volume"].is_null()) continue;
          // AVRCP absolute volume is 0..127: sixteen presses from silent to full.
          const int v = std::clamp(t["volume"].get<int>() + (code == KEY_VOLUMEUP ? 8 : -8), 0, 127);
          bt.set_property(tp, "org.bluez.MediaTransport1", "Volume", btb::DVar::u16(static_cast<uint16_t>(v)), 3000);
        }
        return;
      }
      default: return;
    }
    if (value == 2) return;  // a held play key does not toggle again
    LOG_INFO("hid host: {} from {} → {} {}", btb::evdev_key_name(code), address, method, path);
    bt.call(path, "org.bluez.MediaPlayer1", method, nullptr, 3000);
  });

  // Setup: profiles, plugins, main.conf and WirePlumber settings. On a PC its files go to the
  // data directory and nothing is restarted: the desktop's stack is not the bench's.
  btb::SetupOptions sopt;
  sopt.data_dir = data_dir;
  sopt.apply = !pc;
  // On the board /etc/wireplumber/wireplumber.conf.d/60-btbench-profiles.conf is a link to this
  // (btbench-base): the root filesystem is read-only, the fragment is a setting.
  sopt.wireplumber_conf = data_dir + "/wireplumber-profiles.conf";
  // What there is to set follows the installed stack: main.conf's keys, the kernel's experimental
  // features, bluetoothd's plugins, PipeWire's codecs, WirePlumber's schema.
  sopt.sources = btb::setup_sources_here();
  btb::Setup setup(sopt, [&hid_remote] { return hid_remote.state().value("enabled", false); },
                   [&hid_remote](bool on, std::string* err) {
                     // Already so: nothing to do (registering again would only be refused).
                     if (hid_remote.state().value("enabled", false) == on) return true;
                     if (!on) {
                       hid_remote.disable();
                       return true;
                     }
                     return hid_remote.enable(hid_remote.state().value("name", std::string{}), err);
                   },
                   [&obex](const std::string& noplugin) { obex.set_noplugin(noplugin); });
  obex.set_noplugin(setup.obex_noplugin());

  // The board's own AVRCP player, for what it plays to (a car, a speaker): its library plays
  // through the audio streams.
  btb::BenchPlayer player(bt, audio, data_dir + "/player.json");
  bt.add_user(&player);

  btb::Deps deps{bt, adv, gatt_server, jobs, journal, scripts, sys, hub, audio, pwtop, obex, hid_remote, hid_host, setup,
                 player, dbus_log};
  btb::WebServer server(deps, wopt);
  bt.on_change([&server, &hid_host](bool request) {
    server.publish_bt(request);
    hid_host.poke();
  });

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
  pwtop.start();
  if (!no_bluetooth) dbus_log.start();

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
  pwtop.stop();
  dbus_log.stop();
  jobs.stop_all();
  audio.stop_all();
  obex.stop();
#ifdef BTB_HAVE_HCI
  mon->stop();
#endif
  bt.stop();
  return ok ? 0 : 1;
}
